# E25: the dock ladder

The core engine (timeline + arena + lock-free append + window scans +
retention) runs detached; every further capability is a module that docks
onto it and pays a measured price on the write path. Same edges, same 8
threads, full-count check and an identical time-axis probe answer required
at every rung; the props rungs additionally verify sampled payloads byte
for byte. Three trials per rung, medians below; `e25.csv` holds every run.

Rungs: core (nothing docked) / +vindex (entity axis) / +reverse (backward
tracing) / +props8, +props16 (per-edge property sidecar) / +wal
(durability, write_only semantics = E18's write_only rung: bytes reach the
page cache, no fsync; the fsync spectrum is E18's subject) / full
(vindex + reverse + props16 + wal).

## Medians (Meps, slowdown vs core, RSS MB)

| rung | askubuntu | wiki-talk | cit-Patents | tc-cadets |
|---|---|---|---|---|
| core | 51.5 (1.0x, 32) | 199.1 (1.0x, 181) | 279.0 (1.0x, 373) | 292.7 (1.0x, 510) |
| +vindex | 32.7 (1.6x, 81) | 60.3 (3.3x, 455) | 24.9 (11.2x, 1173) | 60.9 (4.8x, 1169) |
| +reverse | 19.9 (2.6x, 131) | 20.0 (10.0x, 878) | 11.2 (24.9x, 2200) | 34.5 (8.5x, 2125) |
| +props8 | 36.2 (1.4x, 94) | 54.8 (3.6x, 534) | 23.9 (11.7x, 1331) | 61.5 (4.8x, 1387) |
| +props16 | 35.3 (1.5x, 106) | 54.9 (3.6x, 602) | 23.7 (11.8x, 1479) | 61.0 (4.8x, 1582) |
| +wal | 21.7 (2.4x, 81) | 27.4 (7.3x, 454) | 16.0 (17.4x, 1173) | 28.3 (10.3x, 1169) |
| full | 11.6 (4.4x, 156) | 14.0 (14.2x, 1026) | 9.0 (31.1x, 2506) | 20.5 (14.3x, 2538) |

## What the ladder says

1. **The detached core runs at 200-293 Meps** on the three large corpora.
   Every headline ingest number this project has ever reported was the
   +vindex rung: the vertex index, not the time-keyed core, is and always
   was the write path's dominant cost. A deployment that needs only
   time-axis operations (windows, aggregation, retention, replay) gets the
   core rate.
2. **Properties are nearly free.** +props8/+props16 sit at the +vindex
   rung within noise on every corpus (the sidecar memcpy hides behind the
   index insert), costing only the proportional RSS. The "no properties"
   objection is a dock module, not a design limit.
3. **The reverse index costs about one more vertex index**, which is what
   it is.
4. **Durability costs what the log write costs** (write_only semantics);
   the full fsync spectrum is E18's table.
5. Rungs compose roughly multiplicatively into `full`, and every rung
   answered the identical window-count probe: modules change cost, never
   answers.

Mechanism: the property sidecar is a parallel anonymous mapping addressed
by the same arithmetic that places the edge (slot = arena offset / 20),
written before the timestamp's release store, so clause T5 extends to
properties: a published edge always shows its payload
(tests/test_props.cpp holds this under 8 concurrent writers on both
insert paths).
