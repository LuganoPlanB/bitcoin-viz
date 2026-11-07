# Bitcoin activity as Logstalgia flows

[Logstalgia](https://logstalgia.io/) turns access records into moving balls aimed
at grouped paths. Successful records bounce off a paddle; unsuccessful ones pass
through. Its [custom format](https://github.com/acaudwell/Logstalgia/wiki/Custom-Log-Format)
lets us give those records Bitcoin meanings. No web server, Core patch or Logstalgia
patch is required. Bitcoin Viz writes the stream to stdout, just as for Gource.

Gource suits persistent objects and their changing state. Logstalgia suits the
pace, mix and bursts of work moving through a node. The renderer reads the
normalized event journal, rather than turning current inventory into fake traffic.

## Start with peer traffic

Install Logstalgia separately, then run against a signet Core:

```sh
bitcoin-viz live --format logstalgia --datadir "$HOME/.bitcoin" |
  logstalgia --sync --paddle-mode pid --title 'Bitcoin Viz - peer traffic' \
    -g 'Incoming,URI=^/traffic/[^/]+/incoming/,45' \
    -g 'Outgoing,URI=^/traffic/[^/]+/outgoing/,45' \
    -g 'Observer,URI=^/observer/,10' -
```

`--format gource` remains the default. Logstalgia defaults to `--mode traffic`.
The two paddles represent incoming and outgoing activity at the local node;
observer health uses a third lane. Labels distinguish commands such as `tx`,
`cmpctblock`, `headers`, `ping`, `inv` and `getdata`.

The default collector polls Core's per-peer, per-command byte counters. One ball
is **one nonzero byte delta per poll**, not one message or one transaction. Initial
counters establish a baseline and do not replay old traffic. A busy command may
produce one large ball covering many messages. Changes in traffic mix, peer
activity and inbound/outbound balance are useful observations; remote bandwidth,
validation latency and globally unique transaction counts cannot be read from them.

Enable Core message capture and pass `--capture-dir` to get **one event per complete
captured message**, with its payload byte length. Aggregate traffic animation is
then suppressed by the collector. Captured sources use endpoint/file identities,
which are not asserted to be Core peer IDs. Existing capture backlogs are read
once; tooltips retain source time so they are distinguishable from newly arriving
messages. Paths include `aggregate-bytes` or `message-capture` to keep the sources
distinct. `inv`/`getdata` names do not establish what objects they refer to.

Both incoming and outgoing records travel in Logstalgia's standard direction
across the screen. Direction is encoded by the lane, path and RX/TX label; the
screen is not a network topology or a request/reply acknowledgement diagram.

## Five views

| `--mode` | Destinations and visible activity | Useful questions |
|---|---|---|
| `traffic` | Source fidelity / direction / P2P command; real measured bytes | Which peers and commands are active? Are block bursts dominating? |
| `mempool` | Admissions by fee band, later metadata classification, removals by known cause | Are admissions accelerating? Are confirmations clearing tracked memberships? |
| `blocks` | Connected and disconnected heights; block bytes when recorded | Is the local chain progressing, catching up, or reorganizing? |
| `utxo` | Create, spend, undo and coverage-expiry paths, grouped by output script form | What output forms are turning over within our observed window? |
| `all` | All those streams plus observer health | What is the node doing across subsystems? |

Mempool snapshots populate inventory without admission timestamps, so snapshot
population and snapshot-only removals are omitted. `METADATA` is a separate event
when fee information arrives; it is not another admission. Unknown removal causes
remain `unknown-removal` and use neutral successful markers, without implying
rejection or replacement. Confirmations represent previously tracked mempool
memberships, not every transaction in a block.

Blocks carry their RPC-reported serialized byte size and transaction count when
available. Old journals without that metadata still work with unknown size zero.
Block times in the visualization are local observation times, not miner header
times. Initial catch-up can produce a burst of previously mined blocks.

UTXO paths recognize P2PKH, P2SH, P2WPKH, P2WSH and P2TR script **forms**, plus
`other` and `unknown`. They do not infer owners, wallet types or spend policies.
The observed-output window remains partial; `EXPIRE` is retention, not spending.
Undo events are red pass-through markers, while ordinary create/spend/expiry
records bounce. No satoshis are used as byte sizes.

Health markers are included in every view. `GAP` and reorg/undo records use
`success=0` to give a visual warning. That flag is a presentation convention,
not an assertion of invalid Bitcoin consensus behavior. A normal incoming P2P
message's bounce means it was observed, not that Core accepted or relayed it.

## One collector, independent displays

```sh
bitcoin-viz observe --datadir "$HOME/.bitcoin" --db .runtime/bitcoin-viz.sqlite
# Another terminal:
bitcoin-viz render --format logstalgia --mode all --db .runtime/bitcoin-viz.sqlite |
  logstalgia --sync --paddle-mode pid \
    -g 'Traffic,URI=^/traffic/,35' \
    -g 'Mempool,URI=^/mempool/,20' \
    -g 'Blocks,URI=^/chain/,15' \
    -g 'Outputs,URI=^/utxo/,20' \
    -g 'Observer,URI=^/observer/,10' -
```

Renderers use separate SQLite connections and release read transactions before
writing to the pipe. Gource can read the same database at the same time. Live
Logstalgia rendering starts after the current journal head; it does not replay
the current scene. `live` fixes its start cursor before collection starts, so
thread scheduling cannot lose the first events.

## History, resuming and synthetic demos

```sh
# Export the journal retained when the export begins:
bitcoin-viz render --format logstalgia --mode all --once \
  --db .runtime/bitcoin-viz.sqlite >node.log
logstalgia --paddle-mode pid node.log

# Resume after a known journal event ID, then follow:
bitcoin-viz render --format logstalgia --after 12000 --mode traffic \
  --db .runtime/bitcoin-viz.sqlite | logstalgia --paddle-mode pid -

# No Core required; use a fresh database:
bitcoin-viz demo --format logstalgia --mode all --db .runtime/new-log-demo.sqlite |
  logstalgia --paddle-mode pid -
```

Do not use `--sync` for historical exports, resumes that include old events, or
captured demo logs: it discards records older than the current time. Exports use
a fixed upper event boundary so concurrent collection cannot make `--once` run
forever. They include only retained events, not complete node history. If a cursor
falls behind pruning, the renderer emits a `journal-retention-gap` marker and
warns on stderr rather than reconstructing missing activity. Tooltips carry the
journal ID for resuming. Ignored event kinds also advance the cursor.

The demo is explicitly synthetic, finite, and clustered at its generation time.
It demonstrates format and lanes, not realistic traffic rates or peer behavior.
The [sample log](previews/logstalgia-demo.log) can be replayed directly. See the
[recorded preview](previews/logstalgia-demo.mp4) or [frame](previews/logstalgia-demo.png).

`--max-visible` applies to Gource scenes and is rejected for Logstalgia, whose
balls represent events rather than a capped inventory.

## Custom field mapping

All eleven documented fields are emitted:

```text
timestamp|hostname|path|code|bytes|success|color|referrer|user_agent|vhost|pid
```

| Field | Bitcoin meaning |
|---|---|
| timestamp | Journal observation time in Unix seconds; nondecreasing in journal order |
| hostname | Sanitized source actor: peer session, capture endpoint, transaction or local subsystem |
| path | Bounded semantic destination, without per-event hashes proliferating traffic paths |
| code | RX, TX, ADMIT, METADATA, CONFIRM, REMOVE, BLOCK, REORG, CREATE, SPEND, UNDO, EXPIRE, GAP, RESYNC, REBUILD |
| bytes | Measured message/counter/block bytes, or zero when unavailable |
| success | Explicit display convention: warnings pass through, ordinary observations bounce |
| color | Cyan incoming, orange outgoing, gold mempool, blue blocks, green outputs, red warnings |
| referrer | Empty; no causal request/response link is invented |
| user_agent | Journal ID, kind, cause, available object ID, fidelity and captured source time |
| vhost | `bitcoin-viz`, identifying the local observer |
| pid | Local activity lane: incoming, outgoing, mempool, chain, utxo, observer |

Tooltips percent-escape pipes, percent signs and control bytes. Identities and
path segments use the model's safe component encoding. Wall-clock rollbacks are
clamped for ordered playback; original nanosecond times remain in the database.
Logstalgia's on-screen request total therefore counts **visualized events**, not
necessarily messages, unique transactions, successful validations or total bytes.

## Further useful experiments

- A relay-conversation view could correlate captured `inv`, `getdata` and `tx`
  observations within one endpoint, exposing local request-to-arrival timing.
  It would require explicit correlation and timeout events; the existing stream
  does not measure network-wide propagation or infer it from adjacency.
- A block-transfer view could separate announcements, compact-block traffic and
  recovery messages, with accurate labels for actual payload observations.
- A validation-work view could use optional Core tracepoints to expose measured
  validation results and durations. ZMQ membership alone cannot provide them.
- A mempool-pressure view could record periodic size/fee-band samples separately
  from event flows, pairing Logstalgia's activity with a conventional occupancy
  chart. Event counts alone cannot establish mempool occupancy.

These are design directions, not implemented data sources. The current views
are deliberately grounded in the observer's existing journal.
