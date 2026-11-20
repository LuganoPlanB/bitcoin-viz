# SPDX-License-Identifier: GPL-3.0-or-later
"""Loopback RPC fixtures for the native desktop Session."""

import base64
import contextlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import queue
import secrets
import socket
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time


class HeldCall:
    def __init__(self):
        self.entered = threading.Event()
        self.release = threading.Event()
        self.finished = threading.Event()


class Handler(BaseHTTPRequestHandler):
    timeout = 5

    def log_message(self, *args):
        pass

    def do_POST(self):
        node = self.server
        node.attempts += 1
        request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        if self.headers.get("Authorization") != node.authorization:
            self.send_error(401)
            return
        method = request["method"]
        hold = node.holds.get(method)
        try:
            if hold:
                hold.entered.set()
                if not hold.release.wait(14):
                    return
            results = {
                "getblockchaininfo": {"chain": node.chain, "blocks": 0},
                "getnetworkinfo": {"version": 300000},
                "getblockhash": node.genesis,
                "getblockheader": {"confirmations": 1},
                "getbestblockhash": node.genesis,
                "getpeerinfo": [],
                "getrawmempool": {"txids": [], "mempool_sequence": 1},
                "getzmqnotifications": node.notifications,
                "uptime": 1,
            }
            if method not in results:
                self.send_error(500)
                return
            response = json.dumps({"jsonrpc": "2.0", "id": request["id"], "result": results[method]}).encode()
            self.send_response(200)
            self.send_header("Content-Length", str(len(response)))
            self.end_headers()
            self.wfile.write(response)
        except (BrokenPipeError, ConnectionResetError):
            pass  # Reconfiguration cancels the held client transfer.
        finally:
            if hold:
                hold.finished.set()


class NodeServer(ThreadingHTTPServer):
    allow_reuse_address = sys.platform != "win32"
    daemon_threads = False

    def __init__(self, chain, genesis, port=0):
        super().__init__(("127.0.0.1", port), Handler)
        self.chain = chain
        self.genesis = f"{genesis:064x}"
        self.cookie = "__cookie__:" + secrets.token_hex(16)
        self.authorization = "Basic " + base64.b64encode(self.cookie.encode()).decode()
        self.attempts = 0
        self.holds = {}
        self.notifications = []

    def server_bind(self):
        if sys.platform == "win32":
            self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
        super().server_bind()

    @property
    def url(self):
        return f"http://127.0.0.1:{self.server_port}"

    def set_cookie(self, path):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(self.cookie + "\n", encoding="utf-8")
        return path

    def hold(self, method):
        self.holds[method] = HeldCall()
        return self.holds[method]


@contextlib.contextmanager
def node_server(*args):
    with NodeServer(*args) as server:
        thread = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.05})
        thread.start()
        try:
            yield server
        finally:
            for hold in server.holds.values():
                hold.release.set()
            server.shutdown()
            thread.join(2)
            assert not thread.is_alive(), "RPC server thread did not terminate"
            for hold in server.holds.values():
                if hold.entered.is_set():
                    assert hold.finished.wait(2), "Held RPC did not settle"


def fixture_env(root):
    env = os.environ.copy()
    if sys.platform == "win32":
        env["APPDATA"] = str(root / "app data" / "Roaming")
        env["LOCALAPPDATA"] = str(root / "app data" / "Local")
        base = Path(env["LOCALAPPDATA"]) / "Bitcoin"
    elif sys.platform == "darwin":
        env["HOME"] = str(root / "isolated home")
        base = Path(env["HOME"]) / "Library" / "Application Support" / "Bitcoin"
    else:
        env["HOME"] = str(root / "isolated home")
        base = Path(env["HOME"]) / ".bitcoin"
    return env, base


class SessionProcess:
    def __init__(self, executable, root, env):
        self.executable = executable
        self.root = root
        self.env = env
        self.replies = queue.Queue()
        self.output = []

    def __enter__(self):
        self.log = tempfile.TemporaryFile()
        self.process = subprocess.Popen(
            [self.executable], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.log,
            text=True, encoding="utf-8", env=self.env,
        )
        self.reader = threading.Thread(target=self.read)
        self.reader.start()
        self.send(str(self.root))
        return self

    def read(self):
        for line in self.process.stdout:
            self.output.append(line)
            self.replies.put(line)
        self.replies.put(None)

    def send(self, value):
        self.process.stdin.write(json.dumps(value) + "\n")
        self.process.stdin.flush()

    def request(self, command, **settings):
        self.send({"command": command, **settings})
        line = self.replies.get(timeout=3)
        assert line is not None, "Session test process ended during a request"
        return json.loads(line)

    def wait(self, status, timeout=4):
        deadline = time.monotonic() + timeout
        while True:
            state = self.request("state")
            if status in state["status"]:
                return state
            assert time.monotonic() < deadline, f"Timed out waiting for {status!r}: {state['status']}"
            time.sleep(0.025)

    def stop(self):
        self.send({"command": "stop"})
        assert self.process.wait(timeout=3) == 0, "Session process failed at shutdown"

    def assert_no_credentials(self, *nodes):
        assert self.process.poll() is not None, "Stop the Session before checking its log"
        self.reader.join(2)
        assert not self.reader.is_alive(), "Session output reader did not terminate"
        self.log.seek(0)
        messages = self.log.read() + "".join(self.output).encode()
        for node in nodes:
            assert node.cookie.encode() not in messages, "Cookie appeared in Session log"
            assert node.authorization.encode() not in messages, "Authorization appeared in Session log"

    def __exit__(self, *exception):
        try:
            if self.process.poll() is None:
                self.process.stdin.close()
                self.process.wait(timeout=3)
            if exception[0] is None:
                assert self.process.returncode == 0, "Session process failed"
        finally:
            if self.process.poll() is None:
                self.process.kill()
                self.process.wait()
            self.reader.join(2)
            self.process.stdin.close()
            self.process.stdout.close()
            self.log.close()


def meta(database, key):
    with contextlib.closing(sqlite3.connect(Path(database).resolve().as_uri() + "?mode=ro", uri=True)) as connection:
        value, = connection.execute("SELECT value FROM meta WHERE key=?", (key,)).fetchone()
        return json.loads(value)


def paths(executable, root, env, base):
    with SessionProcess(executable, root / "observations", env) as session:
        assert Path(session.request("datadir")) == base, "Native default data directory differs"
        if sys.platform == "win32":
            legacy = Path(env["APPDATA"]) / "Bitcoin"
            legacy.mkdir(parents=True)
            assert Path(session.request("datadir")) == legacy, "Existing roaming data directory was ignored"


def discovery(executable, root, env, base):
    # Own both standard ports before writing any fixture cookie. A port collision
    # fails the test before the Session can contact another process.
    with node_server("main", 1, 8332) as main, node_server("signet", 2, 38332) as signet:
        with SessionProcess(executable, root / "observations", env) as session:
            session.request("connect")
            assert not session.wait("No local node cookie found")["database"]
            assert main.attempts == signet.attempts == 0

            base.mkdir(parents=True)
            (base / ".cookie").write_text("__cookie__:" + secrets.token_hex(16), encoding="utf-8")
            session.request("connect")
            assert not session.wait("cookie rejected")["database"], "Stale cookie published a database"

            main.set_cookie(base / ".cookie")
            signet.set_cookie(base / "signet" / ".cookie")
            session.request("connect")
            first = session.wait("main / Connected")
            assert meta(first["database"], "genesis") == main.genesis
            assert signet.attempts == 0, "Mainnet success did not stop discovery"

            main.chain = "regtest"
            session.request("connect", network=0)
            assert not session.wait("another chain")["database"], "Mismatched chain published a database"
            session.request("connect")
            later = session.wait("signet / Connected")
            assert meta(later["database"], "genesis") == signet.genesis
            assert first["database"] != later["database"], "Two discovered chains shared a database"
            session.stop()
            session.assert_no_credentials(main, signet)


def switch(executable, root, env, base):
    with node_server("regtest", 3) as first, node_server("regtest", 4) as second:
        first_hold = first.hold("getpeerinfo")
        second_probe = second.hold("getblockchaininfo")
        second_peer = second.hold("getpeerinfo")
        data = root / "custom data"
        first.set_cookie(data / "regtest" / ".cookie")
        override = second.set_cookie(root / "cookie overrides" / "nœud.cookie")
        with SessionProcess(executable, root / "observations", env) as session:
            session.request("connect", network=2, datadir=str(data), url=first.url)
            old = session.wait("regtest / Connected")
            assert meta(old["database"], "genesis") == first.genesis
            assert first_hold.entered.wait(2), "Old RPC did not reach the held call"

            new = session.request("connect", url=second.url, cookie=str(override))
            assert new["generation"] == old["generation"] + 1
            assert not new["database"], "Previous database was visible after Connect"
            assert second_probe.entered.wait(3), "Old RPC delayed the new endpoint"
            assert not first_hold.release.is_set(), "Old RPC was released before cancellation was tested"
            second_probe.release.set()
            observed = session.wait("regtest / Connected")
            assert observed["generation"] == new["generation"]
            assert observed["database"] != old["database"], "Two regtest nodes shared a database"
            assert meta(observed["database"], "genesis") == second.genesis
            assert meta(old["database"], "genesis") == first.genesis

            first_hold.release.set()
            assert first_hold.finished.wait(2), "Old held response did not settle"
            assert session.request("state") == observed, "Old response overwrote the new source"
            assert second_peer.entered.wait(2), "New RPC did not reach the held call"
            session.stop()
            assert not second_peer.release.is_set(), "Shutdown test released its held RPC too early"
            session.assert_no_credentials(first, second)


def sequence(executable, root, env, base):
    with node_server("regtest", 5) as node, socket.socket() as unused:
        unused.bind(("127.0.0.1", 0))
        override = f"tcp://127.0.0.1:{unused.getsockname()[1]}"
        node.notifications = [{"type": "pubsequence", "address": "tcp://0.0.0.0:28333"}]
        cookie = node.set_cookie(root / "fixture.cookie")
        with SessionProcess(executable, root / "observations", env) as session:
            session.request("connect", network=2, cookie=str(cookie), url=node.url)
            fallback = session.wait("Connected / RPC polling / snapshots only / Sequence unavailable")
            assert meta(fallback["database"], "genesis") == node.genesis

            session.request("connect", network=2, cookie=str(cookie), url=node.url, zmq=override)
            pending = session.wait("Connected / RPC polling / snapshots only / Waiting for sequence publisher")
            assert pending["database"] == fallback["database"], "Changing transport replaced the node database"
            session.stop()
            session.assert_no_credentials(node)


if __name__ == "__main__":
    name, executable = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix="viz-desktop-session-") as temp:
        root = Path(temp)
        env, base = fixture_env(root)
        {"paths": paths, "discovery": discovery, "switch": switch, "sequence": sequence}[name](
            executable, root, env, base,
        )
    print(f"desktop-node-{name} passed")
