# Bitcoin Viz

A C++17 observer for Bitcoin Core with three live Gource projections. It connects
to Core's **ZMQ sequence publisher**, uses **local cookie-authenticated RPC**, and
feeds **Gource through stdin**. Signet is the default. Core and Gource need no patches.

The application, model and tests are native C++; build and release automation use
CMake. Sources live in `src/`, tests in `tests/`, and the pinned JSON dependency in
`third_party/`. Licensed GPL-3.0-or-later; see [LICENSE](LICENSE).

[Source and releases](https://github.com/luganoplanb/bitcoin-viz).
The [release workflow](docs/RELEASING.md) builds Linux x86_64/arm64, macOS
Intel/Apple Silicon, and Windows x86_64 archives with licenses, source manifests
and SHA256 checksums. Linux release binaries use static Dyne musl builds.

| Mode | The scene shows | Sources |
|---|---|---|
| `state` | Peer sessions, fee-banded local mempool, recent active and disconnected blocks | Sequence + RPC |
| `protocol` | Incoming/outgoing transaction, block, discovery, control and unclassified traffic | Per-peer RPC byte deltas; optional Core message capture |
| `utxo` | Confirmed output creation/spending, reorg undo, separate pending-spend leaves | Decoded connected blocks + mempool metadata |

Recorded signet scenes: [state](docs/previews/state.mp4),
[protocol](docs/previews/protocol.mp4), [UTXO window](docs/previews/utxo.mp4).
See [verification and coverage](docs/VERIFICATION.md) for their provenance.

## Build and install

Requires CMake 3.20+, a C++17 compiler, libzmq 4.3+, SQLite 3.24+, libcurl 7.68+
and OpenSSL Crypto development files. Presets use Ninja. The nlohmann JSON
3.12.0 header and its MIT license are vendored; CMake downloads no dependencies.

On Debian/Ubuntu:

```sh
sudo apt install build-essential cmake ninja-build pkg-config ccache \
  libzmq3-dev libsqlite3-dev libcurl4-openssl-dev libssl-dev
git clone https://github.com/luganoplanb/bitcoin-viz.git
cd bitcoin-viz
cmake --preset release
cmake --build --preset release --parallel 2
ctest --preset release
cmake --install build/release --prefix "$HOME/.local"
```

The executable is `build/release/bitcoin-viz`, or `bitcoin-viz` after installation
if the chosen `bin` directory is on your PATH. CMake uses ccache when available
unless a compiler launcher was explicitly configured.

## Connect to Core

Configure a local signet node using [examples/bitcoin.conf](examples/bitcoin.conf):

```ini
server=1
signet=1
[signet]
zmqpubsequence=unix:/run/user/1000/bitcoin-viz/sequence.sock
```

Replace `1000` with your UID and create the socket directory first, owned by the
Core user with mode 0700. Start your installed `bitcoind -signet` using the chosen
configuration and data directory. Initial synchronization downloads and validates
signet history. No wallet or broadcast RPC is needed.

```sh
bitcoin-viz live --datadir "$HOME/.bitcoin" --mode state |
  gource --log-format custom --realtime --disable-auto-skip --file-idle-time 0 \
    --title 'Bitcoin Viz · Signet' -
```

Use `--mode protocol` or `--mode utxo` for the other views. For another data
directory, supply `--datadir`; explicit `--cookie`, `--rpc-url` and `--zmq` are
also available. The observer checks Core's chain. `--network regtest` supports
controlled testing; mainnet is outside the current CLI's scope.

Core translates `unix:` into libzmq `ipc://`. The CLI accepts either spelling
and normally discovers the sequence publisher with `getzmqnotifications`.
Windows uses a loopback publisher such as `tcp://127.0.0.1:28332`. This is binary
multipart ZMQ, separate from Core's multiprocess IPC. The connection from the
observer to Gource is an ordinary text pipe. Diagnostics go to stderr.

## Keep collecting while changing views

SQLite commits model state, a normalized journal, inverse patches and presentation
actions together. A renderer uses a separate connection, so a slow Gource cannot
block collection. For independent collector and renderer lifetimes:

```sh
bitcoin-viz observe --datadir "$HOME/.bitcoin" --db .runtime/bitcoin-viz.sqlite
# In another terminal:
bitcoin-viz render --db .runtime/bitcoin-viz.sqlite --mode utxo |
  gource --log-format custom --realtime --disable-auto-skip --file-idle-time 0 -
```

Each renderer repopulates the current scene as `node:snapshot`, then follows
committed actions. `render --once` exports the current scene. `live` combines
collection with an independent renderer thread. Only one collector may write a
database at once; the `.writer.lock` enforces this.

```sh
bitcoin-viz status --db .runtime/bitcoin-viz.sqlite
bitcoin-viz events --db .runtime/bitcoin-viz.sqlite --after 0 --limit 100
bitcoin-viz --help
```

Status includes the applied tip, checkpoint, output-window coverage, state and
visible-leaf counts, Core version and reconciliation time. Journal records retain
nanosecond observation times, source counters, original causes, dependencies and
integer satoshis. Schema version remains 1. `events --after ID` supports incremental
inspection without promising a total causal order among independent sources.
Existing schema-1 databases remain readable with an explicit `--db` path; the
new default is `.runtime/bitcoin-viz.sqlite`.

## Protocol detail

The default protocol view polls `bytesrecv_per_msg` and `bytessent_per_msg` every
two seconds. These are **byte deltas, not message counts**. Initial cumulative
counters establish a baseline. Short peer sessions may be missed; `inv` and
`getdata` names alone do not identify transaction or block inventory.

For actual message observations, enable Core's optional `capturemessages=1`
(which records full P2P payloads on disk), then:

```sh
bitcoin-viz observe --datadir "$HOME/.bitcoin" \
  --capture-dir "$HOME/.bitcoin/signet/message_capture"
```

The reader consumes bounded complete records, journals metadata and selected
inventory links, and commits file cursors atomically. Aggregate animation is
suppressed to avoid double counting. Inventory parsing distinguishes txids,
wtxids and block hashes; only known txids touch existing mempool leaves.
Unknown content never creates membership. Inventory detail is capped at 1,000
links per message, with sampling recorded in the journal.

Capture endpoints are labeled `capture-...`, with observer-run and file identity;
they are not falsely joined to Core peer IDs. Backlogs retain their source time
and are not claimed to be live traffic. Partial/corrupt records, truncation and
rotation produce source gaps. The observer does not rotate or delete capture files.

## Coverage and recovery

- The UTXO view shows **observed outputs**, not the full current UTXO set. Startup
  anchors six blocks behind Core's tip. `--window-blocks 144` expires old creation
  cohorts as coverage expiry, never as a spend; `0` retains surviving observed
  outputs from a fixed checkpoint and may grow.
- Reverse-order per-block patches support reorg undo, including outputs created
  then spent in one block. `--undo-horizon 288` bounds undo history. Crossing the
  checkpoint/horizon, missing pruned content or excessive catch-up rebuilds the
  observed window and reports a new coverage anchor.
- During initial download the applied chain is Core's partially synchronized
  chain. Duration limits are checked between bounded RPC/catch-up batches.
- Buffering before snapshots, chain-tip bracketing, the snapshot's next mempool
  sequence boundary, wrapping outer counters, exact membership comparisons and
  cookie refresh support recovery. Inner counter jumps alone do not imply loss.
- Confirmations remove included mempool memberships. Disconnected transactions
  return only after admissions or reconciliation. Removal reasons remain unknown
  when the source cannot supply them; timing does not establish replacement.
- Fee bands use base satoshis divided by `vsize_bip141`, or `ceil(weight/4)`.
  Ambiguous legacy `vsize` leaves the band unknown. Bounded metadata work cannot
  resurrect removed memberships.
- `--max-visible 5000` caps each scene while preserving model state. It is a
  capped subset rather than a representative sample. Restart to refresh selection.
- `--journal-keep 100000` bounds retained event rows after reconciliation; actions
  and disk bytes can exceed that count. Lagging renderers repopulate after pruning.
  SQLite reuses pages without automatically vacuuming.
- Stock Gource approximates moves with D/A and cannot show graph edges, selection
  detail or distinct undo animations. Full-current-set import and custom Gource
  patches are not included. No mainnet-scale performance claim is made.

## Node-free demo and checks

```sh
bitcoin-viz demo --db .runtime/new-demo.sqlite --mode state |
  gource --log-format custom --file-idle-time 0 --stop-at-end -
bitcoin-viz render --db .runtime/new-demo.sqlite --once --mode protocol
```

`demo` requires a new database, labels it `SYNTHETIC-DEMO` and never contacts Core.
`replay` applies controlled JSONL model operations to a new database.
See [architecture](docs/ARCHITECTURE.md) and [verification](docs/VERIFICATION.md),
including the sanitizer preset.

The ambient-visualization approach was inspired by
[`dyne/dowse`, `src/dowse-to-gource.c`](https://github.com/dyne/dowse/blob/6057a415933098ee3cd1cce4746790b9ca0fff63/src/dowse-to-gource.c).
This is an independent implementation.
