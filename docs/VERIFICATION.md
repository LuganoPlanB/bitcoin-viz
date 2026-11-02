# Verification — 2 November 2026

Implemented and exercised on Linux x86_64, Python 3.13, pyzmq 27.1.0,
SQLite, Bitcoin Core 32.1.0 (`version=320100`) and Gource 0.54.

## Contract suite

From the project directory:

```sh
.venv/bin/python -m unittest discover -s tests -q
```

**34 tests passed**, no failures or skips. The cases cover:

| Design acceptance case | Evidence |
|---|---|
| Confirmation removes exactly included mempool memberships | Contract test; real reconnect stream |
| Several removals retain distinct transaction identities | Controlled replacement replay; no automatic reason inference |
| Reorgs only restore observed/reconciled mempool membership | Contract test; real signet disconnect/readmission/reconnect |
| Create and spend within a block leaves no residue after undo | Contract test; inverse-operation order |
| Missing outer counter triggers reconciliation | Contract test; wraparound and inner-counter jumps also exercised |
| Node restart creates a new session and refreshes credentials | Cookie/session contract tests and real Core stop/restart |
| Delayed metadata never recreates membership | Contract test |
| A slow/restarted renderer does not stop collection | Controlled blocked-output test, released and joined; scene repopulation test |
| Display/coverage expiry is distinct from spending | Contract test; expiry itself is reversible |
| Missing capture data is explicit | Partial-record continuation, loss reporting, inventory validation and fair file polling tests |
| Fee-size definitions remain honest across versions | Modern field, weight-derived BIP141 size and unknown legacy-size tests |
| Pruned history / undo horizon requires rebuild | Controlled unavailable-block and horizon tests |

Additional tests cover atomic rollback of tip/coins/journal/actions, exact snapshot
sequence boundary, changing chain-tip snapshots, pending-spend overlays,
OP_RETURN/oversized-script exclusion, local transport restrictions, input
sanitization, renderer caps, journal pruning and competing-writer exclusion.

## Real signet integration

Official binary downloaded from
[Bitcoin Core 32.1](https://bitcoincore.org/bin/bitcoin-core-32.1/).
The Linux x86_64 archive SHA256 is:

```text
61545c17c3120fb7412f896d248b11993af04ec8817aad3649c55b2afb044963
```

The archive matched the official HTTPS `SHA256SUMS` and the project pin.
`tools/core.py fetch` was also rerun while Core was running: it validated the
installed daemon and CLI against the archive without overwriting executables.
**Release PGP signatures were not verified.** The helper makes that limitation explicit.

The isolated node used `-signet`, a project-local datadir, loopback RPC, pruning,
bounded caches and:

```text
-zmqpubsequence=unix:/home/gestalt-dyne/gestalt/dowse-workspace/bitcoin-wizard-cave/.runtime/socket/sequence.sock
```

The subscriber connected with `ipc:///.../sequence.sock`, discovered through RPC.
No wallet or transaction-broadcast RPC was needed.

The stable-tip integration command was:

```sh
.venv/bin/python tools/verify_signet.py \
  --datadir "$PWD/.runtime/signet" \
  --db .runtime/wizard.sqlite --exercise-reorg
```

`--exercise-reorg` is specifically for a disposable node: it temporarily disables
networking and invalidates/reconsiders its tip, restoring the tip and prior
network-active setting in cleanup. Do not use that option against an operational
node. The verification database must be new when repeating the command.

Observed at signet height **231914**, block:

```text
0000003a189d0a6ad06c8bec6e99a0638e7b3d92c5e1879dd1e996af21bd35ff
```

- Correct binary ZMQ `D` and `C` notifications for that hash over the Unix socket.
- **535 actual `A` readmission notifications** between the block notifications.
  These demonstrated why a receiver must tolerate interleaved mempool events.
- **6,260 tracked surviving outputs**, initialized from the preceding two blocks.
- An identical digest of sorted outpoint/full-record pairs before and after
  disconnect/reconnect: `402115f649d73d1e86df75d244bd56142bf7acba2776fbbe21f6036bd6b9affb`.
- **20 `gettxout` checks**, with `include_mempool=false`, matched tracked existence
  and satoshi values.
- **400 real capture records** read from Core's `message_capture` files, retaining
  source times and producing 26 protocol targets.
- All three current scenes emitted valid five-field Gource logs. The integration
  renderer cap selected 5,000 of the 6,260 UTXO leaves without truncating model state.

Machine-readable results are in [verification-signet.json](verification-signet.json).

A separate 20-second observer run survived a real Core shutdown and restart on
the same datadir/socket. Its journal recorded missing-cookie and startup-warmup
gaps, a new `observer.session`, and `observer.resync` with cause `node-restart` and
the original applied tip. The rotated cookie was read automatically. Temporary
network/capture tests were stopped cleanly after verification; the datadir and
installed binaries remain available for restarting.

Core **had not finished signet initial block download**. Networking was paused
for the controlled tip exercise. This validates observation and recovery against
a real partial signet chain; it does not establish full signet synchronization,
mainnet scale, sustained throughput or full-current-UTXO-set coverage. Initial
sync testing exposed and led to fixes for pruning/catch-up races. Catch-up batches
are bounded and explicit checkpoint rebuilds narrow coverage when necessary.

## Actual Gource stdin rendering

```sh
.venv/bin/python tools/preview.py \
  --db .runtime/integration-final.sqlite --output docs/previews
```

Gource read each custom log through **stdin** on a private Xvfb display. Gource
and ffmpeg exited successfully for all three modes, producing 10.64-second videos
and PNG frames. The UTXO preview deliberately selects 200 of 6,260 outputs. These
are real signet-derived scenes, not the synthetic demo. The state preview has
two recent blocks; networking was paused and the reconciled mempool was empty.
The protocol preview represents previously captured messages, not an assertion
that those messages were arriving at screenshot time.

| Mode | Image | Video |
|---|---|---|
| Working state | [state.png](previews/state.png) | [state.mp4](previews/state.mp4) |
| Protocol | [protocol.png](previews/protocol.png) | [protocol.mp4](previews/protocol.mp4) |
| UTXO window | [utxo.png](previews/utxo.png) | [utxo.mp4](previews/utxo.mp4) |

The standalone renderer repopulates with `node:snapshot` agents. It cannot
reconstruct historical avatar choreography from a current-state snapshot.

## Deliberate scope

The implementation supplies all three modes through stock Gource. Optional full
UTXO snapshot import, native USDT/eBPF collection, reason-rich replacement
correlation, graph overlays and Gource renderer modifications are not included.
Unknown removal causes and peer attribution remain unknown. Captured endpoint
identities are not falsely converted into Core peer IDs or transaction authors.
There is no claim of lossless historical observation.

Protocol references checked alongside the local design:
[Core ZMQ protocol](https://github.com/bitcoin/bitcoin/blob/v32.1/doc/zmq.md),
[sequence wire construction](https://github.com/bitcoin/bitcoin/blob/v32.1/src/zmq/zmqpublishnotifier.cpp),
[mempool counter definition](https://github.com/bitcoin/bitcoin/blob/v32.1/src/txmempool.h),
[Core capture parser](https://github.com/bitcoin/bitcoin/blob/v32.1/contrib/message-capture/message-capture-parser.py).
