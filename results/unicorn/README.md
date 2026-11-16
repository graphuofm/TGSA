# UNICORN on reduced provenance (pilot, interrupted)

Goal: run the published UNICORN detector (Han et al., NDSS 2020; code from the
crimson-unicorn GitHub organization, cloned into baselines/unicorn/, git-ignored)
on DARPA TC E3 CADETS before and after log reduction, to see whether the
detector's own output registers the evidence loss measured in E8/E14.

State when the session ended (2026-10-20): the pipeline builds and runs. Scripts:
scripts/unicorn_cdm_to_edgelist.py (CDM JSON to UNICORN edge lists),
scripts/unicorn_make_graphs.py (base/stream split per graph),
scripts/unicorn_reduce_stream.py (CPR or random reduction of a stream),
scripts/unicorn_run_analyzer.sh (GraphChi analyzer, one sketch per graph),
scripts/unicorn_score.py (modeler scoring). The unreduced sketching run had
written 167 of ~388 graph sketches (timing in logs/timing_perfile_graph07.txt,
about 13 s per graph at the tail) when it was stopped. No detection outcome yet,
no reduced runs yet.

To resume: regenerate the edge lists from the raw tarballs with the converter,
rerun unicorn_run_analyzer.sh, then the reduced streams at CPR 5.6x and random
50%, then unicorn_score.py. Parsed graphs and sketches were removed to free disk.
