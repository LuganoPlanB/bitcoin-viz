# Native C++ implementation

`bitcoin-wizard-cave-native` is an independent C++17 implementation. It connects
to Bitcoin Core directly through libcurl RPC and libzmq IPC, maintains its own
SQLite state and journal, parses message capture files, and writes Gource records
to stdout. It does not launch the Python implementation or `bitcoin-cli`.

Python remains supported in the same repository. Both implementations use
database schema 1 and the same object paths, causes, fee definitions, undo
patches and presentation actions. Event timestamps/session IDs naturally differ
between runs. Decimal feerates may use different equivalent string formatting;
money is stored as integer satoshis in both implementations.

## Build

Dependencies: a C++17 compiler, CMake >=3.20, Ninja for the supplied presets,
pkg-config, SQLite >=3.24, libzmq >=4.3, libcurl >=7.68 and OpenSSL Crypto.
The JSON header is vendored at nlohmann/json 3.12.0 with its original MIT license
and recorded SHA256; configure/build perform no downloads.

On Debian/Ubuntu:

```sh
sudo apt install build-essential cmake ninja-build pkg-config \
  libsqlite3-dev libzmq3-dev libcurl4-openssl-dev libssl-dev
cmake --preset release
cmake --build --preset release --parallel 2
ctest --preset release
```

For another generator:

```sh
cmake -S . -B build/local -DCMAKE_BUILD_TYPE=Release
cmake --build build/local --parallel 2
ctest --test-dir build/local --output-on-failure
```

CMake enables `ccache` when available, unless a compiler launcher was explicitly
configured (including an explicitly empty launcher). The native program itself
has no Python dependency. `-DBUILD_TESTING=OFF` omits test targets and test-tool
discovery; with testing enabled, Python checks are registered when an interpreter
is available. Native contracts always run without Python.

Install the executable and licenses to a chosen prefix:

```sh
cmake --install build/release --prefix "$HOME/.local"
```

The release workflow targets Linux x86_64/arm64, macOS x86_64/arm64 and Windows
x86_64. POSIX and Win32 implementations provide file locking, capture reads and
interruptible output. Windows uses loopback TCP ZMQ publishers; filesystem IPC
remains the default for colocated Linux/macOS nodes. See [release builds](RELEASING.md).
No C ABI or installed SDK is promised; `wizard_native` is an internal static
library used by the CLI and native tests.

## Run

Configure Core with `-signet`, `server=1` and a sequence publisher, as documented
in the main README. The observer normally discovers the publisher through
`getzmqnotifications`. `unix:/path` becomes `ipc:///path` for libzmq; this is a
binary multipart socket. The Gource connection is the ordinary text pipe.

```sh
build/release/bitcoin-wizard-cave-native live \
  --datadir "$PWD/.runtime/signet" --mode state |
  gource --log-format custom --realtime --disable-auto-skip --file-idle-time 0 -
```

For independent collector/renderer lifetimes:

```sh
build/release/bitcoin-wizard-cave-native observe --datadir "$PWD/.runtime/signet"
# Another terminal; this can also read a Python-written database:
build/release/bitcoin-wizard-cave-native render --mode protocol |
  gource --log-format custom --realtime --disable-auto-skip --file-idle-time 0 -
```

The Python renderer can likewise read a C++-written database. Only one collector
should write a database at once; both implementations enforce the same `.writer.lock`.
Each collector startup creates a new observer session and reconciles membership.

`--mode state|protocol|utxo`, `--max-visible`, `--db`, `--datadir`, `--cookie`,
`--rpc-url`, `--zmq`, `--network`, `--window-blocks`, `--undo-horizon`,
`--bootstrap-blocks`, `--metadata-budget`, `--journal-keep`, `--peer-interval`,
`--reconcile-interval`, `--duration` and `--capture-dir` retain the Python CLI's
defaults and meanings. `render --once` exports the current scene;
`events --after ID --limit N` emits journal JSONL; `status` emits JSON.

```sh
build/release/bitcoin-wizard-cave-native status
build/release/bitcoin-wizard-cave-native events --after 0 --limit 20
build/release/bitcoin-wizard-cave-native demo --db .runtime/new-native-demo.sqlite --mode utxo
```

The synthetic demo requires a new database and is marked `SYNTHETIC-DEMO`.
It reproduces the Python demo's state and projections.

## Architecture and resource handling

| Native source | Responsibility |
|---|---|
| `storage.cpp` | SQLite statements, RAII transactions, journal/leaves, shared writer lock |
| `model.cpp` | All three projections; exact satoshi conversion, mempool, UTXO apply/undo, peer deltas |
| `core.cpp` | Cookie-authenticated persistent HTTP, precise JSON parsing, binary sequence decoding, bounded receiver thread |
| `observer.cpp` | Bootstrap, snapshots, chain catch-up, source/session recovery, bounded metadata work |
| `capture.cpp` | Core capture framing, selective inventory links, persistent offsets and source gaps |
| `render.cpp` | Scene bootstrap and committed-action reading with presentation caps |
| `main.cpp` | CLI, signals, interruptible output and thread lifetime, synthetic demo/replay |
| `platform.cpp` | POSIX/Win32 locking, positioned file reads, stdout cancellation and Core data-directory defaults |

SQLite state, inverse patches and presentation actions commit together. Failed
operations roll back. The receiver queue is capped at 8,192 records; overflow and
malformed frames request reconciliation. Outer counters wrap modulo 2^32; inner
mempool jumps alone do not imply loss. A replaced cookie creates a new session
and receiver, and stale queued events from the old session are discarded.

RPC accepts only local HTTP endpoints and disables proxies/redirects. Cookies
are reread on each call. Connect/request deadlines are 3/10 seconds, and response
bodies are capped at 64 MiB. The JSON SAX reader retains decimal lexemes before
conversion to satoshis and preserves uint64 counters. Base fees and band comparisons
use integer arithmetic; display rates use decimal long division with 28 significant
digits and half-even rounding.

The renderer uses its own SQLite connection and releases read transactions before
writing. It checks journal retention and reads each action batch within one
snapshot. Native stdout is nonblocking and interruptible, so a saturated pipe
does not prevent collector progress or thread shutdown. Worker threads are joined
before their captured resources are destroyed.

Capture reads are bounded and rotate fairly across files. Full block/transaction
payloads are skipped rather than copied when only metadata is needed. Inventory
links are capped at 1,000 per message and truncation is reported. Endpoint-based
capture actors retain that identity; they are not claimed to be Core peer IDs.
Incomplete/corrupt capture records do not become guessed messages.

Coverage remains an **observed-output window**, with the same defaults and
limitations as Python. Full-current-set snapshot import, USDT/eBPF ingestion,
graph overlays and custom Gource animations remain optional future extensions.
No throughput or mainnet-scale performance claim is made.

## Replay and parity

The extra `replay --input FILE.jsonl --db NEW.sqlite` command applies controlled
operations to a new synthetic database marked `SYNTHETIC-REPLAY`. For example:

```json
{"op":"checkpoint","hash":"0000000000000000000000000000000000000000000000000000000000000000","height":0}
{"op":"admission","txid":"000000000000000000000000000000000000000000000000000000000000000a"}
{"op":"removal","txid":"000000000000000000000000000000000000000000000000000000000000000a","cause":"controlled-removal"}
```

Other operations are `fee`, `enrich`, `pending`, `snapshot`, `connect`, `disconnect`,
`peers`, `message`, `loss` and `prune`. Replay is a model test/input tool, not a
consumer for the emitted journal format. It does not claim to replay missing
Bitcoin network history. `native/tests/parity.py` defines complete fixtures and
applies each to the Python model and native executable.

## Verification

Release: GCC 14.2.0. Sanitizer build: Clang 19.1.7. Both used Linux x86_64,
SQLite 3.46.1, libzmq 4.3.5, libcurl 8.14.1 and OpenSSL 3.5.7.
`ccache` was present in the actual compile commands. The final builds have no
compiler warnings under the configured warning flags.

```sh
cmake --preset sanitize -DCMAKE_CXX_COMPILER=clang++
cmake --build --preset sanitize --parallel 2
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1 ctest --preset sanitize
```

All five CTest suites passed in both build variants:

- `native-contract`: 36 passing cases covering portable UTF-8 paths, model invariants, atomic rollback, reversed undo, pending
  overlays, visibility/pruning, peer deltas/sessions, exact wire/decimal handling,
  capture continuation/fairness, writer exclusion and a real libzmq IPC receiver.
- `python-contract`: the existing 34 Python tests, with no skips.
- `cross-language-parity`: 20 controlled scenarios comparing model state, undo
  records, normalized events and per-mode actions, 120 cross-reader scene checks,
  equivalent demos and invalid-input rejection.
- `native-observer-integration`: a controlled HTTP server and actual libzmq IPC
  publisher exercise admission/removal, missing outer counters, legitimate inner
  sequence jumps, exact snapshot membership, cookie/session rotation, capture
  continuation, proxy bypass and shutdown while stdout is deliberately unread.
- `release-tools`: eight cases covering source versioning, complete platform
  archives, manifests, licenses and checksums.

No address, leak or undefined-behavior findings were reported for those suites.
Python is used as a test driver/oracle, not by the native executable.

This sandbox lacked libcurl development headers. For local verification only,
the matching Debian development package was extracted under `.tools/native-deps`
and the system runtime was selected explicitly:

```sh
cmake --preset release \
  -DCURL_INCLUDE_DIR="$PWD/.tools/native-deps/sysroot/usr/include/x86_64-linux-gnu" \
  -DCURL_LIBRARY=/usr/lib/x86_64-linux-gnu/libcurl.so.4
```

`CCACHE_DIR` was set to `build/.ccache` while compiling in the sandbox. These
local dependencies, binaries, caches and runtime data are excluded from Git.

### Real signet

The standalone native observer was also tested against the same isolated Core
32.1 signet node used for Python. With networking frozen, it tracked two blocks
ending at height 231914, then followed a real ZMQ disconnect/readmission/reconnect.
It reconciled **535 readmitted memberships**, removed those memberships on
reconfirmation, and restored **6,260 surviving output records** exactly. Twenty
`gettxout(..., false)` existence/value checks passed. The coin digest matches the
earlier Python result:

```text
402115f649d73d1e86df75d244bd56142bf7acba2776fbbe21f6036bd6b9affb
```

See [verification-native-signet.json](verification-native-signet.json). To repeat
on a disposable project node with a new verification database:

```sh
python3 tools/verify_native_signet.py \
  --binary build/release/bitcoin-wizard-cave-native \
  --datadir "$PWD/.runtime/signet" --db .runtime/new-native-verify.sqlite \
  --exercise-reorg
```

`--exercise-reorg` temporarily invalidates/reconsiders the local tip and restores
the prior network-active setting in cleanup. Use it only on a disposable node.
Initial signet sync was still incomplete: this is evidence for the node's partial
chain, not a claim of full signet synchronization or complete UTXO coverage.

A separate Python → C++ → Python collector handoff resumed the same copied
signet database in each implementation. All 6,260 output records retained the
same digest. The installed executable also ran successfully with an empty
environment (`env -i ... --version`); installation staged the executable and
both the GPL and vendored MIT licenses.

### Gource stdin

The preview tool can select the native renderer:

```sh
python3 tools/preview.py --native-binary build/release/bitcoin-wizard-cave-native \
  --db .runtime/wizard.sqlite --output docs/native-previews
```

This reads the original Python-written signet database with the C++ renderer and
feeds each mode to Gource through stdin. Its protocol targets come from the prior
real Core capture; they are a current scene snapshot, not newly arriving traffic.
The UTXO preview selects 200 of 6,260 outputs. The state scene has two blocks, an
empty mempool and no peers because the test node's networking was paused.

| Native-rendered mode | Image | Video |
|---|---|---|
| Working state | [state.png](native-previews/state.png) | [state.mp4](native-previews/state.mp4) |
| Protocol | [protocol.png](native-previews/protocol.png) | [protocol.mp4](native-previews/protocol.mp4) |
| UTXO window | [utxo.png](native-previews/utxo.png) | [utxo.mp4](native-previews/utxo.mp4) |

Gource and ffmpeg completed successfully for all three native-rendered scenes.
