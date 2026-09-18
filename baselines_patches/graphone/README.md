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

Modified (full copies, upstream + our case 28/29 hooks and includes):
  test/plaingraph_test.cpp      case 28/29 dispatch.
  test/plain_to_edge.h          includes + run_window_query().
