"""One model writer; bounded ingress, atomic state, periodic exact reconciliation."""

import contextlib
import os
import queue
import sys
import time
import uuid
from pathlib import Path

from .core import RPCError, SequenceFeed, endpoint
from .model import Model, RebuildRequired, fee_metadata

if os.name == "nt":
    import msvcrt
else:
    import fcntl


@contextlib.contextmanager
def writer_lock(database):
    path = Path(str(database) + ".writer.lock")
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a") as lock:
        try:
            if os.name == "nt":
                if path.stat().st_size == 0:
                    lock.write("\0")
                    lock.flush()
                lock.seek(0)
                msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            raise RuntimeError("An observer/ingester already owns this database") from None
        try:
            yield
        finally:
            if os.name == "nt":
                lock.seek(0)
                msvcrt.locking(lock.fileno(), msvcrt.LK_UNLCK, 1)


class Observer:
    def __init__(self, store, rpc, *, network="signet", address=None, window=144, undo_horizon=288,
                 bootstrap=6, metadata_budget=32, journal_keep=100_000):
        self.s, self.rpc = store, rpc
        self.model = Model(store, window, undo_horizon)
        self.network, self.address = network, address
        self.bootstrap = bootstrap
        self.metadata_budget = metadata_budget
        self.journal_keep = journal_keep
        self.counter = None
        self.cookie_marker = None
        self.feed = None
        self.metadata_cursor = 0
        self.last_drop = 0
        self.capture = None

    def identify(self):
        chain = self.rpc.call("getblockchaininfo")
        if chain["chain"] != self.network:
            raise ValueError(f"Expected {self.network}; Core reports {chain['chain']}")
        network = self.rpc.call("getnetworkinfo")
        genesis = self.rpc.call("getblockhash", 0)
        if self.s.get("genesis") not in (None, genesis):
            raise ValueError("Observer database belongs to a different chain; choose a new --db")
        with self.s.db:
            self.s.put("genesis", genesis)
            self.s.put("network", self.network)
            self.s.put("core_version", network["version"])
            self.s.put("node_run", uuid.uuid4().hex[:16])
            self.s.put("window_blocks", self.model.window)
            self.s.put("undo_horizon", self.model.undo_horizon)
            self.s.event("observer.session", "node:snapshot", "startup-or-node-restart", core_version=network["version"], network=self.network)
        self.cookie_marker = self.rpc.cookie_marker
        self.counter = None
        if not self.address:
            notifiers = self.rpc.call("getzmqnotifications")
            self.address = next((n["address"] for n in notifiers if n["type"] == "pubsequence"), None)
            if self.address is None: raise ValueError("Core has no sequence publisher; set zmqpubsequence=unix:/.../sequence.sock")
        self.address = endpoint(self.address)
        return chain

    def rebuild(self, chain, cause):
        height = max(chain.get("pruneheight", 0), chain["blocks"] - self.bootstrap)
        checkpoint = self.rpc.call("getblockhash", height)
        with self.s.db: self.model.checkpoint(checkpoint, height, cause)

    def sync_chain(self):
        chain = self.rpc.call("getblockchaininfo")
        if chain["chain"] != self.network: raise ValueError("Core changed network")
        with self.s.db:
            self.s.put("initialblockdownload", chain.get("initialblockdownload"))
        if self.s.get("tip") is None:
            self.rebuild(chain, "initial-observed-window")
        tip = self.s.get("tip")
        if self.model.window and chain["blocks"] - tip["height"] > max(self.model.window, self.bootstrap):
            # IBD/pruning may run much faster than presentation. This deliberately
            # narrows coverage and records a history gap; it is never a ledger spend.
            self.rebuild(chain, "catchup-exceeds-rolling-window")
        # Rewind using stored inverses until our applied tip is an active ancestor.
        while True:
            tip = self.s.get("tip")
            header = self.rpc.call("getblockheader", tip["hash"])
            if header.get("confirmations", -1) >= 0: break
            try:
                with self.s.db: self.model.disconnect(tip["hash"])
            except RebuildRequired as error:
                self.rebuild(chain, str(error))
                break
        tip = self.s.get("tip")
        for height in range(tip["height"] + 1, min(chain["blocks"], tip["height"] + 32) + 1):
            blockhash = self.rpc.call("getblockhash", height)
            try:
                block = self.rpc.call("getblock", blockhash, 2)
            except RPCError as error:
                if error.code in (-1, -5):
                    self.rebuild(chain, "pruned-or-unavailable-history")
                    raise RebuildRequired("Block content unavailable; rebuilt checkpoint, retry required") from error
                raise
            with self.s.db: self.model.connect(block)
        return self.s.get("tip")

    def reconcile(self, cause):
        self.rpc.call("uptime")
        if self.cookie_marker is not None and self.cookie_marker != self.rpc.cookie_marker:
            self.identify()
            if self.feed:
                self.feed.close()
                self.feed = SequenceFeed(self.address)
                self.feed.start()
                self.last_drop = 0
            cause = "node-restart"
        for _ in range(5):
            tip = self.sync_chain()
            before = self.rpc.call("getbestblockhash")
            snapshot = self.rpc.call("getrawmempool", False, True)
            after = self.rpc.call("getbestblockhash")
            if before != after or after != tip["hash"]: continue
            with self.s.db:
                self.model.reconcile_mempool(snapshot["txids"], snapshot["mempool_sequence"])
                self.s.put("last_reconcile_ns", time.time_ns())
                self.s.put("synchronized", True)
                self.s.event("observer.resync", "node:snapshot", cause,
                             tip=tip, mempool_count=len(snapshot["txids"]),
                             mempool_sequence=str(snapshot["mempool_sequence"]),
                             history_complete=False if cause != "periodic" else None)
            return
        raise RebuildRequired("Chain changed across five snapshot attempts")

    def metadata(self):
        txids = [r[0] for r in self.s.db.execute("SELECT txid FROM mempool ORDER BY txid")]
        if not txids: return
        for _ in range(min(self.metadata_budget, len(txids))):
            txid = txids[self.metadata_cursor % len(txids)]
            self.metadata_cursor += 1
            try:
                entry = self.rpc.call("getmempoolentry", txid)
                data = fee_metadata(entry)
                transaction = self.rpc.call("getrawtransaction", txid, True)
                # A second membership check avoids using content as an admission.
                with self.s.db:
                    self.model.enrich(txid, data)
                    self.model.pending(txid, transaction)
            except RPCError as error:
                if error.code not in (-5, -8): raise
                with self.s.db:
                    self.s.event("observer.metadata-missing", "node:metadata", "concurrent-membership-change", txid=txid)

    def handle(self, event):
        expected = None if self.counter is None else (self.counter + 1) & 0xFFFFFFFF
        self.counter = event.counter
        if expected is not None and expected != event.counter:
            self.reconcile("zmq-counter-gap-or-publisher-restart")
        source = {"kind": "zmq", "topic": "sequence", "notification_sequence": event.counter,
                  "mempool_sequence": str(event.mempool_sequence) if event.mempool_sequence is not None else None}
        if event.label in "CD":
            # Reconcile to Core's current tip; queued notifications may already be historical.
            self.reconcile("chain-notification")
            return
        boundary = self.s.get("mempool_boundary", 0)
        if event.mempool_sequence < boundary: return
        with self.s.db:
            if event.label == "A": self.model.admission(event.object_hash, source=source)
            else: self.model.removal(event.object_hash, source=source)
            self.s.put("mempool_boundary", event.mempool_sequence + 1)

    def run(self, stop, *, duration=None, peer_interval=2, reconcile_interval=15):
        self.identify()
        self.feed = SequenceFeed(self.address)
        self.feed.start()  # Start buffering before the first RPC snapshot.
        started = time.monotonic()
        next_peer = next_reconcile = next_metadata = started
        try:
            print(f"bitcoin-wizard-cave: {self.network}, Core {self.s.get('core_version')}, ZMQ {self.address}", file=sys.stderr)
            while not stop.is_set() and (duration is None or time.monotonic() - started < duration):
                now = time.monotonic()
                if self.feed.error: raise RuntimeError("ZMQ receiver failed: " + self.feed.error)
                try:
                    if now >= next_reconcile:
                        self.reconcile("periodic" if self.s.get("last_reconcile_ns") else "startup")
                        next_reconcile = now + reconcile_interval
                        with self.s.db: self.s.prune(self.journal_keep)
                    if self.feed.dropped != self.last_drop:
                        with self.s.db:
                            self.s.event("observer.source-loss", cause="ingress-overflow-or-malformed",
                                         dropped=self.feed.dropped - self.last_drop)
                        self.last_drop = self.feed.dropped
                        self.reconcile("ingress-loss")
                    for _ in range(128):
                        try: event = self.feed.queue.get_nowait()
                        except queue.Empty: break
                        self.handle(event)
                    if now >= next_peer:
                        peers = self.rpc.call("getpeerinfo")
                        with self.s.db:
                            self.model.peers(peers, self.s.get("node_run"), aggregate=self.capture is None)
                            self.s.put("protocol_fidelity", "message-capture" if self.capture else "aggregate-bytes")
                        if self.capture: self.capture.poll()
                        next_peer = now + peer_interval
                    if now >= next_metadata:
                        self.metadata()
                        next_metadata = now + peer_interval
                except (RPCError, OSError, RebuildRequired) as error:
                    self.rpc.close()
                    with self.s.db:
                        self.s.put("synchronized", False)
                        self.s.event("observer.source-loss", cause="rpc-or-chain-gap", error=str(error))
                    print(f"bitcoin-wizard-cave: recovery needed: {error}", file=sys.stderr)
                    next_reconcile = 0
                    stop.wait(min(2, peer_interval))
                stop.wait(0.05)
        finally:
            self.feed.close()
            self.rpc.close()
            with self.s.db:
                self.s.put("synchronized", False)
                self.s.event("observer.stopped", cause="collector-exit")
