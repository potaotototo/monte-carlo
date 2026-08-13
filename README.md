# Fault-Tolerant Parallel Monte Carlo Runtime

This repository implements the project described in the supplied technical plan as a sequence of independently testable releases. The current code is the R3 fault-tolerant runtime plus R4 observability and benchmark tooling: a validated GBM pricer, deterministic parallel block engine, crash-consistent local run store, restart recovery protocol, replayable process-crash harness, opt-in runtime/persistence metrics, and repeatable compute, aggregation, checkpoint, and recovery sweeps.

## What works now

- European and arithmetic Asian call pricing under risk-neutral GBM.
- Black–Scholes closed-form pricing as the European-option correctness oracle.
- Random123 Philox4x32-10 with a known-answer test.
- Versioned RNG counter layout: 40-bit scenario ID, 24-bit time step, 8-bit dimension, and 24-bit draw index.
- RNG version 2 combines two Philox words into a 53-bit binary64 uniform with a pinned open-interval endpoint rule.
- Stateless inverse-normal generation, so a draw has no hidden dependence on call order.
- Bounded assignment and completion queues with backpressure.
- Block-local Welford statistics and a fixed-position pairwise reduction tree.
- Identical block aggregates and final results across worker counts in the pinned build.
- Antithetic variates accumulated as pair-means rather than incorrectly treated as independent raw payoffs.
- Run-invariant GBM constants compiled once per worker and fused antithetic pair simulation that shares RNG work.
- Canonical little-endian run/statistics payloads with IEEE-754 float encoding and SHA-256 identities.
- Pure coordinator validation for duplicates, stale leases/incarnations, corruption, schema mismatches, and deterministic conflicts.
- Separate stochastic, execution-layout, and build identities for recovery compatibility.
- Build identity covers runtime source, build flags, optimization/FP policy, and compiled CPU features.
- Explicit unavailable small-sample statistics, semantic aggregate checks, strict parsing, and block/thread resource limits.
- Canonical CRC32C-protected metadata, immutable block-result, and full-manifest envelopes.
- Atomic write, file sync, rename, and directory sync installation on local POSIX filesystems.
- Recovery from the highest valid compatible manifest with orphan exclusion and incarnation advancement.
- Non-destructive recovery failure: policy/I/O errors do not trigger fallback or orphan cleanup.
- Exactly-once block contribution, terminal persisted determinism failures, storage preflight, and exclusive run-directory ownership.
- Nine named result/manifest crash points exercised through real child-process termination.
- Immutable SHA-256-protected replay-schema-v2 descriptors, watchdog-bounded seeded crash matrices, and descriptor-driven trace reproduction.
- Opt-in monotonic runtime metrics for workers, bounded queues, coordinator work, reduction, and durable write stages without changing deterministic identities.
- Publication-to-acceptance latency, fixed-tree backlog accounting, paired
  metrics-on/off throughput, aggregation-strategy comparisons, and real-crash
  checkpoint/recovery benchmarks.
- JSON output containing the estimate, standard error, 95% confidence interval, throughput, and analytic error where applicable.

R3 validates application-level process crashes at every R2 result/manifest persistence boundary. It does not emulate machine power loss, torn device writes, or filesystem/kernel faults; the durability claim remains scoped to tested local POSIX filesystems honoring the documented flush protocol.

## Build and test

Both a dependency-free Makefile and a CMake build are provided.

```sh
make test
make
make benchmark
make benchmark-tools
make r3-tools
```

Run a European call simulation:

```sh
./build/run_simulation \
  --scenarios 1000000 \
  --workers 8 \
  --block-size 2048 \
  --seed 42
```

Run a daily-monitored arithmetic Asian call with antithetic variates:

```sh
./build/run_simulation \
  --payoff asian \
  --steps 252 \
  --scenarios 200000 \
  --workers 8 \
  --block-size 2048 \
  --antithetic
```

Enable durable checkpoints and resume by rerunning the same command:

```sh
./build/run_simulation \
  --scenarios 1000000 \
  --workers 8 \
  --block-size 2048 \
  --seed 42 \
  --run-dir ./runs/european-seed-42 \
  --checkpoint-blocks 64
```

The run directory is exclusively owned while a coordinator is active. Recovery rejects changes to the RunSpec, block size, or pinned build/runtime identity; worker count may change.

If `--checkpoint-blocks` is omitted (or set to `0`), the runtime chooses at least 1,024 blocks per full snapshot and caps the planned number of periodic snapshots at 1,024. R4 raised the floor from 64 after a 10,000-block benchmark showed that 64 created 158 full manifests and reduced durable throughput by about 25%; 1,024 removed that measurable penalty on the same host. A nonzero value is an explicit recovery-granularity versus write-amplification choice.

Use `./build/run_simulation --help` for every option.

Add `--metrics` to emit p50/p95/p99 block and persistence latencies, queue peaks,
per-worker counters, coordinator timings, and durable write-stage totals. Metrics
are diagnostic, caller-local data; they are not persisted or included in any
deterministic identity.

Capture a repeatable scaling baseline as CSV:

```sh
./build/benchmark_scaling --scenarios 10000000 --repeats 5 --max-workers 8
```

Run the full R4 benchmark tools and see their contracts:

```sh
./build/benchmark_aggregation --scenarios 1000000 --repeats 3 --max-workers 8
./build/benchmark_persistence --scenarios 200000 --repeats 3
```

Run 1,000 deterministic crash schedules and retain only failures:

```sh
./build/run_crash_matrix \
  --workspace /tmp/mc-r3-matrix \
  --iterations 1000 \
  --first-seed 1 \
  --timeout-seconds 30
```

## Determinism contract

For RNG version 2, each uniform draw is a pure function of:

```text
(global_seed, scenario_id, time_step, dimension, draw_index)
```

Changing the worker count or completion order cannot change a block payload. Blocks occupy stable leaves, and the final aggregate is reduced through a fixed binary tree keyed by block ID. The tests currently assert exact same-machine equality across worker counts. The release-level cross-platform contract remains numerical equality within a declared tolerance because compiler, floating-point, and math-library choices can affect low-order bits.

With antithetic mode enabled, scenario IDs `(2j, 2j+1)` use opposite shocks and are accumulated as one observation `(X(2j) + X(2j+1)) / 2`. Scenario and block counts must therefore be even.

## Layout

```text
include/mc/       public runtime interfaces
src/rng/          Philox and inverse-normal implementation
src/models/       GBM path and payoff evaluation
src/aggregation/  Welford updates and pairwise merge
src/runtime/      scheduler/worker/coordinator execution path
src/metrics.cpp   opt-in R4 latency summaries and reset contract
src/failure_injection.cpp R3 hook tracing and replay descriptors
src/run_store.cpp durable file installation and recovery
src/persistence_codec.cpp canonical R2 binary formats
tools/            command-line entry points
tests/            mathematical, RNG, validation, and determinism tests
docs/             phased implementation notes
```

## Release status

| Release | Outcome | Status |
|---|---|---|
| R0 | Validated sequential mathematical core | Complete |
| R1 | Deterministic parallel block execution and scenario-keyed RNG | Complete |
| R1.5 | Frozen hashes/result validation and pre-persistence hardening | Complete |
| R2 | Durable block results, atomic manifests, and restart recovery | Complete |
| R3 | Deterministic crash injection and replay descriptors | Complete |
| R4 | Synchronization, coordinator, persistence, and recovery benchmarks | Phase 2 implemented; 8-worker efficiency gate open |
| R5 | Heston or another advanced risk/model extension | Planned |

See [docs/IMPLEMENTATION_STATUS.md](docs/IMPLEMENTATION_STATUS.md) for the phased handoff and acceptance evidence.
The durable file, commit, and recovery contract is specified in [docs/R2_DURABLE_RECOVERY.md](docs/R2_DURABLE_RECOVERY.md).
The crash-point, descriptor, matrix, and replay contract is specified in [docs/R3_FAILURE_INJECTION.md](docs/R3_FAILURE_INJECTION.md).
The opt-in measurement contract is specified in [docs/R4_METRICS.md](docs/R4_METRICS.md), and the benchmark methodology, captured baselines, tuning decisions, and remaining performance gate are in [docs/R4_BENCHMARKS.md](docs/R4_BENCHMARKS.md).
The first measured R1.5 scaling snapshot is in [docs/R1_5_BASELINE.csv](docs/R1_5_BASELINE.csv).
Known numerical, protocol, resource, and recovery edge cases—including the rationale for a 53-bit binary64 uniform—are tracked in [docs/EDGE_CASES_AND_RELEASE_GATES.md](docs/EDGE_CASES_AND_RELEASE_GATES.md).
