"""Local cookie-authenticated RPC and the binary ZMQ sequence protocol."""

import base64
import hashlib
import http.client
import json
import queue
import struct
import threading
from dataclasses import dataclass
from decimal import Decimal
from pathlib import Path
from urllib.parse import urlsplit


class RPCError(RuntimeError):
    def __init__(self, method, code, message):
        super().__init__(f"{method}: {code}: {message}")
        self.code = code


class RPC:
    def __init__(self, url, cookie, timeout=10):
        parsed = urlsplit(url)
        if parsed.scheme != "http" or parsed.hostname not in ("localhost", "127.0.0.1", "::1") or parsed.username or parsed.password:
            raise ValueError("RPC must be a local http:// loopback URL without embedded credentials")
        self.host, self.port = parsed.hostname, parsed.port or 38332
        self.path = parsed.path or "/"
        self.cookie = Path(cookie)
        self.timeout = timeout
        self.connection = None
        self.cookie_marker = None
        self.serial = 0

    def call(self, method, *params):
        # Core rotates cookies on restart. Never log their contents.
        credential = self.cookie.read_text().strip()
        self.cookie_marker = hashlib.sha256(credential.encode()).digest()
        auth = base64.b64encode(credential.encode()).decode()
        self.serial += 1
        body = json.dumps({"jsonrpc": "2.0", "id": self.serial, "method": method, "params": params})
        if self.connection is None:
            self.connection = http.client.HTTPConnection(self.host, self.port, timeout=self.timeout)
        try:
            self.connection.request("POST", self.path, body, {"Authorization": "Basic " + auth, "Content-Type": "application/json"})
            response = self.connection.getresponse()
            raw = response.read()
            if response.status == 401:
                raise RPCError(method, 401, "cookie rejected; check node datadir/network")
            if response.status not in (200, 500):
                raise RPCError(method, response.status, "unexpected HTTP response")
            result = json.loads(raw, parse_float=Decimal)
            if result.get("error"):
                error = result["error"]
                raise RPCError(method, error["code"], error["message"])
            if result.get("id") != self.serial: raise RPCError(method, -1, "response id mismatch")
            return result["result"]
        except (OSError, http.client.HTTPException, ValueError):
            self.close()
            raise

    def close(self):
        if self.connection: self.connection.close()
        self.connection = None


@dataclass(frozen=True)
class Sequence:
    object_hash: str
    label: str
    counter: int
    mempool_sequence: int | None


def parse_sequence(frames):
    if len(frames) != 3 or frames[0] != b"sequence" or len(frames[2]) != 4:
        raise ValueError("Expected sequence/body/uint32 multipart frames")
    body = frames[1]
    if len(body) not in (33, 41): raise ValueError("Invalid sequence body length")
    label = chr(body[32])
    if label not in "ARCD" or len(body) != (41 if label in "AR" else 33):
        raise ValueError("Invalid sequence label/body combination")
    # Core explicitly reverses uint256's internal storage before sending.
    # The wire bytes are already in RPC/display order: do NOT reverse again.
    return Sequence(body[:32].hex(), label, struct.unpack("<I", frames[2])[0],
                    struct.unpack("<Q", body[33:])[0] if label in "AR" else None)


def endpoint(value):
    if value.startswith("unix:"): return "ipc://" + value[5:]
    if value.startswith("ipc://") or value.startswith("tcp://127.0.0.1:") or value.startswith("tcp://[::1]:"):
        return value
    raise ValueError("Use unix:/absolute/path, ipc:///absolute/path or loopback tcp:// for ZMQ")


class SequenceFeed:
    """Receive independently of RPC and rendering; overflow is observable loss."""
    def __init__(self, address, capacity=8192):
        self.address = endpoint(address)
        self.queue = queue.Queue(capacity)
        self.dropped = 0
        self.error = None
        self.stop = threading.Event()
        self.ready = threading.Event()
        self.thread = threading.Thread(target=self._run, daemon=True, name="sequence-receiver")

    def start(self):
        self.thread.start()
        if not self.ready.wait(5): raise RuntimeError("ZMQ subscriber startup timed out")
        if self.error: raise RuntimeError(self.error)

    def _run(self):
        import zmq
        context = zmq.Context()
        sock = context.socket(zmq.SUB)
        sock.setsockopt(zmq.SUBSCRIBE, b"sequence")
        sock.setsockopt(zmq.RCVHWM, self.queue.maxsize)
        sock.setsockopt(zmq.LINGER, 0)
        try:
            sock.connect(self.address)
            self.ready.set()
            while not self.stop.is_set():
                if not sock.poll(100): continue
                frames = sock.recv_multipart()
                try:
                    self.queue.put_nowait(parse_sequence(frames))
                except (ValueError, queue.Full):
                    self.dropped += 1
        except Exception as error:
            self.error = str(error)
            self.ready.set()
        finally:
            sock.close()
            context.term()

    def close(self):
        self.stop.set()
        self.thread.join(3)
