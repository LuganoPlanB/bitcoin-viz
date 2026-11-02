"""Optional, bounded reader for Core's -capturemessages files (not multiprocess IPC)."""

import hashlib
import os
import struct
from pathlib import Path

from .model import component

HEADER = struct.Struct("<Q12sI")
MAX_PAYLOAD = 4_000_000


def inventory(payload, limit=1000):
    """Keep txid/wtxid distinct; inventory hashes use uint256's wire byte order."""
    if not payload: raise ValueError("Empty inventory")
    first = payload[0]
    width = 0 if first < 253 else {253: 2, 254: 4, 255: 8}[first]
    if len(payload) < 1 + width: raise ValueError("Truncated inventory count")
    count = first if not width else int.from_bytes(payload[1:1 + width], "little")
    offset = 1 + width
    if count > 50_000 or len(payload) != offset + count * 36:
        raise ValueError("Invalid inventory length")
    objects = []
    for i in range(min(count, limit)):
        start = offset + i * 36
        kind = int.from_bytes(payload[start:start + 4], "little") & 0x3FFFFFFF
        objects.append({"kind": {1: "txid", 2: "blockhash", 3: "filtered-blockhash", 4: "compact-blockhash", 5: "wtxid"}.get(kind, "unknown"),
                        "hash": payload[start + 4:start + 36][::-1].hex()})
    return objects, count > limit


class CaptureReader:
    def __init__(self, store, model, directory, budget=100):
        self.s, self.model = store, model
        self.directory = Path(directory)
        if not self.directory.is_dir(): raise ValueError("--capture-dir must exist")
        self.budget = budget
        self.position = 0

    def poll(self):
        remaining = self.budget
        # Core uses <network datadir>/message_capture/<address>/msgs_{recv,sent}.dat.
        paths = sorted(self.directory.glob("*/msgs_*.dat"))
        if not paths: return 0
        start = self.position % len(paths)
        paths = paths[start:] + paths[:start]
        for index, path in enumerate(paths):
            if remaining <= 0: break
            self.position = (start + index + 1) % len(paths)
            if path.name not in ("msgs_recv.dat", "msgs_sent.dat"): continue
            stat = path.stat()
            key = "capture:" + hashlib.sha256(str(path.resolve()).encode()).hexdigest()
            previous = self.s.get(key, {})
            identity = [stat.st_dev, stat.st_ino]
            offset = previous.get("offset", 0)
            with self.s.db:
                if previous and (previous.get("identity") != identity or stat.st_size < offset):
                    self.s.event("observer.source-loss", cause="capture-rotation-or-truncation", file=str(path))
                    offset = 0
                    previous = {}
                previous = {**previous, "offset": offset, "identity": identity}
                with path.open("rb") as source:
                    source.seek(offset)
                    while remaining > 0:
                        header = source.read(HEADER.size)
                        if not header: break
                        if len(header) != HEADER.size:
                            self._partial(key, previous, path)
                            break
                        timestamp_us, raw_command, size = HEADER.unpack(header)
                        if size > MAX_PAYLOAD:
                            self._partial(key, previous, path, "oversize-or-invalid-capture")
                            break  # No guessed framing after corruption.
                        if source.tell() + size > os.fstat(source.fileno()).st_size:
                            self._partial(key, previous, path)
                            break  # Offset stays at last complete record, retry next poll.
                        command = raw_command.split(b"\0", 1)[0].decode("ascii", errors="replace")
                        objects = None
                        if command in ("inv", "getdata", "notfound"):
                            payload = source.read(size)
                            try:
                                objects, sampled = inventory(payload)
                                if sampled:
                                    self.s.event("observer.coverage", cause="inventory-link-sampling", file=str(path), limit=1000)
                            except ValueError as error:
                                self.s.event("observer.source-loss", cause="incomplete-inventory", error=str(error), file=str(path))
                        else:
                            source.seek(size, 1)  # Do not copy full blocks/transactions just to count bytes.
                        peer = f"capture-{self.s.get('node_run')}-{component(path.parent.name)}-{stat.st_ino}"
                        self.model.message(peer, "incoming" if path.name == "msgs_recv.dat" else "outgoing",
                                           command, size, observed_ns=timestamp_us * 1000, objects=objects)
                        offset = source.tell()
                        remaining -= 1
                        previous = {"offset": offset, "identity": identity}
                        self.s.put(key, previous)
        return self.budget - remaining

    def _partial(self, key, previous, path, cause="capture-incomplete-record"):
        if not previous.get("partial"):
            self.s.event("observer.source-loss", cause=cause, file=str(path))
        self.s.put(key, {**previous, "partial": True})
