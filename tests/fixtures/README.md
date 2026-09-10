# Test fixtures

## `cdm_attack_chain.json`

17 hand-built DARPA-CDM records (line-delimited JSON, CDM18 encoding) describing a
minimal but complete intrusion, used to validate the whole pipeline end to end:

```
netflow 192.168.99.66:4444   (attacker)
  --RECVFROM-->  /usr/sbin/sshd
  --FORK------>  /bin/bash
  --WRITE----->  /tmp/payload
  --EXECUTE--->  /usr/sbin/cron
  --SENDTO---->  netflow 203.0.113.7:443   (exfiltration)
```

Plus deliberate negatives:
- `/usr/bin/vim` writing `/home/u/notes.txt` -- unrelated, must never be tainted
- `EVENT_CLOSE` / `EVENT_OPEN` records -- carry no information flow, must be dropped
- `/usr/bin/vim` writing `/tmp/payload` at t=5, i.e. BEFORE the attack reaches that
  file -- forward taint must exclude it, backward trace must include it (the classic
  BackTracker false-dependency, and the reason version-granularity matters)

### Expected

```
./build/barry_tc_cdm_to_bin --in tests/fixtures/cdm_attack_chain.json --out /tmp/tc_sample
#  17 lines -> 7 edges over 8 nodes (proc=4 file=2 net=2 unk=0), dropped no-flow=2

./build/barry_provenance_query --graph /tmp/tc_sample --query forward \
    --seed "192.168.99.66" --at min
#  6 entities: attacker, sshd, bash, /tmp/payload, cron, exfil  (NOT vim, NOT notes.txt)

./build/barry_provenance_query --graph /tmp/tc_sample --query backward \
    --seed "203.0.113.7" --at max
#  7 entities: the chain above plus /usr/bin/vim (false dependency, expected)
```
