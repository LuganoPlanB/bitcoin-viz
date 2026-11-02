import io
import json
import queue
import struct
import tempfile
import threading
import time
import unittest
from pathlib import Path
from unittest.mock import patch

from bitcoin_wizard_cave.capture import CaptureReader, HEADER, inventory
from bitcoin_wizard_cave.core import RPC, Sequence, endpoint, parse_sequence
from bitcoin_wizard_cave.model import Model, RebuildRequired, component, fee_metadata
from bitcoin_wizard_cave.observer import Observer, writer_lock
from bitcoin_wizard_cave.render import Renderer
from bitcoin_wizard_cave.storage import Store


def h(number): return f"{number:064x}"


def tx(number, inputs=(), outputs=1, script="51"):
    return {"txid": h(number), "vin": [{"txid": h(n), "vout": v} for n, v in inputs],
            "vout": [{"n": n, "value": "0.00000100", "scriptPubKey": {"hex": script}} for n in range(outputs)]}


def block(height, transactions=(), parent=None, number=None):
    return {"hash": h(number or height), "previousblockhash": parent or h(height - 1),
            "height": height, "tx": list(transactions), "time": 123}


class ModelContract(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.path = Path(self.temp.name) / "observer.sqlite"
        self.store = Store(self.path)
        self.model = Model(self.store)
        with self.store.db:
            self.store.put("node_run", "test")
            self.model.checkpoint(h(0), 0, "test")

    def tearDown(self):
        self.store.close()
        self.temp.cleanup()

    def count(self, table): return self.store.db.execute(f"SELECT count(*) FROM {table}").fetchone()[0]

    def causes(self): return [r[0] for r in self.store.db.execute("SELECT cause FROM events")]

    def test_confirmation_removes_only_included_memberships(self):
        with self.store.db:
            for n in (10, 11, 12): self.model.admission(h(n))
            self.model.connect(block(1, [tx(10)]))
        self.assertEqual({r[0] for r in self.store.db.execute("SELECT txid FROM mempool")}, {h(11), h(12)})
        self.assertIn("confirmation", self.causes())

    def test_replacement_removes_multiple_distinct_identities(self):
        with self.store.db:
            for n in (10, 11): self.model.admission(h(n))
            for n in (10, 11): self.model.removal(h(n), "replacement-observed")
            self.model.admission(h(12))
        self.assertEqual(self.count("mempool"), 1)
        self.assertEqual(self.store.db.execute("SELECT txid FROM mempool").fetchone()[0], h(12))

    def test_reorg_never_invents_readmission(self):
        with self.store.db:
            self.model.admission(h(10))
            self.model.connect(block(1, [tx(10)]))
            self.model.disconnect(h(1))
        self.assertEqual(self.count("mempool"), 0)
        self.assertEqual(self.count("coins"), 0)
        with self.store.db: self.model.reconcile_mempool([h(10)], 77)
        self.assertEqual(self.count("mempool"), 1)

    def test_intrablock_create_spend_undo_leaves_no_residue(self):
        with self.store.db:
            self.model.connect(block(1, [tx(10), tx(11, [(10, 0)])]))
        self.assertEqual({r[0] for r in self.store.db.execute("SELECT id FROM coins")}, {h(11) + ":0"})
        with self.store.db: self.model.disconnect(h(1))
        self.assertEqual(self.count("coins"), 0)

    def test_undo_restores_full_previous_coin(self):
        with self.store.db: self.model.connect(block(1, [tx(10)]))
        before = self.store.db.execute("SELECT data FROM coins").fetchone()[0]
        with self.store.db:
            self.model.connect(block(2, [tx(11, [(10, 0)])]))
            self.model.disconnect(h(2))
        self.assertEqual(self.store.db.execute("SELECT data FROM coins").fetchone()[0], before)

    def test_pending_overlay_does_not_spend_confirmed_coin(self):
        with self.store.db:
            self.model.connect(block(1, [tx(10)]))
            self.model.admission(h(11))
            self.model.pending(h(11), tx(11, [(10, 0)]))
        self.assertEqual(self.count("coins"), 1)
        self.assertEqual(self.store.db.execute("SELECT count(*) FROM leaves WHERE path LIKE 'pending/%'").fetchone()[0], 1)
        with self.store.db: self.model.removal(h(11))
        self.assertEqual(self.count("coins"), 1)
        self.assertEqual(self.store.db.execute("SELECT count(*) FROM leaves WHERE path LIKE 'pending/%'").fetchone()[0], 0)

    def test_late_metadata_does_not_resurrect_membership(self):
        with self.store.db:
            self.model.admission(h(10))
            self.model.removal(h(10))
            self.model.enrich(h(10), {"band": "fee-20-plus"})
            self.model.pending(h(10), tx(10))
        self.assertEqual(self.count("mempool"), 0)
        self.assertEqual(self.store.db.execute("SELECT count(*) FROM leaves WHERE path LIKE 'mempool/%'").fetchone()[0], 0)

    def test_atomic_rollback_covers_tip_coins_journal_and_actions(self):
        events = self.count("events")
        with self.assertRaises(RuntimeError):
            with self.store.db:
                self.model.connect(block(1, [tx(10)]))
                raise RuntimeError("simulated interruption before commit")
        self.assertEqual(self.store.get("tip")["hash"], h(0))
        self.assertEqual(self.count("coins"), 0)
        self.assertEqual(self.count("actions"), 0)
        self.assertEqual(self.count("events"), events)

    def test_display_expiry_is_distinct_and_reversible(self):
        self.model.window = 1
        with self.store.db:
            self.model.connect(block(1, [tx(10)]))
            self.model.connect(block(2))
        self.assertEqual(self.count("coins"), 0)
        self.assertIn("coverage-expiry", self.causes())
        self.assertNotIn("spend", self.causes())
        with self.store.db: self.model.disconnect(h(2))
        self.assertEqual(self.count("coins"), 1)
        self.assertEqual(self.store.get("coverage_height"), 1)

    def test_horizon_limit_requires_rebuild(self):
        self.model.undo_horizon = 1
        with self.store.db:
            self.model.connect(block(1, [tx(10)]))
            self.model.connect(block(2))
            self.model.disconnect(h(2))
        with self.assertRaisesRegex(RebuildRequired, "horizon"):
            self.model.disconnect(h(1))

    def test_parent_mismatch_cannot_apply_block(self):
        with self.assertRaisesRegex(RebuildRequired, "parent"):
            self.model.connect(block(2, [tx(10)]))
        self.assertEqual(self.count("coins"), 0)

    def test_unspendable_outputs_are_excluded(self):
        with self.store.db:
            self.model.connect(block(1, [tx(10, script="6a0100"), tx(11, script="51" * 10_001), tx(12)]))
        self.assertEqual(self.count("coins"), 1)

    def test_render_restart_repopulates_current_scene(self):
        with self.store.db: self.model.admission(h(10))
        output = io.StringIO()
        renderer = Renderer(self.path, "state", output)
        renderer.snapshot()
        renderer.db.close()
        self.assertIn("|node:snapshot|A|mempool/", output.getvalue())

    def test_renderer_limit_does_not_truncate_model(self):
        with self.store.db:
            for n in range(10, 15): self.model.admission(h(n))
        renderer = Renderer(self.path, "state", io.StringIO(), max_visible=2)
        renderer.snapshot()
        self.assertEqual(len(renderer.visible), 2)
        self.assertEqual(self.count("mempool"), 5)
        renderer.db.close()

    def test_slow_renderer_does_not_hold_sqlite_read_lock(self):
        with self.store.db: self.model.admission(h(10))
        entered, release = threading.Event(), threading.Event()
        failures = []
        class SlowOutput:
            def write(self, text):
                entered.set()
                if not release.wait(3): raise RuntimeError("test failed to release output")
            def flush(self): pass
        def render():
            renderer = Renderer(self.path, "state", SlowOutput())
            try: renderer.snapshot()
            except Exception as error: failures.append(error)
            finally: renderer.db.close()
        thread = threading.Thread(target=render)
        thread.start()
        try:
            self.assertTrue(entered.wait(2), "renderer never reached blocked output")
            with self.store.db: self.model.admission(h(11))
            self.assertEqual(self.count("mempool"), 2)
        finally:
            release.set()
            thread.join(4)
        self.assertFalse(thread.is_alive())
        self.assertEqual(failures, [])

    def test_pruned_journal_rebuilds_slow_renderer_scene(self):
        with self.store.db: self.model.admission(h(10))
        renderer = Renderer(self.path, "state", io.StringIO())
        renderer.snapshot()
        with self.store.db:
            self.model.removal(h(10))
            self.model.admission(h(11))
            self.store.prune(1)
        renderer.step()
        self.assertEqual(renderer.visible, {f"mempool/fee-unknown/{h(11)}.tx"})
        renderer.db.close()

    def test_peer_counters_are_deltas_not_initial_history(self):
        peer = {"id": 4, "conntime": 123, "bytesrecv_per_msg": {"inv": 100}}
        with self.store.db:
            self.model.peers([peer], "run")
            peer["bytesrecv_per_msg"]["inv"] = 160
            self.model.peers([peer], "run")
        rows = self.store.db.execute("SELECT data FROM events WHERE cause='aggregate-bytes'").fetchall()
        self.assertEqual(len(rows), 1)
        self.assertEqual(json.loads(rows[0][0])["size_bytes"], 60)
        self.assertTrue(self.store.db.execute("SELECT 1 FROM leaves WHERE path LIKE 'traffic/unclassified/inv/%'").fetchone())

    def test_new_run_does_not_reuse_peer_identity(self):
        peer = {"id": 4, "conntime": 123}
        with self.store.db:
            self.model.peers([peer], "run1")
            self.model.peers([peer], "run2")
        self.assertIn("run2", self.store.db.execute("SELECT id FROM peers").fetchone()[0])
        self.assertEqual(self.count("peers"), 1)

    def test_transport_gap_triggers_snapshot_and_skips_stale_event(self):
        observer = Observer(self.store, None)
        observer.counter = 9
        def reconcile(cause):
            self.assertIn("gap", cause)
            with self.store.db: self.model.reconcile_mempool([h(11)], 100)
        with patch.object(observer, "reconcile", side_effect=reconcile) as recovery:
            observer.handle(Sequence(h(10), "A", 12, 99))
        recovery.assert_called_once()
        self.assertEqual({r[0] for r in self.store.db.execute("SELECT txid FROM mempool")}, {h(11)})

    def test_transport_wrap_and_inner_jump_do_not_invent_loss(self):
        observer = Observer(self.store, None)
        observer.counter = 0xFFFFFFFF
        with patch.object(observer, "reconcile") as recovery:
            with self.store.db: self.store.put("mempool_boundary", 10)
            observer.handle(Sequence(h(10), "A", 0, 100))
        recovery.assert_not_called()
        self.assertEqual(self.count("mempool"), 1)

    def test_snapshot_boundary_is_next_sequence_not_last_applied(self):
        observer = Observer(self.store, None)
        with self.store.db: self.model.reconcile_mempool([], 25)
        observer.handle(Sequence(h(10), "A", 0, 25))
        observer.handle(Sequence(h(11), "A", 1, 24))
        self.assertEqual({r[0] for r in self.store.db.execute("SELECT txid FROM mempool")}, {h(10)})

    def test_capture_partial_records_resume_once_and_show_loss(self):
        directory = Path(self.temp.name) / "message_capture"
        peer = directory / "endpoint"
        peer.mkdir(parents=True)
        path = peer / "msgs_recv.dat"
        record = HEADER.pack(123456789, b"ping", 8) + b"12345678"
        path.write_bytes(record[:25])
        reader = CaptureReader(self.store, self.model, directory)
        self.assertEqual(reader.poll(), 0)
        self.assertEqual(reader.poll(), 0)
        self.assertEqual(self.causes().count("capture-incomplete-record"), 1)
        with path.open("ab") as stream: stream.write(record[25:])
        self.assertEqual(reader.poll(), 1)
        self.assertEqual(reader.poll(), 0)
        self.assertEqual(self.causes().count("message-capture"), 1)

    def test_inventory_observation_touches_existing_leaf_without_admission(self):
        with self.store.db:
            self.model.admission(h(10))
            self.model.message("captured-peer", "incoming", "inv", 73,
                               objects=[{"kind": "txid", "hash": h(10)}, {"kind": "txid", "hash": h(11)}])
        self.assertEqual(self.count("mempool"), 1)
        self.assertTrue(self.store.db.execute("SELECT 1 FROM actions WHERE mode='state' AND action='M'").fetchone())

    def test_capture_poll_is_fair_across_busy_files(self):
        directory = Path(self.temp.name) / "capture"
        peer = directory / "endpoint"
        peer.mkdir(parents=True)
        record = HEADER.pack(1234, b"ping", 0)
        (peer / "msgs_recv.dat").write_bytes(record * 5)
        (peer / "msgs_sent.dat").write_bytes(record * 5)
        reader = CaptureReader(self.store, self.model, directory, budget=1)
        reader.poll()
        reader.poll()
        kinds = {r[0] for r in self.store.db.execute("SELECT kind FROM events WHERE cause='message-capture'")}
        self.assertEqual(kinds, {"peer.message.incoming", "peer.message.outgoing"})

    def test_pruned_history_reanchors_with_explicit_rebuild(self):
        from bitcoin_wizard_cave.core import RPCError
        class FakeRPC:
            def call(self, method, *params):
                if method == "getblockchaininfo": return {"chain": "signet", "blocks": 2, "pruneheight": 1}
                if method == "getblockheader": return {"confirmations": 1}
                if method == "getblockhash": return h(params[0])
                if method == "getblock": raise RPCError(method, -1, "Block not available (pruned data)")
                raise AssertionError(method)
        observer = Observer(self.store, FakeRPC(), bootstrap=1)
        with self.assertRaises(RebuildRequired): observer.sync_chain()
        self.assertEqual(self.store.get("checkpoint"), {"height": 1, "hash": h(1)})
        self.assertIn("pruned-or-unavailable-history", self.causes())

    def test_moving_snapshot_tip_does_not_claim_synchronization(self):
        class FakeRPC:
            cookie_marker = b"same"
            def call(self, method, *params):
                if method == "uptime": return 50
                if method == "getbestblockhash": return h(2)
                if method == "getrawmempool": return {"txids": [h(10)], "mempool_sequence": 5}
                raise AssertionError(method)
        observer = Observer(self.store, FakeRPC())
        with patch.object(observer, "sync_chain", return_value={"height": 1, "hash": h(1)}):
            with self.assertRaisesRegex(RebuildRequired, "snapshot attempts"): observer.reconcile("test")
        self.assertEqual(self.count("mempool"), 0)
        self.assertIsNone(self.store.get("synchronized"))

    def test_writer_lock_prevents_competing_observers(self):
        with writer_lock(self.path):
            with self.assertRaisesRegex(RuntimeError, "already owns"):
                with writer_lock(self.path): pass


class WireContract(unittest.TestCase):
    def test_sequence_hash_is_already_display_order_and_counters_are_little_endian(self):
        identifier = bytes(range(32))
        event = parse_sequence([b"sequence", identifier + b"A" + struct.pack("<Q", 2**60 + 7), struct.pack("<I", 12)])
        self.assertEqual(event.object_hash, identifier.hex())
        self.assertEqual(event.mempool_sequence, 2**60 + 7)
        self.assertEqual(event.counter, 12)

    def test_malformed_multipart_is_rejected(self):
        for frames in ([b"sequence"], [b"sequence", b"x" * 33, b"1234"],
                       [b"sequence", bytes(32) + b"A", b"1234"]):
            with self.subTest(frames=frames):
                with self.assertRaises(ValueError): parse_sequence(frames)

    def test_current_and_old_fee_sizes_keep_honest_labels(self):
        self.assertEqual(fee_metadata({"fees": {"base": "0.00001"}, "vsize_bip141": 200})["band"], "fee-5-to-20")
        derived = fee_metadata({"fees": {"base": "0.00001"}, "weight": 401})
        self.assertEqual(derived["vsize_bip141"], 101)
        self.assertEqual(derived["size_source"], "ceil(weight/4)")
        self.assertEqual(fee_metadata({"fees": {"base": "0.00001"}, "vsize": 101})["band"], "fee-unknown")

    def test_cookie_is_reread_on_every_rpc_request(self):
        with tempfile.TemporaryDirectory() as directory:
            cookie = Path(directory) / ".cookie"
            cookie.write_text("user:first")
            rpc = RPC("http://127.0.0.1:38332", cookie)
            class Connection:
                headers = []
                def request(self, method, path, body, headers): self.headers.append(headers)
                def getresponse(self):
                    class Response:
                        status = 200
                        def read(self): return json.dumps({"id": rpc.serial, "result": 1}).encode()
                    return Response()
            rpc.connection = Connection()
            rpc.call("uptime")
            cookie.write_text("user:second")
            rpc.call("uptime")
            self.assertNotEqual(rpc.connection.headers[0]["Authorization"], rpc.connection.headers[1]["Authorization"])

    def test_inventory_hash_order_and_identity_types(self):
        payload = b"\x02" + struct.pack("<I", 1) + bytes.fromhex(h(10))[::-1] + struct.pack("<I", 5) + bytes.fromhex(h(11))[::-1]
        objects, sampled = inventory(payload)
        self.assertEqual(objects, [{"kind": "txid", "hash": h(10)}, {"kind": "wtxid", "hash": h(11)}])
        self.assertFalse(sampled)
        with self.assertRaises(ValueError): inventory(payload[:-1])

    def test_sanitization_blocks_field_and_hierarchy_injection(self):
        cleaned = component("evil|peer\n../../user/name")
        self.assertFalse(any(c in cleaned for c in "|\n/"))
        self.assertNotEqual(component("a/b"), component("a|b"))
        self.assertNotEqual(component(".."), "..")

    def test_only_local_rpc_and_zmq_are_accepted(self):
        with self.assertRaises(ValueError): RPC("http://remote.example:8332", "/tmp/cookie")
        self.assertEqual(endpoint("unix:/tmp/sequence.sock"), "ipc:///tmp/sequence.sock")
        with self.assertRaises(ValueError): endpoint("tcp://0.0.0.0:28332")


if __name__ == "__main__": unittest.main()
