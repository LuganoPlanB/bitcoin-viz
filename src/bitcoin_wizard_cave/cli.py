"""Console entry point. stdout is exclusively the selected output format."""

import argparse
import json
import os
import signal
import sqlite3
import sys
import threading
from pathlib import Path

from . import __version__
from .core import RPC
from .model import Model
from .observer import Observer, writer_lock
from .render import Renderer
from .storage import Store, encode

MODES = ("state", "protocol", "utxo")


def positive(value):
    value = float(value)
    if value <= 0: raise argparse.ArgumentTypeError("must be positive")
    return value


def nonnegative(value):
    value = int(value)
    if value < 0: raise argparse.ArgumentTypeError("must be nonnegative")
    return value


def positive_int(value):
    value = nonnegative(value)
    if value == 0: raise argparse.ArgumentTypeError("must be positive")
    return value


def parser():
    root = argparse.ArgumentParser(description="Bitcoin Wizard Cave — three live Gource views of local Bitcoin activity")
    root.add_argument("--version", action="version", version=__version__)
    commands = root.add_subparsers(dest="command", required=True)
    for name in ("observe", "live", "render", "status", "events", "demo"):
        command = commands.add_parser(name)
        command.add_argument("--db", type=Path, default=Path(".runtime/wizard.sqlite"))
        if name in ("live", "render", "demo"):
            command.add_argument("--mode", choices=MODES, default="state")
            command.add_argument("--max-visible", type=positive_int, default=5000)
        if name in ("observe", "live"):
            command.add_argument("--network", choices=("signet", "regtest"), default="signet")
            command.add_argument("--datadir", type=Path, default=Path.home() / ".bitcoin")
            command.add_argument("--cookie", type=Path)
            command.add_argument("--rpc-url", help="local HTTP RPC URL; defaults to network port")
            command.add_argument("--zmq", help="autodetect Core sequence publisher unless supplied")
            command.add_argument("--window-blocks", type=nonnegative, default=144, help="rolling UTXO creation cohorts; 0 keeps all outputs observed after checkpoint")
            command.add_argument("--undo-horizon", type=positive_int, default=288)
            command.add_argument("--bootstrap-blocks", type=nonnegative, default=6)
            command.add_argument("--metadata-budget", type=nonnegative, default=32)
            command.add_argument("--journal-keep", type=positive_int, default=100_000)
            command.add_argument("--peer-interval", type=positive, default=2)
            command.add_argument("--reconcile-interval", type=positive, default=15)
            command.add_argument("--duration", type=positive)
            command.add_argument("--capture-dir", type=Path, help="optional Core message_capture directory; suppresses duplicate aggregate animation")
        if name == "render": command.add_argument("--once", action="store_true", help="current scene only")
        if name == "events":
            command.add_argument("--after", type=nonnegative, default=0)
            command.add_argument("--limit", type=positive_int, default=100)
    return root


def status(database):
    db = sqlite3.connect(f"{database.resolve().as_uri()}?mode=ro", uri=True)
    try:
        db.execute("BEGIN")
        result = {key: json.loads(value) for key, value in db.execute("SELECT key,value FROM meta") if not key.startswith("capture:")}
        result["counts"] = {table: db.execute(f"SELECT count(*) FROM {table}").fetchone()[0]
                            for table in ("coins", "mempool", "peers", "blocks", "events", "actions")}
        result["leaves"] = dict(db.execute("SELECT mode,count(*) FROM leaves GROUP BY mode"))
        result["coverage"] = "observed outputs only; complete membership is claimed only within retained creation cohorts"
        result["protocol"] = "aggregate byte deltas unless capture is enabled; byte deltas are not message counts"
        print(json.dumps(result, indent=2, sort_keys=True))
    finally: db.close()


def demo(database):
    """A deterministic synthetic fixture, kept separate from a real observer DB."""
    if database.exists(): raise ValueError("Demo requires a new --db to protect existing observations")
    store = Store(database)
    model = Model(store)
    h = lambda number: f"{number:064x}"
    with store.db:
        store.put("node_run", "SYNTHETIC-DEMO")
        store.put("network", "synthetic")
        model.checkpoint(h(0), 100, "synthetic-demo")
        model.admission(h(10), fee_metadata_demo(300, 120))
        model.admission(h(11), fee_metadata_demo(2400, 180))
        model.peers([{"id": 7, "conntime": 1, "network": "ipv4", "connection_type": "outbound-full-relay"}], "demo")
        for command in ("version", "verack", "ping", "addr", "headers", "cmpctblock", "tx", "inv"):
            model.message("demo-peer7", "incoming", command, 300)
        model.connect({"hash": h(1), "height": 101, "previousblockhash": h(0), "tx": [
            {"txid": h(10), "vin": [{"txid": h(8), "vout": 0}], "vout": [
                {"n": i, "value": "0.001", "scriptPubKey": {"hex": "51"}} for i in range(8)]}]})
    store.close()


def fee_metadata_demo(fee, size):
    from .model import fee_metadata
    from decimal import Decimal
    return fee_metadata({"fees": {"base": Decimal(fee) / 100_000_000}, "weight": size * 4})


def main(argv=None):
    os.umask(0o077)
    args = parser().parse_args(argv)
    stop = threading.Event()
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, lambda *_: stop.set())
    try:
        if args.command == "status": return status(args.db)
        if args.command == "events":
            db = sqlite3.connect(f"{args.db.resolve().as_uri()}?mode=ro", uri=True)
            db.row_factory = sqlite3.Row
            try:
                for row in db.execute("SELECT * FROM events WHERE id>? ORDER BY id LIMIT ?", (args.after, args.limit)):
                    event = dict(row)
                    event.update(schema=1, data=json.loads(event["data"]))
                    print(encode(event))
            finally: db.close()
            return
        if args.command == "demo":
            demo(args.db)
            return Renderer(args.db, args.mode, max_visible=args.max_visible).run(stop, follow=False)
        if args.command == "render":
            return Renderer(args.db, args.mode, max_visible=args.max_visible).run(stop, follow=not args.once)
        with writer_lock(args.db):
            store = Store(args.db)
            rpc = RPC(args.rpc_url or f"http://127.0.0.1:{38332 if args.network == 'signet' else 18443}",
                      args.cookie or args.datadir / args.network / ".cookie")
            observer = Observer(store, rpc, network=args.network, address=args.zmq,
                                window=args.window_blocks, undo_horizon=args.undo_horizon,
                                bootstrap=args.bootstrap_blocks, metadata_budget=args.metadata_budget,
                                journal_keep=args.journal_keep)
            if args.capture_dir:
                from .capture import CaptureReader
                observer.capture = CaptureReader(store, observer.model, args.capture_dir)
            renderer = None
            render_errors = []
            if args.command == "live":
                def render():
                    try: Renderer(args.db, args.mode, max_visible=args.max_visible).run(stop)
                    except BrokenPipeError: stop.set()
                    except Exception as error:
                        render_errors.append(error)
                        stop.set()
                renderer = threading.Thread(target=render, daemon=True, name="gource-writer")
                renderer.start()
            try:
                observer.run(stop, duration=args.duration, peer_interval=args.peer_interval,
                             reconcile_interval=args.reconcile_interval)
            finally:
                stop.set()
                if renderer: renderer.join(1)
                store.close()
            if render_errors: raise render_errors[0]
    except (ValueError, RuntimeError, OSError, sqlite3.Error) as error:
        print(f"bitcoin-wizard-cave: {error}", file=sys.stderr)
        raise SystemExit(1) from None


if __name__ == "__main__": main()
