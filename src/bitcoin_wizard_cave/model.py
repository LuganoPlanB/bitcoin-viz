"""State transitions and three projections. No RPC or renderer dependency."""

import hashlib
import json
import re
from decimal import Decimal

from .storage import encode


class RebuildRequired(RuntimeError):
    pass


def component(value):
    """Untrusted text cannot introduce Gource fields, lines or directories."""
    value = str(value)
    clean = re.sub(r"[^A-Za-z0-9_.:-]", "_", value)[:96]
    if clean in ("", ".", ".."): clean = "unknown"
    if clean != value:
        clean += "-" + hashlib.sha256(value.encode()).hexdigest()[:12]
    return clean


def sats(value):
    result = Decimal(str(value)) * 100_000_000
    if result != result.to_integral_value():
        raise ValueError("Non-integral satoshi amount")
    return int(result)


def fee_metadata(entry):
    fee = entry.get("fees", {}).get("base")
    size = entry.get("vsize_bip141")
    source = "vsize_bip141"
    if size is None and entry.get("weight") is not None:
        size = (int(entry["weight"]) + 3) // 4
        source = "ceil(weight/4)"
    # Historical 'vsize' may be sigop-adjusted; never silently call it BIP141.
    rate = Decimal(sats(fee)) / Decimal(size) if fee is not None and size else None
    band = "fee-unknown" if rate is None else (
        "fee-below-1" if rate < 1 else "fee-1-to-5" if rate < 5
        else "fee-5-to-20" if rate < 20 else "fee-20-plus")
    return {"fee_sat": sats(fee) if fee is not None else None,
            "vsize_bip141": size, "size_source": source if size else "unknown",
            "sat_per_vb": str(rate) if rate is not None else None,
            "band": band, "depends": entry.get("depends", []),
            "spentby": entry.get("spentby", []), "wtxid": entry.get("wtxid")}


def mempool_path(txid, data):
    return f"mempool/{data.get('band', 'fee-unknown')}/{txid}.tx"


def coin_path(coin):
    height = coin["height"]
    return f"utxo/{height//100*100}-{height//100*100+99}/{height}-{coin['block']}/{coin['id'].replace(':', '-')}.coin"


def block_path(height, blockhash, branch="recent"):
    return f"chain/{branch}/{height//100*100}-{height//100*100+99}/{height}-{blockhash}.block"


def traffic_category(command):
    if command in {"tx", "wtxidrelay", "sendtxrcncl"}: return "transactions"
    if command in {"block", "headers", "getheaders", "cmpctblock", "getblocktxn", "blocktxn", "sendcmpct", "sendheaders"}: return "blocks"
    if command in {"addr", "addrv2", "getaddr", "sendaddrv2"}: return "discovery"
    if command in {"version", "verack", "ping", "pong", "feefilter", "sendtxpackages"}: return "control"
    return "unclassified"  # inv/getdata require payload inspection.


class Model:
    def __init__(self, store, window=144, undo_horizon=288, recent=12):
        self.s = store
        self.db = store.db
        self.window = window
        self.undo_horizon = undo_horizon
        self.recent = recent

    def admission(self, txid, data=None, actor="node:mempool", cause="admission", source=None):
        old = self.db.execute("SELECT data FROM mempool WHERE txid=?", (txid,)).fetchone()
        if old:
            if data is not None: self.enrich(txid, data)
            return
        data = data or {"band": "fee-unknown"}
        self.db.execute("INSERT INTO mempool VALUES(?,?)", (txid, encode(data)))
        event = self.s.event("mempool.add", actor, cause, txid=txid, metadata=data, source=source)
        self.s.leaf("state", mempool_path(txid, data), actor, "E9B949", data, event)

    def enrich(self, txid, data):
        row = self.db.execute("SELECT data FROM mempool WHERE txid=?", (txid,)).fetchone()
        if row is None: return  # A late RPC response never recreates membership.
        old = json.loads(row[0])
        if old == data: return
        event = self.s.event("mempool.metadata", "node:metadata", txid=txid, metadata=data)
        if mempool_path(txid, old) != mempool_path(txid, data):
            self.s.leaf("state", mempool_path(txid, old), "node:metadata", "E9B949", old, event, delete=True)
        self.s.leaf("state", mempool_path(txid, data), "node:metadata", "E9B949", data, event)
        self.db.execute("UPDATE mempool SET data=? WHERE txid=?", (encode(data), txid))

    def removal(self, txid, cause="unknown-removal", actor="node:mempool", source=None):
        row = self.db.execute("SELECT data FROM mempool WHERE txid=?", (txid,)).fetchone()
        if row is None: return
        data = json.loads(row[0])
        event = self.s.event("mempool.remove", actor, cause, txid=txid, source=source)
        self.s.leaf("state", mempool_path(txid, data), actor, "E9B949", data, event, delete=True)
        for leaf in self.db.execute("SELECT path FROM leaves WHERE mode='utxo' AND path LIKE ?", (f"pending/{txid}/%",)).fetchall():
            self.s.leaf("utxo", leaf[0], actor, "DDA0DD", {}, event, delete=True)
        self.db.execute("DELETE FROM mempool WHERE txid=?", (txid,))

    def pending(self, txid, transaction):
        if not self.db.execute("SELECT 1 FROM mempool WHERE txid=?", (txid,)).fetchone(): return
        event = self.s.event("utxo.pending", "node:mempool", "pending-overlay", txid=txid)
        for vin in transaction.get("vin", []):
            if "txid" not in vin: continue
            outpoint = f"{vin['txid']}:{vin['vout']}"
            if self.db.execute("SELECT 1 FROM coins WHERE id=?", (outpoint,)).fetchone():
                self.s.leaf("utxo", f"pending/{txid}/{outpoint.replace(':', '-')}.pending", "node:mempool", "DDA0DD", {"outpoint": outpoint}, event)

    def reconcile_mempool(self, txids, boundary):
        current = {row[0] for row in self.db.execute("SELECT txid FROM mempool")}
        target = set(txids)
        for txid in sorted(current - target): self.removal(txid, "snapshot-difference", "node:snapshot")
        for txid in sorted(target - current): self.admission(txid, actor="node:snapshot", cause="snapshot-population")
        self.s.put("mempool_boundary", boundary)

    def checkpoint(self, blockhash, height, cause):
        event = self.s.event("observer.rebuild", "node:snapshot", cause, checkpoint=blockhash, height=height,
                             coverage="rolling-observed-outputs" if self.window else "fixed-observed-outputs")
        for mode in ("state", "utxo"):
            for row in self.db.execute("SELECT path FROM leaves WHERE mode=?", (mode,)).fetchall():
                if mode == "state" and not row[0].startswith("chain/"): continue
                self.s.leaf(mode, row[0], "node:snapshot", "999999", {}, event, delete=True)
        self.db.execute("DELETE FROM coins")
        self.db.execute("DELETE FROM blocks")
        self.s.put("tip", {"hash": blockhash, "height": height})
        self.s.put("checkpoint", {"hash": blockhash, "height": height})
        self.s.put("coverage_height", height + 1)

    def _coin(self, outpoint):
        row = self.db.execute("SELECT data FROM coins WHERE id=?", (outpoint,)).fetchone()
        return json.loads(row[0]) if row else None

    def _set_coin(self, outpoint, coin, actor, cause):
        old = self._coin(outpoint)
        event = self.s.event("utxo." + cause, actor, cause, outpoint=outpoint, previous=old, coin=coin)
        if old:
            self.s.leaf("utxo", coin_path(old), actor, "68B89B", old, event, delete=True)
        if coin is None:
            self.db.execute("DELETE FROM coins WHERE id=?", (outpoint,))
        else:
            self.db.execute("INSERT OR REPLACE INTO coins VALUES(?,?,?)", (outpoint, coin["height"], encode(coin)))
            self.s.leaf("utxo", coin_path(coin), actor, "68B89B", coin, event)

    def connect(self, block):
        tip = self.s.get("tip")
        if tip and tip["hash"] == block["hash"]: return
        if not tip or tip["hash"] != block.get("previousblockhash") or block["height"] != tip["height"] + 1:
            raise RebuildRequired("chain-parent-mismatch")
        height, blockhash = block["height"], block["hash"]
        undo = []
        untracked = 0
        for tx in block["tx"]:
            txid, actor = tx["txid"], "tx:" + tx["txid"]
            self.removal(txid, "confirmation", "node:chain")
            for vin in tx.get("vin", []):
                if "txid" not in vin: continue
                outpoint = f"{vin['txid']}:{vin['vout']}"
                old = self._coin(outpoint)
                if old:
                    undo.append([outpoint, old])
                    self._set_coin(outpoint, None, actor, "spend")
                else: untracked += 1
            for vout in tx.get("vout", []):
                script = vout.get("scriptPubKey", {}).get("hex", "")
                # CScript::IsUnspendable: OP_RETURN or script exceeding MAX_SCRIPT_SIZE.
                if height == 0 or script.startswith("6a") or len(script) > 20_000: continue
                outpoint = f"{txid}:{vout['n']}"
                coin = {"id": outpoint, "height": height, "block": blockhash,
                        "value_sat": sats(vout["value"]), "script": script,
                        "coinbase": any("coinbase" in i for i in tx.get("vin", []))}
                undo.append([outpoint, self._coin(outpoint)])
                self._set_coin(outpoint, coin, actor, "create")
        previous_coverage = self.s.get("coverage_height", height)
        coverage = max(previous_coverage, height - self.window + 1) if self.window else previous_coverage
        for row in self.db.execute("SELECT id,data FROM coins WHERE height<?", (coverage,)).fetchall():
            undo.append([row[0], json.loads(row[1])])
            self._set_coin(row[0], None, "view:retention", "coverage-expiry")
        self.db.execute("INSERT INTO blocks VALUES(?,?,?,?,?)", (blockhash, height, tip["hash"], encode(undo), previous_coverage))
        event = self.s.event("chain.connect", "node:chain", "connection", blockhash=blockhash, height=height,
                             header_time=block.get("time"), outside_window_inputs=untracked)
        self.s.leaf("state", block_path(height, blockhash), "node:chain", "7EA6E0", {"height": height}, event)
        self.s.leaf("state", block_path(height, blockhash, "disconnected"), "node:chain", "DE7777", {}, event, delete=True)
        # Retention is a presentation event, not a block disconnect.
        self._trim_chain(height)
        self.db.execute("DELETE FROM blocks WHERE height<=?", (height - self.undo_horizon,))
        self.s.put("tip", {"hash": blockhash, "height": height})
        self.s.put("coverage_height", coverage)

    def _trim_chain(self, height):
        rows = self.db.execute("SELECT path,data FROM leaves WHERE mode='state' AND path LIKE 'chain/%'").fetchall()
        for row in rows:
            if json.loads(row[1]).get("height", height) <= height - self.recent:
                event = self.s.event("view.expiry", "view:retention", "display-expiry", path=row[0])
                self.s.leaf("state", row[0], "view:retention", "999999", {}, event, delete=True)

    def disconnect(self, blockhash):
        tip = self.s.get("tip")
        if tip is None or tip["hash"] != blockhash: raise RebuildRequired("disconnect-tip-mismatch")
        row = self.db.execute("SELECT * FROM blocks WHERE hash=?", (blockhash,)).fetchone()
        if row is None: raise RebuildRequired("undo-horizon-or-checkpoint-crossed")
        actor = "reorg:" + blockhash
        for outpoint, previous in reversed(json.loads(row["undo"])):
            self._set_coin(outpoint, previous, actor, "undo-restore" if previous else "undo-creation")
        event = self.s.event("chain.disconnect", actor, "reorganization", blockhash=blockhash, height=row["height"])
        self.s.leaf("state", block_path(row["height"], blockhash), actor, "7EA6E0", {}, event, delete=True)
        self.s.leaf("state", block_path(row["height"], blockhash, "disconnected"), actor, "DE7777", {"height": row["height"]}, event)
        self.db.execute("DELETE FROM blocks WHERE hash=?", (blockhash,))
        self.s.put("tip", {"hash": row["parent"], "height": row["height"] - 1})
        self.s.put("coverage_height", row["coverage"])
        # Only A events / a snapshot may readmit disconnected transactions.

    def peers(self, peers, session, aggregate=True):
        current = {r[0]: json.loads(r[1]) for r in self.db.execute("SELECT id,data FROM peers")}
        seen = set()
        for peer in peers:
            key = f"{session}-peer{peer['id']}-{peer.get('conntime', 0)}"
            seen.add(key)
            role = "inbound" if peer.get("inbound") else component(peer.get("connection_type", "outbound"))
            path = f"network/{role}/{component(peer.get('network', 'unknown'))}/{key}.peer"
            data = {"path": path, "recv": peer.get("bytesrecv_per_msg", {}), "sent": peer.get("bytessent_per_msg", {})}
            old = current.get(key)
            if old is None or old["path"] != path:
                event = self.s.event("peer.snapshot" if old is None else "peer.metadata", "node:network", "poll", peer_session=key)
                if old:
                    for mode in ("state", "protocol"): self.s.leaf(mode, old["path"], "node:network", "B9A2DD", {}, event, delete=True)
                for mode in ("state", "protocol"): self.s.leaf(mode, path, "node:network", "B9A2DD", {"peer_session": key}, event)
            if old and aggregate:
                for field, direction in (("recv", "incoming"), ("sent", "outgoing")):
                    for command, total in data[field].items():
                        delta = int(total) - int(old[field].get(command, 0))
                        if delta < 0:
                            self.s.event("observer.source-loss", "node:network", "counter-reset", peer_session=key, command=command)
                        elif delta:
                            self.message(key, direction, command, delta, fidelity="aggregate-bytes")
            self.db.execute("INSERT OR REPLACE INTO peers VALUES(?,?)", (key, encode(data)))
        for key, old in current.items():
            if key in seen: continue
            event = self.s.event("peer.absent", "node:network", "poll-difference", peer_session=key)
            for mode in ("state", "protocol"): self.s.leaf(mode, old["path"], "node:network", "B9A2DD", {}, event, delete=True)
            self.db.execute("DELETE FROM peers WHERE id=?", (key,))

    def message(self, peer, direction, command, size, *, fidelity="message-capture", observed_ns=None, objects=None):
        if direction not in ("incoming", "outgoing") or not isinstance(size, int) or size < 0:
            raise ValueError("Invalid message direction or size")
        actor = "peer:" + component(peer)
        category = traffic_category(command)
        if objects and command in ("inv", "getdata", "notfound"):
            kinds = {obj.get("kind") for obj in objects}
            if kinds <= {"txid", "wtxid"}: category = "transactions"
            elif kinds <= {"blockhash", "filtered-blockhash", "compact-blockhash"}: category = "blocks"
            else: category = "mixed-inventory"
        event = self.s.event("peer.message." + direction, actor, fidelity, command=command, size_bytes=size,
                             source_observed_ns=observed_ns, objects=objects, fidelity=fidelity)
        # All peers pulse shared class targets, avoiding an unbounded directory per session.
        path = f"traffic/{category}/{component(command)}/{direction}.activity"
        self.s.leaf("protocol", path, actor, "6FC5D2" if direction == "incoming" else "E6AB72", {}, event, pulse=True)
        if objects:
            for obj in objects:
                if obj.get("kind") != "txid": continue
                row = self.db.execute("SELECT data FROM mempool WHERE txid=?", (obj["hash"],)).fetchone()
                if row:
                    self.s.leaf("state", mempool_path(obj["hash"], json.loads(row[0])), actor, "E9B949", json.loads(row[0]), event, pulse=True)
