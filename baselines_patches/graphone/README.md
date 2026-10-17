# GraphOne harness modifications (drop-in copies)

baselines/ is gitignored (multi-GB checkouts), so the files we added or
modified inside GraphOne's tree are archived here verbatim. To reproduce:
clone GraphOne (github.com/the-data-lab/GraphOne), copy these files over
the corresponding paths, build graphone64.

Added:
  analytics/window_query.h      job 28: windowed neighbour delivery on the
                                archived adjacency, checksummed against the
                                shared query file (E20).
  analytics/concurrent_stream.h job 29: the E26 concurrent throughput
                                protocol (preload, W writers via batch_edge,
                                R readers on static views, settled-query
                                verification). Uses consistent (non-STALE)
                                views: STALE views taken during concurrent
                                archiving returned wrong answers for settled
                                windows on tc-cadets (150/3,276 queries at
                                8W:4R), an observation recorded in
                                results/e26_concurrent/README.md.

  analytics/e33_queries.h       jobs 34/35/36: the E33 matrix phases q1
                                (window scan over the whole adjacency, OpenMP
                                over vertices), q3 (time-respecting
                                earliest-arrival reachability, per-query hash
                                map of labels, popped in increasing-label
                                order) and q4 (as-of neighbours), formulas
                                character for character from docs/E33_SPEC.md,
                                checksummed against results/e33_matrix/<corpus>.ref.
                                Also the shared helpers: query-file reader,
                                the 1800 s phase budget (first 20 queries
                                timed, total projected, "Done = k/N"), and
                                the interpolating percentile. Jobs 30-33 are
                                upstream streaming jobs, so ours are 34-36.
                                job 28 gained per-query latency lines
                                ("Window Median us", "Window P95 us",
                                "Window Done") and the same budget; its
                                original output lines are unchanged.

Modified (full copies, upstream + our case 28/29/34/35/36 hooks and includes):
  test/plaingraph_test.cpp      case 28/29/34/35/36 dispatch.
  test/plain_to_edge.h          includes + run_window_query(),
                                run_scan_query(), run_reach_query(),
                                run_asof_query(). All four take consistent
                                (non-STALE) V_CENTRIC static views after
                                Make Graph has caught up; job 28 used
                                STALE_MASK|V_CENTRIC before E33, which is
                                equivalent once everything is archived but
                                drops a non-archived tail (static_view.h
                                sets edge_count = 0 under STALE_MASK).

Driver: scripts/run_graphone.py (jobs 20, 28, 34, 35, 36 on graphone64,
job 0 on graphone32; writes the legacy per-job CSV and the spec-shaped
e33_graphone.csv). Ground truth: tools/e33_ref.cpp.
