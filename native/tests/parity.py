"""Cross-language behavioral checks; the native executable never invokes Python."""

import json
import sqlite3
import subprocess
import sys
import tempfile
from decimal import Decimal
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "src"))
from bitcoin_wizard_cave.model import Model, fee_metadata
from bitcoin_wizard_cave.render import Renderer
from bitcoin_wizard_cave.storage import Store
from bitcoin_wizard_cave.cli import demo


def h(n): return f"{n:064x}"


def tx(n, inputs=(), outputs=1, script="51"):
    return {"txid": h(n), "vin": [{"txid": h(i), "vout": v} for i, v in inputs],
            "vout": [{"n": i, "value": "0.00000100", "scriptPubKey": {"hex": script}} for i in range(outputs)]}


def block(height, transactions=(), number=None):
    return {"hash": h(number or height), "height": height, "previousblockhash": h(height - 1), "time": 123, "tx": list(transactions)}


def apply(model, store, op):
    kind = op["op"]
    with store.db:
        if kind == "checkpoint":
            model.window = op.get("window", 144)
            model.undo_horizon = op.get("horizon", 288)
            model.checkpoint(op["hash"], op["height"], op.get("cause", "controlled-replay"))
        elif kind == "admission": model.admission(op["txid"], op.get("data"), op.get("actor", "node:mempool"), op.get("cause", "admission"), op.get("source"))
        elif kind == "removal": model.removal(op["txid"], op.get("cause", "unknown-removal"), op.get("actor", "node:mempool"), op.get("source"))
        elif kind == "enrich": model.enrich(op["txid"], op["data"])
        elif kind == "fee": model.enrich(op["txid"], fee_metadata(op["entry"]))
        elif kind == "pending": model.pending(op["txid"], op["transaction"])
        elif kind == "snapshot": model.reconcile_mempool(op["txids"], op["boundary"])
        elif kind == "connect": model.connect(op["block"])
        elif kind == "disconnect": model.disconnect(op["hash"])
        elif kind == "peers": model.peers(op["peers"], op.get("session", "replay"), op.get("aggregate", True))
        elif kind == "message": model.message(op["peer"], op["direction"], op["command"], op["size"], fidelity=op.get("fidelity", "message-capture"), observed_ns=op.get("observed_ns"), objects=op.get("objects"))
        elif kind == "loss": store.event("observer.source-loss", cause=op.get("cause", "controlled-loss"))
        elif kind == "prune": store.prune(op["keep"])
        else: raise AssertionError(kind)


def normalized(value):
    if isinstance(value, dict):
        return {k: Decimal(str(v)) if k == "sat_per_vb" and v is not None else normalized(v)
                for k, v in value.items() if k != "observed_ns"}
    if isinstance(value, list): return [normalized(v) for v in value]
    return value


def state(db):
    result = {}
    for table, columns in {
        "coins": ("id", "height", "data"), "mempool": ("txid", "data"), "peers": ("id", "data"),
        "blocks": ("hash", "height", "parent", "undo", "coverage"),
        "leaves": ("mode", "path", "actor", "color", "data"),
    }.items():
        result[table] = [tuple(normalized(json.loads(value)) if name in ("data", "undo") else value
                               for name, value in zip(columns, row))
                         for row in db.execute(f"SELECT {','.join(columns)} FROM {table} ORDER BY 1,2")]
    result["meta"] = {k: normalized(json.loads(v)) for k, v in db.execute("SELECT key,value FROM meta")}
    events = {}
    result["events"] = []
    for event_id, kind, actor, cause, data in db.execute("SELECT id,kind,actor,cause,data FROM events ORDER BY id"):
        event = (kind, actor, cause, normalized(json.loads(data)))
        events[event_id] = event
        result["events"].append(event)
    for mode in ("state", "protocol", "utxo"):
        result["actions:" + mode] = [(action, path, actor, color, events[event])
            for event, action, path, actor, color in db.execute(
                "SELECT event_id,action,path,actor,color FROM actions WHERE mode=? ORDER BY id", (mode,))]
    return result


def run(binary, *args):
    p = subprocess.run([str(binary), *map(str, args)], capture_output=True, text=True, timeout=15)
    assert p.returncode == 0, (args, p.stderr)
    return p.stdout


def main():
    binary = Path(sys.argv[1]).resolve()
    checkpoint = {"op": "checkpoint", "hash": h(0), "height": 0}
    admit = lambda n: {"op": "admission", "txid": h(n)}
    connect = lambda height, *transactions: {"op": "connect", "block": block(height, transactions)}
    disconnect = lambda height: {"op": "disconnect", "hash": h(height)}
    cases = {
        "confirmation": [admit(10), admit(11), connect(1, tx(10))],
        "replacement": [admit(10), admit(11), {"op": "removal", "txid": h(10), "cause": "replacement-observed"},
                        {"op": "removal", "txid": h(11), "cause": "replacement-observed"}, admit(12)],
        "observed-readmission": [admit(10), connect(1, tx(10)), disconnect(1), {"op": "snapshot", "txids": [h(10)], "boundary": 77}],
        "intrablock-undo": [connect(1, tx(10), tx(11, [(10, 0)])), disconnect(1)],
        "full-coin-undo": [connect(1, tx(10)), connect(2, tx(11, [(10, 0)])), disconnect(2)],
        "pending-overlay": [connect(1, tx(10)), admit(11), {"op": "pending", "txid": h(11), "transaction": tx(11, [(10, 0)])}],
        "pending-removal": [connect(1, tx(10)), admit(11), {"op": "pending", "txid": h(11), "transaction": tx(11, [(10, 0)])}, {"op": "removal", "txid": h(11)}],
        "late-metadata": [admit(10), {"op": "removal", "txid": h(10)}, {"op": "enrich", "txid": h(10), "data": {"band": "fee-20-plus"}}],
        "fee-modern": [admit(10), {"op": "fee", "txid": h(10), "entry": {"fees": {"base": "0.00001"}, "vsize_bip141": 200}}],
        "fee-weight": [admit(10), {"op": "fee", "txid": h(10), "entry": {"fees": {"base": "0.00001"}, "weight": 401}}],
        "fee-legacy-unknown": [admit(10), {"op": "fee", "txid": h(10), "entry": {"fees": {"base": "0.00001"}, "vsize": 100}}],
        "unspendable": [connect(1, tx(10, script="6a0100"), tx(11, script="51" * 10001), tx(12))],
        "expiry-undo": [{**checkpoint, "window": 1}, connect(1, tx(10)), connect(2), disconnect(2)],
        "counter-source": [admit(10), {"op": "snapshot", "txids": [h(11)], "boundary": 2**60 + 7}],
        "peer-delta-and-session": [
            {"op": "peers", "peers": [{"id": 4, "conntime": 123, "bytesrecv_per_msg": {"inv": 100, "ping": 5}}]},
            {"op": "peers", "peers": [{"id": 4, "conntime": 123, "bytesrecv_per_msg": {"inv": 160, "ping": 13}}]},
            {"op": "peers", "peers": [{"id": 4, "conntime": 123}], "session": "new-run"}],
        "peer-path-move": [{"op": "peers", "peers": [{"id": 1, "network": "ipv4"}]}, {"op": "peers", "peers": [{"id": 1, "network": "onion"}]}],
        "inventory-observation": [admit(10), {"op": "message", "peer": "evil|peer\n../../name", "direction": "incoming", "command": "inv", "size": 73,
            "objects": [{"kind": "txid", "hash": h(10)}, {"kind": "wtxid", "hash": h(11)}]}],
        "mixed-inventory": [{"op": "message", "peer": "endpoint", "direction": "outgoing", "command": "getdata", "size": 73,
            "objects": [{"kind": "txid", "hash": h(10)}, {"kind": "blockhash", "hash": h(1)}]}],
        "source-loss": [{"op": "loss"}, {"op": "loss", "cause": "capture-truncation"}],
        "journal-pruning": [admit(10), {"op": "removal", "txid": h(10)}, admit(11), {"op": "prune", "keep": 1}],
    }
    with tempfile.TemporaryDirectory(prefix="bwc-parity-") as directory:
        root = Path(directory)
        for name, operations in cases.items():
            native, python = root / (name + "-cpp.sqlite"), root / (name + "-python.sqlite")
            fixture = root / (name + ".jsonl")
            operations = [checkpoint, *operations]
            fixture.write_text("\n".join(json.dumps(op, sort_keys=True) for op in operations) + "\n")
            store = Store(python)
            with store.db:
                store.put("node_run", "SYNTHETIC-REPLAY")
                store.put("network", "synthetic")
            model = Model(store)
            for op in operations: apply(model, store, json.loads(json.dumps(op, sort_keys=True)))
            run(binary, "replay", "--db", native, "--input", fixture)
            native_db = sqlite3.connect(native)
            expected, actual = state(store.db), state(native_db)
            for section in expected:
                assert expected[section] == actual[section], f"{name}: {section} differs\nPython: {expected[section]}\nNative: {actual[section]}"
            native_db.close()
            for mode in ("state", "protocol", "utxo"):
                from io import StringIO
                output = StringIO()
                reader = Renderer(native, mode, output)
                reader.snapshot()
                reader.db.close()
                cpp_output = run(binary, "render", "--db", python, "--mode", mode, "--once")
                assert [line.split("|", 1)[1] for line in output.getvalue().splitlines()] == [line.split("|", 1)[1] for line in cpp_output.splitlines()], (name, mode, "cross-reader scene differs")
            store.close()
        # The user-facing synthetic demo is equivalent in both implementations.
        pydemo, cppdemo = root / "demo-py.sqlite", root / "demo-cpp.sqlite"
        demo(pydemo)
        run(binary, "demo", "--db", cppdemo)
        pydb, cppdb = sqlite3.connect(pydemo), sqlite3.connect(cppdemo)
        assert state(pydb) == state(cppdb), "Synthetic demo differs"
        pydb.close()
        cppdb.close()
        for args in [("render", "--mode", "wrong"), ("observe", "--duration", "nan"), ("observe", "--metadata-budget", "-1")]:
            p = subprocess.run([str(binary), *args], capture_output=True, text=True, timeout=5)
            assert p.returncode != 0 and not p.stdout, (args, "invalid options were accepted")
        p = subprocess.run([str(binary), "demo", "--db", str(cppdemo)], capture_output=True, text=True, timeout=5)
        assert p.returncode != 0, "Demo overwrote an existing database"
    print(f"Cross-language parity: {len(cases)} state/journal scenarios, 120 cross-reader scenes, equivalent demos, CLI rejection checks passed")


if __name__ == "__main__": main()
