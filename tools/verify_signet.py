#!/usr/bin/env python3
"""Integration checks against a running isolated signet Core.

--exercise-reorg briefly invalidates/reconsiders the tip and disables networking.
Use it ONLY with the disposable project node. The prior network state is restored.
"""

import argparse
import hashlib
import io
import json
import time
from pathlib import Path

from bitcoin_wizard_cave.capture import CaptureReader
from bitcoin_wizard_cave.core import RPC, SequenceFeed
from bitcoin_wizard_cave.observer import Observer, writer_lock
from bitcoin_wizard_cave.render import Renderer
from bitcoin_wizard_cave.storage import Store


def digest(store):
    value = hashlib.sha256()
    for row in store.db.execute("SELECT id,data FROM coins ORDER BY id"):
        value.update((row[0] + row[1]).encode())
    return value.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--datadir", type=Path, required=True)
    parser.add_argument("--rpc-url", default="http://127.0.0.1:38332")
    parser.add_argument("--db", type=Path, required=True, help="must be a new verification database")
    parser.add_argument("--exercise-reorg", action="store_true")
    args = parser.parse_args()
    if args.db.exists(): parser.error("--db must be new")
    rpc = RPC(args.rpc_url, args.datadir / "signet" / ".cookie")
    active = rpc.call("getnetworkinfo")["networkactive"]
    invalidated = None
    feed = None
    with writer_lock(args.db):
        store = Store(args.db)
        try:
            if args.exercise_reorg: rpc.call("setnetworkactive", False)
            observer = Observer(store, rpc, bootstrap=2)
            observer.identify()
            feed = SequenceFeed(observer.address)
            feed.start()
            observer.feed = feed
            # PUB/SUB has no replay barrier. This bounded handshake wait only
            # supports the controlled test; production still reconciles periodically.
            time.sleep(0.3)
            observer.reconcile("integration-baseline")
            with store.db: observer.model.peers(rpc.call("getpeerinfo"), store.get("node_run"))
            capture_dir = args.datadir / "signet" / "message_capture"
            capture_count = 0
            if capture_dir.is_dir():
                capture = CaptureReader(store, observer.model, capture_dir)
                for _ in range(4): capture_count += capture.poll()
            baseline = store.get("tip")
            before = digest(store)
            received = []
            notification_counts = {label: 0 for label in "ARCD"}
            def await_block(label, blockhash):
                deadline = time.monotonic() + 30
                while time.monotonic() < deadline:
                    notification = feed.queue.get(timeout=max(0.01, deadline - time.monotonic()))
                    notification_counts[notification.label] += 1
                    observer.handle(notification)
                    if notification.label == label and notification.object_hash == blockhash:
                        received.append({"label": label, "hash": blockhash, "counter": notification.counter})
                        return
                raise AssertionError("No matching block notification before deadline")
            if args.exercise_reorg:
                assert baseline["height"] > 1, "Need at least two downloaded signet blocks"
                invalidated = baseline["hash"]
                rpc.call("invalidateblock", invalidated)
                await_block("D", invalidated)
                assert store.get("tip")["height"] == baseline["height"] - 1
                rpc.call("reconsiderblock", invalidated)
                await_block("C", invalidated)
                assert store.get("tip") == baseline
                assert digest(store) == before, "UTXO membership/data changed after disconnect/reconnect"
                invalidated = None
            checked = 0
            for row in store.db.execute("SELECT id,data FROM coins ORDER BY id LIMIT 20").fetchall():
                txid, vout = row[0].split(":")
                actual = rpc.call("gettxout", txid, int(vout), False)
                assert actual is not None, row[0]
                from bitcoin_wizard_cave.model import sats
                assert sats(actual["value"]) == json.loads(row[1])["value_sat"]
                checked += 1
            modes = {}
            for mode in ("state", "protocol", "utxo"):
                output = io.StringIO()
                renderer = Renderer(args.db, mode, output, max_visible=5000)
                renderer.snapshot()
                renderer.db.close()
                lines = output.getvalue().splitlines()
                assert all(len(line.split("|")) == 5 and line.split("|")[2] == "A" for line in lines)
                modes[mode] = len(lines)
            assert modes["state"] > 0 and modes["utxo"] > 0
            if capture_count: assert modes["protocol"] > 0
            report = {"network": store.get("network"), "core_version": store.get("core_version"),
                      "applied_tip": store.get("tip"), "initialblockdownload": rpc.call("getblockchaininfo")["initialblockdownload"],
                      "zmq": observer.address, "notifications": received, "notification_counts": notification_counts, "coin_digest": before,
                      "tracked_coins": store.db.execute("SELECT count(*) FROM coins").fetchone()[0],
                      "gettxout_spot_checks": checked, "captured_messages": capture_count,
                      "rendered_leaves": modes}
            print(json.dumps(report, indent=2))
        finally:
            try:
                if invalidated: rpc.call("reconsiderblock", invalidated)
                if args.exercise_reorg: rpc.call("setnetworkactive", active)
            finally:
                if feed: feed.close()
                rpc.close()
                with store.db:
                    store.put("synchronized", False)
                    store.event("observer.stopped", cause="integration-check-exit")
                store.close()


if __name__ == "__main__": main()
