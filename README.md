# Bitcoin Wizard Cave

A small external observer for Bitcoin Core, inspired by Dowse's ambient DNS
visualization. It connects to Core's **ZMQ sequence publisher over a Unix-domain
socket**, uses **local cookie-authenticated RPC**, and feeds **Gource through stdin**.
Signet is the default. Bitcoin Core and Gource do not need patches.

Recorded signet previews: [working state](docs/previews/state.mp4),
[protocol](docs/previews/protocol.mp4), [UTXO window](docs/previews/utxo.mp4).
The [verification notes](docs/VERIFICATION.md) explain their coverage.

| Mode | The scene shows | Sources |
|---|---|---|
| `state` | Peer sessions, fee-banded local mempool, recent active and disconnected blocks | Sequence + RPC |
| `protocol` | Incoming/outgoing transaction, block, discovery, control and unclassified traffic | Per-peer RPC byte deltas; optional Core message capture |
| `utxo` | Confirmed output creation/spending, reorg undo, separate pending-spend leaves | Decoded connected blocks + mempool metadata |

Compared with Dowse's direct event-to-line adapter, the observer retains stable
object identities, a normalized journal, exact mempool reconciliation, reversible
block updates, and three independent projections. SQLite commits state, undo
patches and presentation actions together. A renderer reads through a separate
connection/process, so a slow Gource cannot block collection.

## Install and try it

Requires Python 3.11+, `pyzmq`, and Gource. No C build is needed.

```sh
cd bitcoin-wizard-cave
python3 -m venv .venv
.venv/bin/python -m pip install -e .
```

This workspace already has `.venv/bin/bitcoin-wizard-cave` installed. On a minimal
Debian Python without `ensurepip`, create the venv with `--without-pip` and use
the official [pip zip application](https://pip.pypa.io/en/stable/installation/#standalone-zip-application)
to install into it. The local `.tools/pip.pyz` was used for this environment.

Download the pinned official Linux x86_64 Core binary, then start an isolated
signet node in its own terminal:

```sh
python3 tools/core.py fetch
python3 tools/core.py start
```

The helper uses `.runtime/signet`, bounded Core caches, pruning, loopback RPC,
cookie authentication and `.runtime/socket/sequence.sock`. Initial synchronization
still downloads and validates signet history. Run `python3 tools/core.py info`
to inspect progress, and `python3 tools/core.py stop` to stop this node.

In another terminal:

```sh
.venv/bin/bitcoin-wizard-cave live --datadir "$PWD/.runtime/signet" --mode state |
  gource --log-format custom --realtime --disable-auto-skip \
    --file-idle-time 0 --max-file-lag 0.1 --title 'Bitcoin Wizard Cave · Signet' -
```

Select `--mode protocol` or `--mode utxo` for the other views. Use a UTXO title
such as `Bitcoin Wizard Cave · observed output window` to keep coverage visible.
Diagnostics go to stderr; stdout contains only valid custom-log records.

For an existing node, use its data directory or explicit `--cookie`, `--rpc-url`
and `--zmq`. The observer verifies the RPC chain and fails if it is not signet.
`--network regtest` exists only for controlled testing; mainnet is intentionally
outside this first release's CLI.

## Unix socket configuration

See [examples/bitcoin.conf](examples/bitcoin.conf). With an existing Core:

```ini
server=1
signet=1
[signet]
zmqpubsequence=unix:/run/user/1000/bitcoin-vis/sequence.sock
```

Create the directory with suitable ownership/permissions first; `1000` is an
example UID. The libzmq subscriber connects to
`ipc:///run/user/1000/bitcoin-vis/sequence.sock`. Core translates `unix:` to
`ipc://`; the CLI accepts either spelling and normally discovers the publisher
with `getzmqnotifications`. This is binary multipart ZMQ, not a text pipe. It is
separate from Core's multiprocess IPC. The **observer-to-Gource** connection is
the ordinary text pipe.

## Keep collecting while changing views

Run collection independently for renderer restarts and mode switching:

```sh
.venv/bin/bitcoin-wizard-cave observe --datadir "$PWD/.runtime/signet" --db .runtime/wizard.sqlite
```

Then, in one or more other terminals:

```sh
.venv/bin/bitcoin-wizard-cave render --db .runtime/wizard.sqlite --mode utxo |
  gource --log-format custom --realtime --disable-auto-skip --file-idle-time 0 -
```

Each renderer quietly repopulates the current scene as `node:snapshot`, then
follows committed actions. `render --once` exports the current scene. `live`
combines a collector with an independent renderer thread for the convenient
single-pipe invocation; use `observe` for a collector whose lifetime should
survive renderer exit.

```sh
.venv/bin/bitcoin-wizard-cave status --db .runtime/wizard.sqlite
.venv/bin/bitcoin-wizard-cave events --db .runtime/wizard.sqlite --after 0 --limit 100
```

The JSON status includes the applied tip, checkpoint, creation-height coverage,
state and visible-leaf counts, Core version and reconciliation time. Journal
records preserve nanosecond observation time, source counters, original causes,
dependencies, money in integer satoshis, and source metadata. Schema version is 1.
`events --after ID` permits incremental inspection; it does not promise a global
causal order among independent sources.

## Protocol detail

The default protocol view polls `bytesrecv_per_msg` and `bytessent_per_msg` every
two seconds. These are **byte deltas, not message counts**. Initial cumulative
counters are a baseline, not replayed traffic. `inv`/`getdata` remain unclassified
when only their names are available. Very short peer sessions may be missed.

For actual message observations, start the helper with `--capture`, then run:

```sh
.venv/bin/bitcoin-wizard-cave observe --datadir "$PWD/.runtime/signet" \
  --capture-dir "$PWD/.runtime/signet/signet/message_capture"
```

Core's optional `-capturemessages` writes full P2P payloads to disk. The observer
reads a bounded number of complete records each poll, journals metadata and
selected inventory links, and persists each file cursor atomically. It suppresses
aggregate animation to avoid double counting. Inventory parsing distinguishes
txids, wtxids and block hashes; only known txids touch existing mempool leaves.
Unknown content never creates a membership. Mixed inventory has its own branch.
Partial/corrupt records, truncations and rotations are journaled as source gaps;
the reader waits for completion rather than guessing record boundaries.

Capture files identify an endpoint, not Core's peer ID. Their actors are labeled
`capture-...`, include observer-run and file identity, and are not falsely joined
to a polled connection. Existing file records are read from the beginning once,
with original source time retained separately; a large backlog is not live
traffic. The observer does not rotate or delete Core's capture files. Inventory
detail is capped at 1,000 links per message and sampling is explicitly journaled.
No eBPF privileges, trace probe installation or full transaction payload retention
in the observer database are required.

## Coverage, recovery and presentation

- The UTXO view is **observed outputs**, not the full current UTXO set. Startup
  anchors six blocks behind the node's current tip and applies later blocks.
  Default `--window-blocks 144` expires older creation cohorts with
  `view:retention` / `coverage-expiry`, never as a spend. `--window-blocks 0`
  retains all observed surviving outputs from a fixed checkpoint and may grow.
- Per-block inverse patches undo operations in reverse order, including outputs
  created then spent inside the same block. `--undo-horizon 288` bounds undo
  history. A reorg crossing that horizon/checkpoint, missing pruned content, or
  excessive rolling-window catch-up records a rebuild and a new coverage anchor.
- During Core's initial download the applied chain is only the node's partially
  synchronized chain. If pruning/catch-up outruns the observer it reports gaps
  and retries; it does not promise complete historical playback. A duration limit
  is checked between bounded RPC/catch-up batches, so an in-flight batch may finish
  after the requested time.
- The subscriber buffers before the mempool snapshot. Chain-tip bracketing,
  the snapshot's **next** mempool sequence boundary, modulo-32-bit outer counters,
  periodic exact membership comparison and cookie refresh support recovery.
  Inner mempool counter jumps alone do not imply transport loss.
- Confirmations remove included mempool memberships. Disconnected transactions
  reappear only after actual admissions or reconciliation. ZMQ removals without
  reasons remain `unknown-removal`; replacements are never inferred from timing.
- Fee bands use base satoshis divided by `vsize_bip141`, or `ceil(weight/4)` when
  available. Ambiguous legacy `vsize` alone leaves the band unknown. Metadata
  collection is bounded and cannot resurrect removed membership.
- `--max-visible 5000` caps each Gource scene, leaving model state intact. The
  renderer prints visible/total and suppressed counts. It is a capped subset,
  not a representative sample; hidden leaves are not automatically backfilled.
  Restart the renderer for a fresh selection. Dense full-chainstate rendering is
  not an aim of stock Gource.
- `--journal-keep 100000` limits retained normalized event rows after successful
  reconciliations. Actions and disk bytes can exceed that count, especially while
  catching up. Lagging renderers repopulate from current state after pruning.
  SQLite reuses freed pages; no automatic database vacuum is performed.
- Gource's stock format approximates path moves with D/A. It cannot show graph
  edges, selection details, coordinated confirmation or distinct undo animations.
  These remain available as data/causes in the journal for a future renderer.
  Optional complete-current-set snapshot import and Gource patches are not included.

## Tests and a node-free preview

```sh
.venv/bin/python -m unittest discover -s tests -v
.venv/bin/bitcoin-wizard-cave demo --db .runtime/demo.sqlite --mode state |
  gource --log-format custom --file-idle-time 0 --stop-at-end -
```

`demo` requires a new database and labels it `SYNTHETIC-DEMO`; it never contacts
Core. Reuse that database with `render --once --mode protocol` or `--mode utxo`.
See [verification evidence and limitations](docs/VERIFICATION.md).

Design sources: the workspace's
`Gource_Bitcoin_Node_Visualization_and_Live_Integration.md` and
[`dyne/dowse`, `src/dowse-to-gource.c`](https://github.com/dyne/dowse/blob/6057a415933098ee3cd1cce4746790b9ca0fff63/src/dowse-to-gource.c).
The checked-out Dowse `main` branch did not contain the adapter; it was studied
from local `origin/master` at that revision. This is a new implementation, not a
copy of its C code. Licensed GPL-3.0-or-later; see [LICENSE](LICENSE).
