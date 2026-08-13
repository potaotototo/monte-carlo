# R4 phase 1 — opt-in runtime metrics

R4 phase 1 adds measurements needed to distinguish computation, queue pressure,
coordinator work, reduction, and durable I/O. It does not change the stochastic
experiment, block layout, reduction tree, or storage schema.

## API and compatibility contract

`run_parallel`, `run_parallel_durable`, and `DurableRunStore::open` accept an
optional caller-owned `RuntimeMetrics*`. The default is `nullptr`, preserving the
pre-R4 API call form and avoiding metric vectors and clock reads when measurement
is disabled. The remaining null checks occur at block/queue boundaries, never in
the per-scenario numerical loop. A pointer passed directly to `DurableRunStore::open`
must outlive that store because the pointer is non-owning.

Metrics are diagnostic output, not deterministic state:

- they are not included in the RunSpec, execution-layout hash, build identity,
  result checksum, manifest, or recovery compatibility decision;
- enabling them may perturb thread scheduling and wall-clock performance;
- enabling them must not change a block aggregate, payload checksum, or final
  fixed-tree result;
- durations use `std::chrono::steady_clock` and are reported as integer
  nanoseconds; they are suitable for elapsed-time comparisons, not timestamps;
- a fresh invocation resets the supplied structure rather than accumulating
  data from a prior invocation.

Additive duration and byte counters saturate at `UINT64_MAX` instead of wrapping.
Reaching that value means the aggregate is censored; it is not an exact sample.

R4 changes runtime source and therefore changes the build fingerprint. Existing
R3 durable stores and replay descriptors remain tied to the tagged R3 binary;
that is the intended build-compatibility behavior, not a stochastic-schema
change.

## Measurement boundaries

The runtime records:

- per worker: completed blocks/scenarios, assignment wait, block computation,
  and completion-queue wait;
- per executed block: computation latency indexed by stable `block_id`;
- scheduler: total time spent submitting to the bounded assignment queue;
- coordinator: completion-queue wait and result validation/consumption time;
- queues: maximum observed depth, updated while the existing queue mutex is
  held so no second synchronization mechanism is introduced;
- final fixed-position reduction time and total invocation time;
- durable open/recovery time;
- per newly installed result and per newly installed checkpoint latency;
- durable stage totals for write, file `fsync`, rename, and directory `fsync`,
  plus installed file counts and bytes.

Durable file counts and bytes advance only after the complete atomic-install
protocol succeeds. A pre-existing immutable file with identical bytes is not a
new installation. Stage timings can describe completed stages of an attempt
even if a later stage fails; they must not be interpreted as a commit count.

## Missing samples and percentiles

Per-block vectors use zero as an explicit missing-sample sentinel:

- `block_compute_ns[block_id] == 0` means the block was recovered or not
  executed in this invocation;
- `result_persist_ns[block_id] == 0` means no new durable result was installed
  for that block;
- an empty checkpoint vector means no checkpoint was installed.

Clock measurements of actual operations are expected to be positive on the
supported platforms. If the clock cannot resolve a positive interval, an actual
operation is recorded as one nanosecond so it cannot collide with the missing
sentinel. Percentile calculation ignores zero sentinels and returns
`null` when no observations remain. It uses the nearest-rank definition; the
accepted quantile range is `[0, 1]`, with zero selecting the minimum and one the
maximum. CLI summaries currently emit p50, p95, and p99.

The vectors are bounded by `max_materialized_blocks`, the same limit that guards
the eager block universe. This makes opt-in memory cost explicit: two
`uint64_t` vectors require 16 bytes per materialized block, excluding vector
bookkeeping. The disabled path allocates neither vector.

## Command-line and benchmark use

Add `--metrics` to `run_simulation` to emit a nested JSON object. A completed
durable restart correctly reports no workers, no block/persistence latency
samples, and zero newly installed files, while still reporting recovery-open,
reduction, and total elapsed time.

`benchmark_scaling` enables metrics for every measured repetition and adds
median block p50/p95/p99 latency, queue peaks, scheduler wait, coordinator wait,
and coordinator-consumption columns. Its wall-clock throughput therefore
measures the metrics-enabled path. Parallel efficiency divides by the workers
actually used, rather than workers requested, because a run with fewer blocks
cannot activate surplus threads.

Timing data is noisy and machine-specific. Use repeated runs, compare medians,
pin workload/build/power conditions, and treat small differences as noise.

Do not share one metrics object between overlapping runs or stores. A top-level
run and `DurableRunStore::open` reset the supplied object, and the individual
metrics fields are not a general-purpose concurrent aggregation API.

## Phase-1 acceptance gates

- Metrics-enabled and disabled executions produce bit-for-bit identical final
  aggregates.
- Worker block/scenario counters cover the executed universe exactly.
- Queue peaks never exceed configured bounded capacities.
- A fresh durable run reports the exact number of metadata, result, and
  checkpoint files installed under the selected checkpoint policy.
- A completed durable restart reports zero computation and zero new writes.
- Invalid or non-finite percentile quantiles fail closed.
- Optimized, ASan/UBSan, ThreadSanitizer, and CMake/CTest suites pass.

## Deferred R4 work

Phase 1 intentionally does not add time-series queue sampling, HDR histograms,
hardware performance counters, cross-process aggregation, or an automated
durable checkpoint-cadence sweep. The next R4 slice should capture repeatable
compute and persistence baselines and use these measurements to choose defaults,
without altering R3 recovery semantics.
