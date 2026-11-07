# Architecture

Bitcoin Viz is a C++17 executable. `bitcoin_viz_core` is an internal static
library shared by the CLI and contracts, with no installed SDK or C ABI.
The program talks directly to Core using libcurl RPC and libzmq; it starts
neither a language interpreter nor `bitcoin-cli`.

| Source under `src/` | Responsibility |
|---|---|
| `storage.cpp` | SQLite statements, RAII transactions, journal, leaves and writer lock |
| `model.cpp` | Three projections, exact satoshis, mempool, UTXO apply/undo, peer deltas |
| `core.cpp` | Cookie-authenticated HTTP, exact JSON numbers, sequence decoding and bounded receiver |
| `observer.cpp` | Bootstrap, snapshots, catch-up, recovery and bounded metadata work |
| `capture.cpp` | Capture framing, selected inventory links, offsets and source gaps |
| `render.cpp` | Current-scene bootstrap and committed-action reading with presentation caps |
| `logstalgia.cpp` | Event-journal flows, source fidelity, byte sizes, cursor following and retention gaps |
| `main.cpp` | CLI, signals, output/thread lifetime, synthetic demo and replay |
| `platform.cpp` | POSIX/Win32 locks, positioned reads, interruptible output and Core defaults |

SQLite state, inverse patches and actions commit atomically; failed transactions
roll back. The receiver queue holds at most 8,192 records. Overflow or malformed
frames request reconciliation. Outer counters wrap modulo 2^32; inner mempool
jumps alone do not establish loss. Cookie replacement starts a new session and
receiver, discarding old queued events.

RPC permits local HTTP endpoints, disables proxies and redirects, rereads cookies,
uses 3/10-second connect/request deadlines and caps bodies at 64 MiB. The JSON SAX
reader retains decimal lexemes and uint64 counters. Satoshis and band comparisons
use integers; display rates use 28 significant digits with half-even rounding.

The renderer holds its own SQLite connection and releases read transactions before
writing. Each action batch uses one snapshot. Retention gaps trigger scene rebuilds.
Stdout is interruptible, so a saturated pipe can shut down while collection
continues. Threads join before their captured resources are destroyed.

Capture reads are bounded and fair across files. Large payloads are skipped when
only metadata is needed. Inventory links cap at 1,000 with explicit sampling.
Endpoint identities remain distinct from polled peer IDs. Incomplete or corrupt
records produce gaps rather than guessed messages.

Logstalgia reads normalized events rather than Gource's actions. Its default
cursor starts at the journal head; explicit cursors resume and finite exports
fix an upper boundary. Each batch reads pruning metadata and events in one SQLite
snapshot and releases it before output. Pruning produces a visible gap rather
than a state snapshot presented as recent traffic. See [Logstalgia](LOGSTALGIA.md)
for field mappings, event semantics and suggested future views.

## Controlled replay

`bitcoin-viz replay --input FILE.jsonl --db NEW.sqlite` applies controlled operations
to a new database marked `SYNTHETIC-REPLAY`:

```json
{"op":"checkpoint","hash":"0000000000000000000000000000000000000000000000000000000000000000","height":0}
{"op":"admission","txid":"000000000000000000000000000000000000000000000000000000000000000a"}
{"op":"removal","txid":"000000000000000000000000000000000000000000000000000000000000000a","cause":"controlled-removal"}
```

Other operations: `fee`, `enrich`, `pending`, `snapshot`, `connect`, `disconnect`,
`peers`, `message`, `loss`, `prune`. This is controlled model input, not the emitted
journal format or a way to recover missing network history. The SQLite schema
remains version 1; existing databases work through an explicit `--db` path.
