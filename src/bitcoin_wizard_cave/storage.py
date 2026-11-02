"""One SQLite transaction commits model, inverse patches and presentation together."""

import json
import sqlite3
import time
from pathlib import Path


def encode(value):
    return json.dumps(value, default=str, separators=(",", ":"), sort_keys=True)


class Store:
    def __init__(self, path):
        self.path = str(path)
        Path(path).parent.mkdir(parents=True, exist_ok=True)
        self.db = sqlite3.connect(path, timeout=10)
        self.db.row_factory = sqlite3.Row
        self.db.execute("PRAGMA journal_mode=WAL")
        self.db.execute("PRAGMA synchronous=FULL")
        self.db.executescript("""
            CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value TEXT NOT NULL);
            CREATE TABLE IF NOT EXISTS mempool(txid TEXT PRIMARY KEY, data TEXT NOT NULL);
            CREATE TABLE IF NOT EXISTS peers(id TEXT PRIMARY KEY, data TEXT NOT NULL);
            CREATE TABLE IF NOT EXISTS coins(id TEXT PRIMARY KEY, height INTEGER NOT NULL, data TEXT NOT NULL);
            CREATE INDEX IF NOT EXISTS coins_height ON coins(height);
            CREATE TABLE IF NOT EXISTS blocks(hash TEXT PRIMARY KEY, height INTEGER NOT NULL,
                parent TEXT NOT NULL, undo TEXT NOT NULL, coverage INTEGER NOT NULL);
            CREATE TABLE IF NOT EXISTS leaves(mode TEXT NOT NULL, path TEXT NOT NULL,
                actor TEXT NOT NULL, color TEXT NOT NULL, data TEXT NOT NULL,
                PRIMARY KEY(mode,path));
            CREATE TABLE IF NOT EXISTS events(id INTEGER PRIMARY KEY AUTOINCREMENT,
                observed_ns INTEGER NOT NULL, node_run TEXT, kind TEXT NOT NULL,
                actor TEXT NOT NULL, cause TEXT, data TEXT NOT NULL);
            CREATE TABLE IF NOT EXISTS actions(id INTEGER PRIMARY KEY AUTOINCREMENT,
                event_id INTEGER NOT NULL, mode TEXT NOT NULL, action TEXT NOT NULL,
                path TEXT NOT NULL, actor TEXT NOT NULL, color TEXT NOT NULL);
            CREATE INDEX IF NOT EXISTS actions_event ON actions(event_id);
        """)
        schema = self.get("schema")
        if schema not in (None, 1):
            raise ValueError(f"Unsupported observer database schema {schema}")
        with self.db:
            self.put("schema", 1)

    def get(self, key, default=None):
        row = self.db.execute("SELECT value FROM meta WHERE key=?", (key,)).fetchone()
        return json.loads(row[0]) if row else default

    def put(self, key, value):
        self.db.execute("INSERT OR REPLACE INTO meta VALUES(?,?)", (key, encode(value)))

    def event(self, kind, actor="node:observer", cause=None, **data):
        if kind == "observer.source-loss":
            self.put("source_loss_count", self.get("source_loss_count", 0) + 1)
            self.put("last_source_loss", {"observed_ns": time.time_ns(), "cause": cause, **data})
        return self.db.execute(
            "INSERT INTO events(observed_ns,node_run,kind,actor,cause,data) VALUES(?,?,?,?,?,?)",
            (time.time_ns(), self.get("node_run"), kind, actor, cause, encode(data)),
        ).lastrowid

    def leaf(self, mode, path, actor, color, data, event, *, delete=False, pulse=False):
        old = self.db.execute("SELECT data FROM leaves WHERE mode=? AND path=?", (mode, path)).fetchone()
        if delete:
            if old is None:
                return
            self.db.execute("DELETE FROM leaves WHERE mode=? AND path=?", (mode, path))
            action = "D"
        else:
            serialized = encode(data)
            if old and old[0] == serialized and not pulse:
                return
            action = "M" if old else "A"
            self.db.execute("INSERT OR REPLACE INTO leaves VALUES(?,?,?,?,?)",
                            (mode, path, actor, color, serialized))
        self.db.execute("INSERT INTO actions(event_id,mode,action,path,actor,color) VALUES(?,?,?,?,?,?)",
                        (event, mode, action, path, actor, color))

    def prune(self, keep):
        """A renderer lagging behind this boundary must repopulate its scene."""
        if keep <= 0:
            return
        last = self.db.execute("SELECT max(id) FROM events").fetchone()[0] or 0
        boundary = last - keep
        self.db.execute("DELETE FROM actions WHERE event_id<=?", (boundary,))
        self.db.execute("DELETE FROM events WHERE id<=?", (boundary,))
        self.put("journal_floor", max(0, boundary))

    def close(self):
        self.db.close()
