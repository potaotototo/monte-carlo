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

Post-implementation audit fixes:

- queue waits now count only actual full/empty condition-variable blocking,
  rather than charging every push/pop call as contention;
- worker, scheduler, and coordinator accumulation is thread-local until join,
  avoiding measurement-induced false sharing;
- direct `DurableRunStore` users receive open, result, and checkpoint latency;
- metric vectors are preallocated before execution/durable mutation, and excess
  direct checkpoint samples are counted and dropped without affecting commit.

Design decisions: metrics remain outside every persisted schema and identity;
the metrics pointer is non-owning; zero is a missing per-block sample; latency
percentiles ignore missing samples and use nearest rank. The benchmark reports
metrics-enabled throughput and computes efficiency from active workers, not
surplus requested workers.

The detailed contract, overhead caveats, edge cases, and deferred R4 work are
documented in `R4_METRICS.md`.

Acceptance evidence after the metrics bug audit: optimized, ASan/UBSan, and
ThreadSanitizer builds pass 36/36 tests; an independent CMake Release build
passes CTest; and a 256-seed post-instrumentation R3 regression passes with 9/9
crash-point coverage and the full topology-diversity gate.

## R4 phase 2 — benchmark matrix and measured tuning

Implemented:

- paired metrics-disabled/metrics-enabled block-size, worker-count, and bounded
  queue-capacity sweeps with bitwise result-neutrality checks;
- global-mutex, portable atomic-sum, unpadded worker-local, padded worker-local,
  and production deterministic-tree aggregation comparisons;
- durable block-size/checkpoint-cadence sweeps covering result, checkpoint,
  publication-to-acceptance, completed-restart, file-count, byte-count, and CPU
  measurements;
- a real single-worker R3 process crash in every persistence row, including
  immutable replay-descriptor verification, recovery-open timing, total
  recovery work, duplicate-work accounting, and the theoretical recomputation
  bound;
- focused 10,000-committed-block and 100-million-scenario target-scale runs;
- a dedicated balanced AB/BA checkpoint gate harness with retained raw pairs,
  robust spread, deterministic bootstrap interval, Theil–Sen drift, explicit
  order-effect checks, and machine-readable component/pass flags;
- CMake smoke tests with CSV-header contract checks for all four tools.

Post-implementation audit fixes and decisions:

- replaced unavailable `atomic<double>::fetch_add` with a relaxed portable
  compare/exchange loop;
- initialized fixed-tree backlog correctly on zero-work completed restarts;
- ensured failed invocations clear staged commit timestamps rather than exposing
  them as latency samples;
- separated all recovery computation from genuinely duplicated work after a
  crash;
- made every CSV row self-describing and named the measured throughput-loss
  convention explicitly so it cannot be confused with elapsed-time overhead;
- centralized canonical self-executable resolution for the persistence
  benchmark, crash matrix, replay tool, and crash-spawning tests, including bare
  invocations through `PATH`;
- preflighted benchmark inputs before warmup/CSV output and made temporary-
  workspace cleanup failures observable;
- retained the deterministic tree, 2,048-scenario block default, and automatic
  queue sizing because alternatives did not show a stable end-to-end win;
- raised the automatic full-manifest cadence floor from 64 to 1,024 blocks.
  On the target-scale run this reduced the manifest count from 158 to 11 and
  recovered about 25% throughput, at the documented cost of a larger default
  crash-recomputation window.

Local target evidence: single-thread throughput, recoverable throughput,
coordinator active-time proxy, 10,000-scenario P99 commit latency, and
10,000-block recovery-open time passed their R4 gates. Sparse checkpoint
throughput loss remains open: three exploratory target-scale sweeps produced a
1.46% sample median but a -2.36% to 25.44% range, with an 84% swing in the
non-durable baseline. With `n=3`, the median is descriptive only and supplies no
reliable uncertainty estimate. The raw alternating-order rows are retained
rather than treating the favorable median as a pass. Release work must add at
least 20 balanced AB/BA pairs, robust spread statistics and a 95% interval,
control-drift/order-effect checks, and require the interval's upper bound to be
below 10%; this evidence gate does not block independent R5 model development.
That harness is now implemented with the predeclared 20-pair/10%/5%/5% policy;
only the isolated-host capture remains. Harness availability is not recorded as
a performance pass.
The eight-worker efficiency capture reached about 50%, below the 60% target,
although four-worker efficiency remained above 70%; that gate stays open rather
than being waived. Exact commands, limitations, results, and CSV artifacts are
documented in `R4_BENCHMARKS.md`.

Final R4 phase-2 verification: the optimized, ASan/UBSan, and ThreadSanitizer
suites each pass 36/36 tests; a fresh CMake Release build passes all eight CTest
targets, including the benchmark CLI contracts, early-rejection checks, and
subprocess-tool help contracts; and
256 seeded process-crash schedules pass with 9/9 failure-point coverage and the
full topology-diversity gate.

Checkpoint-gate harness follow-up: 46/46 optimized unit tests pass. Fresh
Release, ASan/UBSan, and ThreadSanitizer builds each pass all 14 CTest targets,
including the paired-tool smoke test and bad-cadence rejection. The Makefile
build is warning-clean under the repository's full warning policy. These tests
validate execution and classification logic; they do not substitute for the
still-pending isolated-host 20-pair performance capture.

## R5 phase 1 — deterministic Heston model

Implemented:

- a model-tagged Heston parameter payload with canonical binary64 identity and
  a pinned full-truncation-Euler/log-asset discretization version;
- two fixed scenario-keyed Philox dimensions and explicit correlated-normal
  construction, without mutable per-worker RNG state;
- European and arithmetic Asian payoff paths, plus fused two-driver
  antithetic-pair simulation and pair-mean aggregation;
- validation for parameter domains and derived numerical constants, while
  retaining mathematically valid zero-parameter and `rho=-1` or `rho=1`
  limits;
- structured Feller-ratio warnings rather than rejecting a valid but more
  bias-prone parameter set;
- Heston CLI inputs and JSON model/warning metadata, with the GBM-only
  `--volatility` option rejected for Heston;
- Heston-aware canonical durable metadata and replay descriptors; and
- mixed GBM/Heston seeded crash-matrix cases using the unchanged exactly-once
  manifest/recovery protocol.

Compatibility decision: RunSpec schema v1 is treated as the tagged schema its
existing `model_type` field anticipated. Its common prefix and GBM tail remain
byte-for-byte unchanged, preserving the golden GBM RunSpec hash and existing
GBM durable metadata. Heston has a separate model tail containing
`discretization_version, v0, kappa, theta, xi, rho`; the inactive GBM
volatility field is neither serialized nor hashed. Feller warnings are derived
from the authoritative persisted parameters after decode instead of being
duplicated in storage.

Phase-1 verification covers Heston identity/metadata round trips, warning and
parameter edges, a deterministic two-dimension RNG moment/correlation smoke
test, pathwise agreement with the constant-variance GBM limit, exact results
across worker counts, fused-antithetic estimator equality, and zero-work durable
recovery. Independent analytic/QuantLib prices and an external stream battery
were intentionally deferred to phase 2. The complete numerical and
compatibility contract is in
`R5_HESTON.md`.

Final R5 phase-1 verification: the optimized unit suite passes 42/42 tests;
Release, ASan/UBSan, and ThreadSanitizer builds each pass all 9 CTest targets,
including the Heston CLI warning contract; compiler static analysis reports no
issues; and a 64-seed mixed GBM/Heston process-crash matrix passes exact
clean/recovered equality with 9/9 failure-point coverage, both model types, six
block sizes, eight block counts, four checkpoint intervals, four queue modes,
and partial final blocks.

## R5 phase 2 — independent Heston and RNG validation

Implemented:

- a dependency-free semi-analytic continuous-time Heston European-call oracle
  isolated from the full-truncation simulation kernel;
- cancellation-resistant Riccati evaluation, including exact zero-`xi`, tiny
  positive-`xi`, time-varying deterministic-variance, and absorbing-zero limits;
- variance-normalized adaptive Fourier expansion with embedded error estimates,
  strike-phase segmentation, observed-tail convergence, and finite fail-closed
  work limits, replacing the unsafe universal raw-frequency cutoff 200;
- a five-case price grid independently reproduced by QuantLib 1.43 and SciPy,
  including QuantLib's Kahl–Jäckel stress test;
- an eight-case SciPy cutoff-sweep regression grid covering the short-maturity,
  low-variance, and non-ATM combinations that exposed the fixed-cutoff defect;
- Heston CLI analytic price and signed discretization-plus-sampling error; and
- a bounded, coordinate-explicit raw Philox stream adapter with portable
  little-endian binary and auditable hex modes.

The analytic grid gate is complete. The external adapter and its CI golden
contract are pinned. A predeclared 1 GiB PractRand 0.96 confirmation passes for
dimension 0, dimension 1, and their scenario-wise interleaving. Exploratory
anomalies and clean independent replications are retained rather than omitted.
Exact methodology and commands are in `R5_HESTON.md`, full-precision prices are
in `R5_HESTON_REFERENCE_GRID.csv` and
`R5_HESTON_ADAPTIVE_REFERENCE_GRID.csv`, and the external transcript is
summarized in `R5_PRACTRAND_0_96_RESULTS.md`.

Final R5 phase-2 verification: 44/44 optimized unit tests pass; Release,
ASan/UBSan, and ThreadSanitizer builds each pass all 12 CTest targets. The
predeclared PractRand confirmation processes three 1 GiB streams with no
anomalies at any emitted checkpoint. CLI JSON validation and Makefile/CMake
warning-clean builds also pass.

Adaptive-cutoff follow-up: the defect was isolated by reproducing the old C++
price with an independent SciPy integral truncated at 200, then expanding the
domain to convergence. The worst retained short-maturity case was understated
by 89.8%. Merely increasing the fixed limit was rejected because another
parameter regime could move the decay scale again. The implemented policy and
the research informing it are recorded in `R5_HESTON.md`; extreme moneyness
that exceeds the explicit numerical budget is deliberately reported as an
unavailable oracle.

Final adaptive-cutoff verification: 45/45 optimized unit tests pass. Fresh
Release, ASan/UBSan, and ThreadSanitizer builds each pass all 12 CTest targets;
compiler warnings and `git diff --check` are clean. ASan again uses
`detect_leaks=0` because LeakSanitizer is unavailable on this macOS runtime,
and TSan keeps race detection fatal while disabling only thread-leak reports
from intentional crash-child `_exit` paths.
