# Security / provenance datasets

Status as of 2026-08-21.

| Dataset | What it is | Size | Ground truth | Status |
|---|---|---|---|---|
| **DARPA TC E3 — CADETS** | FreeBSD whole-system provenance, 12 days | 42M events / 25 GB | yes (report PDF) | ⏳ blocked on Google Drive quota, auto-retrying |
| **DARPA TC E3 — THEIA** | Ubuntu whole-system provenance, 12 days | 106M events / 40 GB | yes | ⏳ same |
| **LANL cyber1** | 58 days of auth/proc/flow/DNS from a real enterprise | 1.65B events / 12 GB gz | **yes, `redteam.txt`** | 🔴 needs a request form (see below) |
| DARPA OpTC | Windows endpoint telemetry | ~1 TB | yes | not started |
| SNAP temporal graphs | social/citation proxies | 60K–646M edges | none | ✅ on disk, used for E7 pilot |

## DARPA TC E3 — the blocker

Data lives on a public Google Drive folder shared with the whole research community:
`https://drive.google.com/drive/folders/1QlbUFWAGq3Hpl8wVdzOdIoZLFxkII4EK`

Folder listing, docs, schema, and the ground-truth report all downloaded fine. The
large data files return *"Too many users have viewed or downloaded this file
recently"* — a folder-wide, rolling quota, not a per-file one, and not something on
our end. Already retrieved into `darpa_tc_e3/docs/`:

- `TC_Ground_Truth_Report_E3_Update.pdf` (the attack scenarios)
- `CDM18.avdl`, `TCCDMDatum.avsc` (schema)
- `operational_event_log.md` (day-by-day engagement log)

`raw/retry_dl.sh` retries every 20 minutes for 24 hours and stops as soon as the
CADETS files land. If the quota never opens, the reliable workaround needs a Google
account: open the folder in a browser, "Add shortcut to Drive", then download through
an authenticated client — the per-user quota is separate from the public one.

## LANL cyber1 — needs one action from Jiacheng

The single best-value security dataset for Barry: it is already event-tuple shaped
(`time, src_user, dst_user, src_computer, dst_computer, ...`), 1.65B events over 58
days, **CC0 licensed**, and ships red-team ground truth. It converts to Barry's Edge
format almost directly, with no CDM parsing at all.

Access needs a short form (email + intended use) at
`https://csr.lanl.gov/data/cyber1/`. I cannot submit that — it has to be you.

Files once granted: `auth.txt.gz` (7.2 GB), `proc.txt.gz` (2.2 GB),
`flows.txt.gz` (1.1 GB), `dns.txt.gz` (177 MB), `redteam.txt.gz` (4.8 KB).

## Pipeline (built and tested, waiting on data)

```
tar -xzOf ta1-cadets-e3-official.json.tar.gz \
  | ./build/barry_tc_cdm_to_bin --out data/bin/tc-cadets-e3 --progress

./build/barry_provenance_query --graph data/bin/tc-cadets-e3 \
    --query backward --seed "<alert entity>" --at <ns>
```

`barry_tc_cdm_to_bin` turns CDM events into information-flow edges; the direction
policy (`kFlow[]` in `tools/tc_cdm_to_bin.cpp`) is what makes taint meaningful, so it
is documented per event type and is the thing to argue about in review. Validated on
`tests/fixtures/cdm_attack_chain.json`.
