# SPDX-License-Identifier: GPL-3.0-or-later
"""Run the desktop Session against an isolated Bitcoin Core regtest."""

import contextlib
import json
from pathlib import Path
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time

from desktop_session import SessionProcess, fixture_env, meta


def wait_for(description, predicate, timeout=8):
    deadline = time.monotonic() + timeout
    while not predicate():
        assert time.monotonic() < deadline, f"Timed out waiting for {description}"
        time.sleep(0.04)


def events(database, kind):
    path = Path(database).resolve().as_uri() + "?mode=ro"
    with contextlib.closing(sqlite3.connect(path, uri=True)) as connection:
        return [(actor, cause, json.loads(data)) for actor, cause, data in connection.execute(
            "SELECT actor, cause, data FROM events WHERE kind=? ORDER BY id", (kind,),
        )]


class Core:
    def __init__(self, executable, cli, data, env):
        self.executable = executable
        self.cli = cli
        self.data = data
        self.env = env
        self.process = None
        self.cookie = data / "regtest" / ".cookie"
        self.data.mkdir()
        with socket.socket() as rpc, socket.socket() as zmq:
            rpc.bind(("127.0.0.1", 0))
            zmq.bind(("127.0.0.1", 0))
            self.rpc_port = rpc.getsockname()[1]
            self.zmq_port = zmq.getsockname()[1]
        self.rpc_url = f"http://127.0.0.1:{self.rpc_port}"
        self.zmq_url = f"tcp://127.0.0.1:{self.zmq_port}"

    def rpc(self, method, *params, wallet=None):
        args = [self.cli, "-regtest", f"-datadir={self.data}", "-rpcconnect=127.0.0.1",
                f"-rpcport={self.rpc_port}", "-rpcclienttimeout=3"]
        if wallet:
            args.append(f"-rpcwallet={wallet}")
        result = subprocess.run([*args, method, *map(str, params)], text=True, capture_output=True,
                                env=self.env, timeout=8)
        if result.returncode:
            raise RuntimeError(f"Core RPC {method} failed ({result.returncode}): {result.stderr.strip()}")
        try:
            return json.loads(result.stdout)
        except json.JSONDecodeError:
            return result.stdout.strip()

    def start(self, sequence=False):
        assert self.process is None
        args = [self.executable, "-regtest", f"-datadir={self.data}", "-server=1",
                f"-rpcport={self.rpc_port}", "-rpcbind=127.0.0.1", "-networkactive=0",
                "-listen=0", "-printtoconsole=0", "-fallbackfee=0.0002"]
        if sequence:
            args.append(f"-zmqpubsequence={self.zmq_url}")
        self.process = subprocess.Popen(args, env=self.env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

        def ready():
            assert self.process.poll() is None, "Core exited before RPC became ready"
            if not self.cookie.exists():
                return False
            try:
                return self.rpc("getblockchaininfo")["chain"] == "regtest"
            except RuntimeError:
                return False

        wait_for("regtest RPC readiness", ready)

    def stop(self):
        if self.process is None:
            return
        process = self.process
        try:
            if process.poll() is None:
                self.rpc("stop")
            assert process.wait(timeout=8) == 0, "Core did not exit cleanly"
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            self.process = None

    def __enter__(self):
        return self

    def __exit__(self, *exception):
        self.stop()


def observe(session, core):
    session.request("connect", network=2, datadir=str(core.data), url=core.rpc_url)


def assert_tip(database, height, block):
    tip = meta(database, "tip")
    return tip["height"] == height and tip["hash"] == block


def run(executable, collector, bitcoind, cli, root, env):
    wallet = "session-regtest"
    observations = root / "observations"
    with Core(bitcoind, cli, root / "node data", env) as core:
        core.start()
        assert core.rpc("getzmqnotifications") == []
        core.rpc("createwallet", wallet)
        address = core.rpc("getnewaddress", wallet=wallet)
        core.rpc("generatetoaddress", 101, address)

        strict = subprocess.run([collector, "observe", "--db", str(root / "cli.sqlite"), "--datadir",
                                 str(core.data), "--network", "regtest", "--rpc-url", core.rpc_url,
                                 "--duration", "0.2"], capture_output=True, env=env, timeout=8)
        assert strict.returncode == 1 and b"Core has no sequence publisher" in strict.stderr

        with SessionProcess(executable, observations, env) as session:
            observe(session, core)
            polling = session.wait("Connected / RPC polling / snapshots only", timeout=8)
            database = polling["database"]
            assert assert_tip(database, 101, core.rpc("getbestblockhash"))
            with SessionProcess(executable, observations, env) as competing:
                observe(competing, core)
                assert not competing.wait("already owns this database")["database"], "Two writers opened the same database"
                competing.stop()
            polled_block = core.rpc("generatetoaddress", 1, address)[0]
            polled_tx = core.rpc("sendtoaddress", address, 1, wallet=wallet)
            wait_for("RPC-only block height 102", lambda: assert_tip(database, 102, polled_block), timeout=21)
            wait_for("RPC-only mempool snapshot difference", lambda: any(
                actor == "node:snapshot" and cause == "snapshot-population" and data["txid"] == polled_tx
                for actor, cause, data in events(database, "mempool.add")
            ))
            assert any(data["blockhash"] == polled_block for _, _, data in events(database, "chain.connect"))
            assert not any(cause == "chain-notification" for _, cause, _ in events(database, "observer.resync"))
            print("PASS RPC-only snapshots: Core height 102, exact block and mempool transaction, exclusive writer", flush=True)

            first_cookie = core.cookie.read_bytes()
            first_run = meta(database, "node_run")
            core.stop()
            session.wait("Reconnecting", timeout=8)
            core.start(sequence=True)
            assert first_cookie != core.cookie.read_bytes(), "Bitcoin Core cookie did not rotate"
            assert any(item["type"] == "pubsequence" and item["address"] == core.zmq_url
                       for item in core.rpc("getzmqnotifications"))
            sequence = session.wait("Connected / RPC + ZMQ sequence", timeout=12)
            assert sequence["database"] == database and sequence["generation"] == polling["generation"]
            assert meta(database, "node_run") != first_run
            assert any(cause == "node-restart" for _, cause, _ in events(database, "observer.resync"))
            if wallet not in core.rpc("listwallets"):
                core.rpc("loadwallet", wallet)
            # Require a chain notification before exercising the mempool feed.
            notification_block = core.rpc("generatetoaddress", 1, address)[0]
            wait_for("ZMQ block notification and exact tip at height 103", lambda: any(
                cause == "chain-notification" and data["tip"]["hash"] == notification_block
                for _, cause, data in events(database, "observer.resync")
            ))
            assert assert_tip(database, 103, notification_block)
            live_tx = core.rpc("sendtoaddress", address, 1, wallet=wallet)
            wait_for("ZMQ mempool admission", lambda: any(
                actor == "node:mempool" and cause == "admission" and data["txid"] == live_tx
                and data["source"]["kind"] == "zmq" and data["source"]["topic"] == "sequence"
                for actor, cause, data in events(database, "mempool.add")
            ))
            print("PASS live ZMQ: rotated cookie, same database, Core height 103, chain notification and sequence admission", flush=True)

            sequence_run = meta(database, "node_run")
            core.stop()
            session.wait("Reconnecting", timeout=8)
            core.start()
            plain = session.wait("Connected / RPC polling / snapshots only", timeout=12)
            assert plain["status"].endswith("snapshots only"), "Old publisher survived restart without ZMQ"
            assert plain["database"] == database and plain["generation"] == polling["generation"]
            assert meta(database, "node_run") != sequence_run
            assert meta(database, "network") == "regtest"
            assert any(cause == "sequence-disconnected" for _, cause, _ in events(database, "observer.source-loss"))
            assert not any("synthetic" in cause for _, cause, _ in events(database, "observer.rebuild"))
            assert core.rpc("getblockcount") == 103
            assert assert_tip(database, 103, core.rpc("getbestblockhash"))
            started = time.monotonic()
            session.stop()
            elapsed = time.monotonic() - started
            assert elapsed < 3 and meta(database, "synchronized") is False
            print(f"PASS restart without ZMQ: source loss recorded, RPC-only recovered, shutdown {elapsed:.3f}s", flush=True)


if __name__ == "__main__":
    executable, collector, bitcoind, cli = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix="viz-desktop-regtest-") as temp:
        root = Path(temp)
        env, _ = fixture_env(root)
        run(executable, collector, bitcoind, cli, root, env)
