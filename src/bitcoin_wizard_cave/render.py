"""Independent durable-log reader: a blocked Gource never blocks the collector."""

import json
import sqlite3
import sys
import time
from pathlib import Path

from .model import component


class Renderer:
    def __init__(self, database, mode, output=sys.stdout, max_visible=5000):
        self.db = sqlite3.connect(f"{Path(database).resolve().as_uri()}?mode=ro", uri=True, timeout=10)
        self.mode, self.output, self.max_visible = mode, output, max_visible
        self.visible = set()
        self.cursor = 0
        self.last_time = 0
        self.suppressed = 0

    def line(self, stamp, actor, action, path, color):
        # Paths originate in the model, but this check also protects imported databases.
        if any(c in path for c in "|\n\r") or any(p in ("", ".", "..") for p in path.split("/")):
            raise ValueError("Unsafe Gource path")
        self.last_time = max(self.last_time, int(stamp))
        self.output.write(f"{self.last_time}|{component(actor)}|{action}|{path}|{color}\n")

    def snapshot(self):
        self.db.execute("BEGIN")
        try:
            cursor = self.db.execute("SELECT coalesce(max(id),0) FROM actions").fetchone()[0]
            rows = self.db.execute("SELECT path,actor,color FROM leaves WHERE mode=? ORDER BY path LIMIT ?",
                                   (self.mode, self.max_visible)).fetchall()
            total = self.db.execute("SELECT count(*) FROM leaves WHERE mode=?", (self.mode,)).fetchone()[0]
            coverage = self.db.execute("SELECT value FROM meta WHERE key='coverage_height'").fetchone()
            self.db.commit()
        except Exception:
            self.db.rollback()
            raise
        stamp = int(time.time())
        for path in sorted(self.visible): self.line(stamp, "view:resync", "D", path, "999999")
        self.visible.clear()
        for path, _, color in rows:
            self.line(stamp, "node:snapshot", "A", path, color)
            self.visible.add(path)
        self.cursor = cursor
        self.output.flush()
        print(f"bitcoin-wizard-cave: mode={self.mode} visible={len(rows)}/{total}; "
              f"UTXO creation-height boundary={json.loads(coverage[0]) if coverage else 'not initialized'}; "
              "stock Gource omits metadata; inspect status/events", file=sys.stderr)

    def step(self):
        floor = self.db.execute("SELECT value FROM meta WHERE key='journal_floor'").fetchone()
        event_at_cursor = self.db.execute("SELECT event_id FROM actions WHERE id=?", (self.cursor,)).fetchone()
        if floor and (event_at_cursor is None and self.cursor != 0):
            self.snapshot()
            return 0
        rows = self.db.execute("""SELECT a.id,a.action,a.path,a.actor,a.color,e.observed_ns FROM actions a
            JOIN events e ON e.id=a.event_id WHERE a.id>? AND a.mode=? ORDER BY a.id LIMIT 1000""",
            (self.cursor, self.mode)).fetchall()
        for cursor, action, path, actor, color, stamp in rows:
            self.cursor = cursor
            if action == "D":
                if path not in self.visible: continue
                self.visible.remove(path)
            elif path not in self.visible:
                if len(self.visible) >= self.max_visible:
                    self.suppressed += 1
                    continue
                self.visible.add(path)
                action = "A"
            self.line(stamp // 1_000_000_000, actor, action, path, color)
        if rows: self.output.flush()
        return len(rows)

    def run(self, stop, follow=True):
        try:
            self.snapshot()
            while follow and not stop.is_set():
                if not self.step(): stop.wait(0.1)
        finally:
            print(f"bitcoin-wizard-cave: renderer suppressed {self.suppressed} additions at visible limit", file=sys.stderr)
            self.db.close()
