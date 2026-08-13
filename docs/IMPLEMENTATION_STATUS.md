# Implementation status and phase handoff

## R0 — validated Monte Carlo pricer

Implemented:

- exact-discretization GBM paths;
- streaming European and arithmetic Asian payoffs without full path buffers;
- Welford mean/variance statistics and confidence intervals;
- Black–Scholes analytic call price;
- deterministic convergence test against the analytic price;
- antithetic pair-mean aggregation and a variance-reduction test.

Acceptance evidence: `make test` covers the analytic oracle and asserts that a 200,000-scenario estimate falls within four estimated standard errors of the Black–Scholes price.

## R1 — deterministic parallel engine

Implemented:

- Random123 Philox4x32-10 algorithm with the zero-counter/zero-key known-answer vector;
- fixed and validated RNG counter layout v1;
- one-uniform/one-normal stateless inverse-CDF transformation;
- deterministic scenario blocks;
- bounded scheduler-to-worker and worker-to-coordinator queues;
- block-local aggregation;
- fixed-position reduction tree independent of result arrival order;
- exact same-build equality tests across one, two, four, and eight workers under queue capacity one.

R1 originally treated a duplicate block message as an error. The R1.5 coordinator below supersedes that provisional behavior with the final validation classification used by R2.

## R1.5 — pre-persistence hardening

Implemented:

- a compiled GBM kernel that hoists run-invariant drift, diffusion, and discount values out of the per-scenario path;
- fused antithetic pair simulation, preserving the pair-mean estimator while sharing one Philox/inverse-normal operation per pair and time step;
- canonical RunSpec and MeanVarianceV1 encoders using fixed-order little-endian integers and IEEE-754 binary64 bits;
- SHA-256 implementation checked against standard single- and two-block known-answer vectors;
- a golden RunSpec-v1 hash to catch accidental canonical-schema changes;
- versioned run hash, statistics schema, payload checksum, incarnation, and lease fields on in-memory result messages;
- a side-effect-free coordinator validator and tests for every result classification;
- a CSV scaling benchmark with warmup and median-of-repetitions reporting.

The refreshed baseline in `R1_5_BASELINE.csv` was captured on 2026-08-13 using an 8-core Apple M1 MacBook Air, Apple Clang 15, `-O3 -ffp-contract=off`, ten million one-step European scenarios per run, five measured repetitions, and a warmup. The hardened engine reached 97.3 million scenarios/second at block size 32,768 with about 62% eight-worker efficiency. This is a local regression snapshot, not a portable performance guarantee.

`block_size` remains intentionally outside the stochastic `run_spec_hash`: it changes the durable block universe and is captured by a separate `execution_layout_hash`. R2 persists and rejects mismatches in that identity and the build/runtime fingerprint.

## Pre-R2 edge-case hardening

Completed after the R1.5 audit:

- RNG version 2 uses 53 retained bits from two Philox words with a pinned open-interval endpoint rule;
- engine semantics version 2 and golden European/Asian block hashes prevent silent compatibility drift;
- unavailable variance, standard error, and confidence intervals are explicit rather than reported as zero;
- MeanVarianceV1 invariants are validated before checksum classification;
- execution-layout and build fingerprints are validated independently of stochastic identity;
- one-million-block and 256-thread safety limits prevent accidental eager-allocation or thread explosions;
- strict locale-independent parsing rejects partial, signed-unsigned, non-finite, and overflowed inputs;
- coordinator construction validates its complete block universe;
- derived GBM constants and every path step fail with contextual diagnostics on overflow or underflow;
- the pre-R2 optimized, ASan/UBSan, and TSan suites passed 19/19 tests.

## R2 — durable run store and recovery

Implemented:

- canonical storage-schema-v1 envelopes for immutable run metadata, block results, and full-snapshot manifests;
- CRC32C byte-integrity checks kept distinct from SHA-256 logical content identities;
- golden canonical hashes for all three durable record types;
- immutable `run_spec.bin` containing RunSpec, block layout, run ID, stochastic/layout hashes, and compiler/standard-library/runtime-platform fingerprint;
- build fingerprints covering runtime source, effective build flags, optimizer/FP policy, and compiled CPU features;
- result records containing the canonical block range, incarnation, lease, RNG/statistics versions, aggregate, and all compatibility identities;
- unique temporary writes followed by complete write, file `fsync`, close, same-filesystem rename, and source/destination directory `fsync`;
- manifest-only durable commit linearization, with orphan result files ignored during recovery;
- highest-valid-manifest selection with fallback from corrupt manifests or corrupt/missing referenced results;
- fail-closed recovery policy: resource/I/O failures do not cause fallback, and cleanup occurs only after successful validation/preflight;
- refusal to reconstruct missing metadata over a nonempty durable store;
- durable incarnation advancement before rescheduling, using the observed manifest sequence as an additional high-water mark;
- recovery of exact fixed-tree leaves and scheduling of only uncommitted blocks;
- terminal failed manifests carrying determinism-conflict diagnostics;
- benign duplicate/stale retry classification and explicit schema-v1 failure status codes;
- byte/file/free-space/manifest limits, completion-budget preflight, stale-temp cleanup, and exclusive coordinator locking;
- adaptive default checkpoint cadence, table-driven CRC32C, reserved codec buffers, and checkpointing without a full result-vector copy;
- CLI checkpoint/resume support through `--run-dir` and storage-policy options.

R2 handoff evidence: the optimized suite covered an interrupted eight-block run with two committed blocks and one orphan. Recovery restored two blocks, recomputed six, and produced a bit-for-bit identical fixed-tree aggregate to the clean run. Reopening the completed run restored all eight blocks, started zero workers, and did not change manifest sequence or incarnation. Additional tests covered incompatible specifications/layouts, CRC failure, manifest fallback, referenced-result corruption, persisted determinism failure, stable failure status encoding, benign retries, non-destructive operator-policy rejection, missing metadata, storage exhaustion, canonical format goldens, and concurrent-coordinator exclusion. The R2 optimized and sanitizer suites passed 27/27 tests; current full-suite evidence is recorded under R3.

The exact protocol and filesystem preconditions are documented in `R2_DURABLE_RECOVERY.md`.

## R3 — deterministic failure injection and replay

Implemented:

- nine stable named hooks before/after result and manifest file `fsync`, before/after rename, and after durable manifest installation but before in-memory commit-state update;
- real subprocess termination through reserved exit code 86 rather than exception simulation;
- immutable, atomically installed replay-schema-v2 descriptors containing the exact RunSpec, engine/store configuration, failure schedule, build identity, trace hash, block/checkpoint context, and a SHA-256 over the complete canonical record;
- deterministic seed-to-point/occurrence selection using a pinned SplitMix64 mapping;
- bounded descriptor parsing (64 KiB total, 64 fields, 4 KiB per line), strict canonical re-encoding, incremental constant-memory trace hashing, and fail-closed rejection of schema v1, nonzero reserved scheduler seeds, or reused evidence paths;
- descriptor-driven `replay_failure` verification of the complete replay identity in a new empty run directory;
- watchdog-supervised `run_crash_matrix` phases with failure-only retention and exact clean/recovered/completed-restart checks;
- deterministic variation of block size/count, checkpoint cadence, queue capacities, partial final blocks, and stochastic configuration, plus a topology-diversity gate for larger matrices;
- explicit single-worker replay restriction until a deterministic multi-worker completion scheduler exists;
- explicit separation between application process-crash validation and power-loss/filesystem fault injection.

Acceptance evidence: optimized, ASan/UBSan, and ThreadSanitizer builds pass 32/32 tests, including all nine hooks, replay-schema-v2 integrity and resource limits, immutable evidence, incremental trace hashing, and deliberate watchdog expiry. CMake Release/CTest also passes. A fresh 1,000-seed run passed with 9/9 failure-point coverage, bitwise clean-versus-recovered equality, exact block accounting, zero-work completed restarts, six block sizes, eight block counts, four checkpoint intervals, four assignment/completion queue modes, and partial final blocks. A saved post-install descriptor was independently reproduced with the complete replay identity and ordered trace hash `b0514886f042433508499861e47e503895241ee8c9c9ba08ee03edf1013acfab`. LeakSanitizer is unavailable on this macOS runtime, so ASan used `detect_leaks=0`. TSan used `report_thread_leaks=0` because deliberate `_exit` intentionally abandons live child threads; race reports remained enabled and fatal.

The exact R3 contract and tool usage are documented in `R3_FAILURE_INJECTION.md`.

## R4 phase 1 — runtime and persistence instrumentation

Implemented:

- an optional caller-owned metrics object that leaves existing call sites and
  the default hot path unchanged;
- monotonic nanosecond timings for workers, scheduler backpressure,
  coordinator waits/consumption, deterministic reduction, and total runtime;
- per-block computation and durable-result latencies with explicit missing
  samples for recovered blocks;
- bounded-queue peak depths observed under the existing queue locks;
- durable open, checkpoint, write, file-sync, rename, directory-sync, installed
  file-count, and byte-count measurements;
- nested CLI JSON through `--metrics` and expanded scaling-benchmark CSV
  columns for p50/p95/p99 block latency and contention indicators;
- exact-result-neutrality, counter-coverage, bounded-depth, durable file-count,
  and zero-work completed-restart tests.

Design decisions: metrics remain outside every persisted schema and identity;
the metrics pointer is non-owning; zero is a missing per-block sample; latency
percentiles ignore missing samples and use nearest rank. The benchmark reports
metrics-enabled throughput and computes efficiency from active workers, not
surplus requested workers.

The detailed contract, overhead caveats, edge cases, and deferred R4 work are
documented in `R4_METRICS.md`.

Acceptance evidence: optimized, ASan/UBSan, and ThreadSanitizer builds pass
34/34 tests; an independent CMake Release build passes CTest; and a 256-seed
post-instrumentation R3 regression passes with 9/9 crash-point coverage and the
full topology-diversity gate.
