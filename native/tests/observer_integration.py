"""Controlled HTTP RPC + actual libzmq IPC tests using only Python's stdlib."""

import base64
import contextlib
import ctypes
import ctypes.util
import http.server
import json
import os
import signal
import sqlite3
import struct
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path


def h(n): return f"{n:064x}"


def until(predicate, label, timeout=4):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            value = predicate()
            if value: return value
        except (sqlite3.Error, FileNotFoundError): pass
        time.sleep(0.025)
    raise AssertionError("Timed out: " + label)


def rows(path, query, params=()):
    with contextlib.closing(sqlite3.connect(f"file:{path}?mode=ro", uri=True, timeout=0.2)) as db:
        return db.execute(query, params).fetchall()


def members(path): return {r[0] for r in rows(path, "SELECT txid FROM mempool")}


def meta(path, key):
    result = rows(path, "SELECT value FROM meta WHERE key=?", (key,))
    return json.loads(result[0][0]) if result else None


class Publisher:
    def __init__(self, path):
        self.python_zmq = None
        try:
            import zmq
            self.python_zmq = zmq
        except ImportError:
            pass
        if self.python_zmq is not None:
            self.context = zmq.Context()
            self.socket = self.context.socket(zmq.PUB)
            address = "tcp://127.0.0.1:*" if os.name == "nt" else "ipc://" + str(path)
            self.socket.bind(address)
            self.endpoint = self.socket.getsockopt(zmq.LAST_ENDPOINT).decode()
            return
        self.lib = ctypes.CDLL(ctypes.util.find_library("zmq"))
        for name, restype, args in [
            ("zmq_ctx_new", ctypes.c_void_p, []),
            ("zmq_socket", ctypes.c_void_p, [ctypes.c_void_p, ctypes.c_int]),
            ("zmq_bind", ctypes.c_int, [ctypes.c_void_p, ctypes.c_char_p]),
            ("zmq_send", ctypes.c_int, [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int]),
            ("zmq_setsockopt", ctypes.c_int, [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_size_t]),
            ("zmq_close", ctypes.c_int, [ctypes.c_void_p]),
            ("zmq_ctx_term", ctypes.c_int, [ctypes.c_void_p]),
        ]:
            fn = getattr(self.lib, name)
            fn.restype, fn.argtypes = restype, args
        self.context = self.lib.zmq_ctx_new()
        self.socket = self.lib.zmq_socket(self.context, 1)
        self.endpoint = "ipc://" + str(path)
        linger = ctypes.c_int(0)
        assert self.lib.zmq_setsockopt(self.socket, 17, ctypes.byref(linger), ctypes.sizeof(linger)) == 0
        assert self.lib.zmq_bind(self.socket, self.endpoint.encode()) == 0

    def send(self, label, n, counter, sequence):
        frames = [b"sequence", bytes.fromhex(h(n)) + label.encode() + struct.pack("<Q", sequence), struct.pack("<I", counter)]
        if self.python_zmq is not None:
            self.socket.send_multipart(frames)
            return
        for i, frame in enumerate(frames):
            assert self.lib.zmq_send(self.socket, frame, len(frame), 2 if i < 2 else 0) == len(frame)

    def close(self):
        if self.python_zmq is not None:
            self.socket.close(0)
            self.context.term()
            return
        self.lib.zmq_close(self.socket)
        self.lib.zmq_ctx_term(self.context)


def main():
    binary = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="bwc-observer-") as directory:
        root = Path(directory)
        cookie = root / ".cookie"
        cookie.write_text("user:first")
        publisher = Publisher(root / "sequence.sock")
        lock = threading.Lock()
        current = {"txids": [], "sequence": 1, "credential": "user:first", "peer_bytes": 100}
        calls = []

        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"
            def log_message(self, *args): pass
            def handle(self):
                try: super().handle()
                except ConnectionError: pass  # The observer deliberately closes connections during recovery.
            def do_POST(self):
                request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                with lock:
                    expected = "Basic " + base64.b64encode(current["credential"].encode()).decode()
                    if self.headers.get("Authorization") != expected:
                        self.send_response(401)
                        self.send_header("Content-Length", "0")
                        self.end_headers()
                        return
                    method = request["method"]
                    calls.append(method)
                    if method == "getblockchaininfo": value = {"chain": "signet", "blocks": 0, "pruneheight": 0, "initialblockdownload": False}
                    elif method == "getnetworkinfo": value = {"version": 320100}
                    elif method in ("getblockhash", "getbestblockhash"): value = h(0)
                    elif method == "getblockheader": value = {"confirmations": 1}
                    elif method == "getrawmempool": value = {"txids": current["txids"], "mempool_sequence": current["sequence"]}
                    elif method == "getzmqnotifications": value = [{"type": "pubsequence", "address": publisher.endpoint}]
                    elif method == "getpeerinfo":
                        current["peer_bytes"] += 8
                        value = [{"id": 4, "conntime": 123, "network": "ipv4", "bytesrecv_per_msg": {"ping": current["peer_bytes"]}}]
                    elif method == "uptime": value = 100
                    else: raise AssertionError(method)
                response = json.dumps({"jsonrpc": "2.0", "id": request["id"], "result": value}).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(response)))
                self.end_headers()
                self.wfile.write(response)

        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        server.daemon_threads = True
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        rpc_url = f"http://127.0.0.1:{server.server_port}"
        environment = {**os.environ, "http_proxy": "http://127.0.0.1:9", "HTTP_PROXY": "http://127.0.0.1:9"}
        capture = root / "capture" / "endpoint"
        capture.mkdir(parents=True)
        record = struct.pack("<Q12sI", 123456789, b"ping", 8) + b"12345678"
        captured = capture / "msgs_recv.dat"
        captured.write_bytes(record[:25])
        database = root / "observe.sqlite"
        process = None
        try:
            with (root / "observer.stderr").open("w") as error:
                process = subprocess.Popen([str(binary), "observe", "--db", str(database), "--cookie", str(cookie), "--rpc-url", rpc_url,
                    "--capture-dir", str(capture.parent), "--peer-interval", "0.1", "--reconcile-interval", "60", "--metadata-budget", "0", "--duration", "15"],
                    stdout=subprocess.DEVNULL, stderr=error, env=environment)
                until(lambda: meta(database, "synchronized"), "initial snapshot")
                until(lambda: rows(database, "SELECT 1 FROM events WHERE cause='capture-incomplete-record'"), "partial capture gap")
                with captured.open("ab") as target: target.write(record[25:])
                until(lambda: rows(database, "SELECT 1 FROM events WHERE cause='message-capture'"), "completed capture record")
                time.sleep(0.3)
                with lock: current.update(txids=[h(10)], sequence=11)
                publisher.send("A", 10, 10, 10)
                until(lambda: rows(database, "SELECT 1 FROM events WHERE kind='mempool.add' AND data LIKE '%notification_sequence%'"), "actual sequence admission")
                # Deliberately skip counter 11, and make the received event older
                # than the snapshot boundary. Only reconciled membership may win.
                with lock: current.update(txids=[h(20)], sequence=30)
                publisher.send("A", 99, 12, 29)
                until(lambda: rows(database, "SELECT 1 FROM events WHERE cause='zmq-counter-gap-or-publisher-restart'"), "transport-gap recovery")
                until(lambda: members(database) == {h(20)}, "exact recovery membership")
                with lock: current.update(txids=[h(20), h(21)], sequence=101)
                publisher.send("A", 21, 13, 100)
                until(lambda: members(database) == {h(20), h(21)}, "inner jump accepted")
                assert len(rows(database, "SELECT 1 FROM events WHERE cause='zmq-counter-gap-or-publisher-restart'")) == 1
                with lock: current.update(txids=[h(21)], sequence=102)
                publisher.send("R", 20, 14, 101)
                until(lambda: rows(database, "SELECT 1 FROM events WHERE kind='mempool.remove' AND cause='unknown-removal'"), "sequence removal")
                until(lambda: members(database) == {h(21)}, "exact removal membership")
                old_run = meta(database, "node_run")
                with lock:
                    current["credential"] = "user:second"
                    cookie.write_text("user:second")
                # Force reconciliation through an outer-counter gap. Periodic
                # snapshots are disabled during injected-event assertions so
                # they cannot legitimately win the race and cover an A/R first.
                publisher.send("A", 99, 16, 101)
                until(lambda: rows(database, "SELECT 1 FROM events WHERE cause='node-restart'"), "credential rotation and session reset")
                assert meta(database, "node_run") != old_run
                until(lambda: all(old_run not in r[0] for r in rows(database, "SELECT id FROM peers")), "peer session refresh")
                assert len(rows(database, "SELECT 1 FROM events WHERE cause='message-capture'")) == 1, "Capture record was duplicated"
                if os.name != "nt": process.send_signal(signal.SIGTERM)
                assert process.wait(timeout=20 if os.name == "nt" else 5) == 0
                assert meta(database, "synchronized") is False
            # Keep stdout unread so the live renderer actually blocks on a full
            # pipe. RPC reconciliation must progress, and shutdown must settle.
            with lock: current.update(txids=[h(n) for n in range(1000, 2000)], sequence=2000)
            live_db = root / "live.sqlite"
            with (root / "live.stderr").open("w") as error:
                process = subprocess.Popen([str(binary), "live", "--db", str(live_db), "--cookie", str(cookie), "--rpc-url", rpc_url,
                    "--peer-interval", "0.1", "--reconcile-interval", "0.2", "--metadata-budget", "0", "--duration", "3"],
                    stdout=subprocess.PIPE, stderr=error, env=environment)
                first = until(lambda: meta(live_db, "last_reconcile_ns"), "live snapshot")
                until(lambda: meta(live_db, "last_reconcile_ns") > first, "reconciliation while stdout unread")
                assert members(live_db) == {h(n) for n in range(1000, 2000)}
                assert process.wait(timeout=5) == 0
                process.stdout.close()
                assert meta(live_db, "synchronized") is False
            assert "getrawmempool" in calls and "getzmqnotifications" in calls
        except Exception:
            for diagnostic in (root / "observer.stderr", root / "live.stderr"):
                if diagnostic.is_file(): print(diagnostic.name + ":\n" + diagnostic.read_text(errors="replace")[-4000:], file=sys.stderr)
            raise
        finally:
            if process and process.poll() is None:
                process.terminate()
                try: process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
            server.shutdown()
            server.server_close()
            thread.join(timeout=2)
            publisher.close()
        print("Observer integration: real IPC ingress, gap recovery, inner jumps, exact membership, cookie/session rotation, capture continuation, proxy bypass and blocked-output shutdown passed")


if __name__ == "__main__": main()
