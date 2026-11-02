#!/usr/bin/env python3
"""Exercise the standalone native observer against a running signet Core.

--exercise-reorg is for a disposable project node: temporarily freeze networking,
invalidate/reconsider its tip, and restore its prior network-active setting.
Python is the test driver; the observed executable has no Python dependency.
"""

import argparse
import hashlib
import json
import signal
import sqlite3
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
from bitcoin_wizard_cave.core import RPC
from bitcoin_wizard_cave.model import sats


def until(predicate, description, timeout=20):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        try:
            value = predicate()
            if value: return value
        except (sqlite3.Error, FileNotFoundError): pass
        time.sleep(0.05)
    raise AssertionError("Timed out: " + description)


def data(path, query, params=()):
    with sqlite3.connect(f"file:{path}?mode=ro", uri=True) as db:
        return db.execute(query, params).fetchall()


def meta(path, key):
    rows = data(path, "SELECT value FROM meta WHERE key=?", (key,))
    return json.loads(rows[0][0]) if rows else None


def digest(path):
    result = hashlib.sha256()
    for outpoint, coin in data(path, "SELECT id,data FROM coins ORDER BY id"):
        canonical = json.dumps(json.loads(coin), sort_keys=True, separators=(",", ":"))
        result.update((outpoint + canonical).encode())
    return result.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--datadir", type=Path, required=True)
    parser.add_argument("--db", type=Path, required=True)
    parser.add_argument("--rpc-url", default="http://127.0.0.1:38332")
    parser.add_argument("--exercise-reorg", action="store_true")
    args = parser.parse_args()
    if args.db.exists(): parser.error("--db must be new")
    args.db.parent.mkdir(parents=True, exist_ok=True)
    rpc = RPC(args.rpc_url, args.datadir / "signet" / ".cookie")
    active = rpc.call("getnetworkinfo")["networkactive"]
    invalidated = None
    process = None
    binary = args.binary.resolve()
    diagnostics = args.db.with_suffix(".stderr")
    try:
        if args.exercise_reorg: rpc.call("setnetworkactive", False)
        with diagnostics.open("w") as error:
            process = subprocess.Popen([str(binary), "observe", "--datadir", str(args.datadir.resolve()), "--db", str(args.db),
                "--rpc-url", args.rpc_url, "--bootstrap-blocks", "2", "--metadata-budget", "0", "--peer-interval", "0.2",
                "--reconcile-interval", "0.5", "--duration", "60"], stdout=subprocess.DEVNULL, stderr=error)
            until(lambda: meta(args.db, "synchronized"), "native baseline")
            baseline = meta(args.db, "tip")
            before = digest(args.db)
            assert baseline["hash"] == rpc.call("getbestblockhash")
            readmitted = 0
            if args.exercise_reorg:
                assert baseline["height"] > 1, "Download at least two signet blocks first"
                invalidated = baseline["hash"]
                stamp = meta(args.db, "last_reconcile_ns")
                rpc.call("invalidateblock", invalidated)
                until(lambda: meta(args.db, "tip")["height"] == baseline["height"] - 1 and
                      meta(args.db, "last_reconcile_ns") > stamp, "native disconnect and snapshot")
                expected = set(rpc.call("getrawmempool"))
                until(lambda: {r[0] for r in data(args.db, "SELECT txid FROM mempool")} == expected, "native readmission membership")
                readmitted = len(expected)
                rpc.call("reconsiderblock", invalidated)
                until(lambda: meta(args.db, "tip") == baseline and not data(args.db, "SELECT txid FROM mempool"), "native reconnect and confirmation removals")
                assert digest(args.db) == before, "Tracked UTXO data changed after undo/reapply"
                invalidated = None
            checked = 0
            for outpoint, coin in data(args.db, "SELECT id,data FROM coins ORDER BY id LIMIT 20"):
                txid, index = outpoint.split(":")
                actual = rpc.call("gettxout", txid, int(index), False)
                assert actual is not None and sats(actual["value"]) == json.loads(coin)["value_sat"]
                checked += 1
            process.send_signal(signal.SIGTERM)
            assert process.wait(timeout=5) == 0
        assert meta(args.db, "synchronized") is False
        assert "chain-notification" in {r[0] for r in data(args.db, "SELECT cause FROM events")} or not args.exercise_reorg
        journal_counts = dict(data(args.db, "SELECT kind,count(*) FROM events GROUP BY kind"))
        assert journal_counts.get("chain.disconnect", 0) == (1 if args.exercise_reorg else 0)
        # A known Python-written database from the first signet exercise is also
        # readable directly by the native renderer without export or migration.
        python_db = args.db.parent / "wizard.sqlite"
        cross_read = False
        if python_db.is_file():
            result = subprocess.run([str(binary), "render", "--db", str(python_db), "--mode", "utxo", "--once", "--max-visible", "20"], capture_output=True, text=True, timeout=5)
            assert result.returncode == 0 and len(result.stdout.splitlines()) == 20
            cross_read = True
        report = {"implementation": "standalone C++17", "binary": str(binary), "network": meta(args.db, "network"),
                  "core_version": meta(args.db, "core_version"), "applied_tip": baseline,
                  "initialblockdownload": rpc.call("getblockchaininfo")["initialblockdownload"],
                  "tracked_coins": data(args.db, "SELECT count(*) FROM coins")[0][0], "coin_digest": before,
                  "readmitted_transactions": readmitted, "gettxout_spot_checks": checked,
                  "journal_counts": journal_counts, "native_read_python_db": cross_read,
                  "zmq_chain_reconciliation": bool(args.exercise_reorg)}
        print(json.dumps(report, indent=2, sort_keys=True))
    finally:
        try:
            if invalidated: rpc.call("reconsiderblock", invalidated)
            if args.exercise_reorg: rpc.call("setnetworkactive", active)
        finally:
            if process and process.poll() is None:
                process.terminate()
                try: process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
            rpc.close()


if __name__ == "__main__": main()
