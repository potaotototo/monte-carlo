# R4 phase 2 — benchmark and tuning contract

R4 phase 2 adds repeatable experiments for the compute scheduler, aggregation
strategy, durable checkpoint cadence, real process-crash recovery, and the
10,000-block release gates. The tools print CSV to standard output and never
modify a durable run supplied by the user. Persistence runs create a unique
child below the system temporary directory or `--workspace`, then remove only
that child on normal exit.

## Build and commands

```sh
make benchmark-tools

./build/benchmark_scaling \
  --scenarios 5000000 --steps 1 --repeats 3 --max-workers 8 \
  --queue-capacities 0,1,8

./build/benchmark_aggregation \
  --scenarios 1000000 --steps 1 --repeats 3 --max-workers 8 \
  --block-size 2048

./build/benchmark_persistence \
  --scenarios 200000 --steps 1 --workers 8 --repeats 3 \
  --block-sizes 2048,10000 --checkpoint-intervals 1,4,16,64
```

Every row includes the scenario count, time-step count, and repetition count.
Warmups are excluded. Reported durations and latency summaries are medians of
the measured repetitions unless a column explicitly describes one run's exact
count. Process CPU utilization is process CPU time divided by wall time, so a
multithreaded value may exceed 100%.

## Compute and queue sweep

`benchmark_scaling` tests block sizes 512, 2,048, 8,192, and 32,768 at worker
counts 1, 2, 4, 8, and a requested maximum when it is not already present.
Queue capacity `0` is the production automatic policy of twice the active
worker count. Each repetition contains a metrics-off and metrics-on run; their
order alternates to reduce ordering bias, and their final aggregates must be
bitwise identical.

`metrics_throughput_loss_percent` is
`100 * (1 - metrics_rate / metrics_disabled_rate)`. It is not elapsed-time
overhead; a twofold slowdown is a 50% throughput loss and a 100% elapsed-time
overhead. The explicit name prevents those two valid but different conventions
from being confused. An earlier R4 draft called this column
`metrics_overhead_percent`; the final header is a semantic clarification and
does not alter the recorded numeric values.

The metrics-on run reports compute and publication-to-acceptance P50/P95/P99,
queue peaks, actual condition-variable blocked time, coordinator active time
and acceptance rate, and the fixed-tree leaf backlog. A commit sample starts
when a worker publishes a result and ends after coordinator acceptance. On a
durable run it therefore includes result installation and any checkpoint
triggered by that result. It is not merely file-write latency.

`coordinator_consume_ns / metrics_median_seconds` is a useful active-time proxy
for one coordinator core, but it is not OS-attributed per-thread CPU time. The
fixed reduction is final-only, so successful runs retain all leaves and the
maximum reduction backlog equals the complete block universe. This exposes the
current memory scaling rather than pretending the tree reduces incrementally.

## Aggregation strategy sweep

`benchmark_aggregation` compares:

- a global mutex around each Welford update;
- a relaxed atomic sum implemented with a portable compare/exchange loop;
- unpadded static worker-local Welford state;
- 64-byte-aligned worker-local state; and
- the production deterministic block tree.

The first four are benchmark-only alternatives. Mutex and atomic arrival order
is scheduler-dependent; the atomic case reports only a mean and deliberately
does not provide variance. `atomic_lock_free` records the host implementation,
but lock freedom does not remove cache-line contention. Worker-local strategies
use a static range split and merge in worker order, so their low-order bits are
not the production determinism contract. Only the production row exercises the
real scheduler, stable block leaves, and fixed tree.

## Persistence and crash sweep

`benchmark_persistence` measures a clean metrics-off run, a fresh metrics-on
durable run, a zero-work completed restart, and a real injected process exit for
each block-size/cadence pair. It verifies every durable/recovered aggregate
against the clean fixed-tree result bit for bit.

The parent resolves its own executable to a canonical executable file at
startup, including when invoked by a bare name through `PATH`, and the crash
child re-executes that resolved file. Worker and block/materialization limits are
validated before a temporary workspace is created or a CSV header is emitted.
Normal completion removes the unique workspace explicitly; cleanup failure is a
reported nonzero exit rather than a silently successful benchmark. Exceptional
unwinding retains a non-throwing best-effort cleanup and emits a warning if that
cleanup fails.

Failure replay remains deliberately single-worker. The crash child exits at
`ResultAfterRename` just before the selected result is accepted. When possible,
the selected occurrence is immediately before the second periodic checkpoint,
so one checkpoint exists and up to one cadence of completed work is lost. The
parent verifies the immutable replay descriptor and sequential block ID before
measuring recovery.

Two recovery counters must not be confused:

- `recovery_computed_scenarios` is all work missing from the latest manifest,
  including scenarios the crashed process had not reached;
- `recomputed_scenarios_after_crash` is the subset already computed by the
  crash child but not committed, hence duplicate work;
- `max_recomputed_scenarios` is the cadence/block-size upper bound, capped by
  the complete run size.

The injection is an application-process crash on the documented local POSIX
flush protocol. It is not a power-loss, torn-write, device-cache, or kernel
fault benchmark.

`durable_throughput_loss_percent` is
`100 * (1 - durable_rate / non_durable_rate)` and compares the complete durable
path with the non-durable path; it includes immutable result files and mandatory
manifests. The earlier `throughput_overhead_percent` header was renamed without
changing its numeric values.
It must not be called checkpoint-only overhead. Checkpoint-only cost is
estimated by comparing cadences with the same workload and result-file count.
The release gate uses
`100 * (1 - sparse_checkpoint_rate / final_only_rate)`, so it is specifically a
checkpoint throughput-loss gate rather than an elapsed-time-overhead gate.

## Captured local evidence

The CSV snapshots were captured on 2026-08-14 on an Apple M1 MacBook Air with
8 cores (4 performance, 4 efficiency), 8 GB RAM, macOS 14.5, Apple Clang 15,
`-O3 -ffp-contract=off`, AC power, and low-power mode disabled. The APFS data
volume had about 7.8 GiB free and was reported at 97% capacity. No CPU affinity,
exclusive-host reservation, or hardware counter collection was available, so
small differences and negative measured throughput loss are treated as noise.

Captured artifacts:

- `R4_SCALING_BASELINE.csv`: 48 compute/queue rows;
- `R4_AGGREGATION_BASELINE.csv`: 20 strategy rows;
- `R4_PERSISTENCE_BASELINE.csv`: 8 repeated cadence rows;
- `R4_RECOVERY_10K_BASELINE.csv`: 10,000 committed-result recovery with
  1,000-scenario blocks;
- `R4_DURABLE_TARGET_BASELINE.csv`: 100 million scenarios, 10,000 blocks of
  10,000 scenarios, and target-scale cadences;
- `R4_CHECKPOINT_GATE_REPEATS.csv`: three independent target-scale cadence
  sweeps captured from commit `fff2bc6`, preserving all six raw rows and the
  alternated cadence order.

## Findings and decisions

1. Keep the deterministic tree. At eight workers it reached 71.3 million
   scenarios/s. The global mutex fell from 17.4 million/s at one worker to
   4.6 million/s at eight; the lock-free atomic sum fell from 19.2 to
   7.9 million/s. Shared aggregation is conclusively the wrong design.

2. Keep `block_size=2048`. The best metrics-off eight-worker rates for 2,048,
   8,192, and 32,768 were all about 72.7–73.2 million/s. The difference is too
   small and noisy to justify the larger per-block latency and crash duplicate
   work of a larger default.

3. Keep automatic queue capacity at twice active workers. Capacity one caused
   material regressions in several rows, while capacity eight and automatic
   capacity traded small, inconsistent wins. There is no evidence for a
   universal explicit replacement.

4. Raise the automatic full-manifest cadence floor from 64 to 1,024 blocks. In
   the 10,000-block target run, cadence 64 installed 158 manifests and delivered
   36.5 million scenarios/s, about 24.6% below the final-only cadence. Cadence
   1,024 installed 11 manifests and reached 48.7 million/s, with no measurable
   penalty relative to final-only checkpointing on this host. The cost is an
   explicit increase in the automatic worst-case duplicate-work window from 64
   to 1,024 blocks; callers can still select a smaller positive cadence.

5. Do not interpret all durability cost as checkpoint cost. At the 100-million
   scenario scale, cadence 6,000 ran at 49.2 million scenarios/s with about
   1.22 seconds of work per periodic interval. The one-periodic-checkpoint run
   was within noise of the final-only run, while immutable result installation
   remained the dominant durable cost.

6. Do not close a performance gate from a favorable median when the host is
   unstable. Three independent target-scale sweeps, ordered 6,000/10,000 then
   10,000/6,000 then 6,000/10,000, measured checkpoint throughput losses of
   1.459%, 25.445%, and -2.360%. The median is 1.459%, but the range crosses the
   10% gate and the second capture's non-durable time rose from about 1.05s to
   1.92s. The evidence therefore diagnoses uncontrolled host variability rather
   than establishing a stable checkpoint cost.

## R4 target status on this host

| Target | Evidence | Status |
|---|---:|---|
| Single thread ≥1M scenarios/s | best metrics-off 18.7M/s | Pass |
| Parallel efficiency ≥60% through 8 workers | best 4-worker 72%; best 8-worker about 50% | Open at 8 |
| Recoverable ≥5M/s with checkpoints no more often than 1/s | 49.2M/s, cadence ≈1.22s | Pass |
| Coordinator ≤25% of one core before 8 workers | default 2,048/auto row active-time proxy ≈6.9% | Pass by proxy |
| P99 commit <50ms for 10,000-scenario blocks | 9.23ms at cadence 6,000 | Pass |
| Checkpoint throughput loss <10% | three target-scale losses: -2.36%, 1.46%, 25.44%; median 1.46%, but range crosses gate | Open; host capture unstable |
| Recovery open <2s for 10,000 committed blocks | completed-open 0.56s at target scale | Pass |

The eight-worker efficiency target is intentionally left open. The current M1
has four performance and four efficiency cores, and this capture followed
sustained builds and I/O, but those facts do not turn a measured miss into a
pass. A later tuning phase should profile the metrics-off eight-worker path on
an isolated host before changing the scheduler or claiming the gate.

The checkpoint throughput-loss gate likewise remains open. Three independent
target-scale repetitions now establish both a median and an observed spread,
but one capture exceeds the gate by a large margin and the non-durable baseline
varied by about 84%. A later capture needs an isolated, thermally stable host and
the same alternating-order protocol. The original one-repetition CSV remains
unchanged evidence; the six new raw rows are stored separately rather than
rewriting history.
