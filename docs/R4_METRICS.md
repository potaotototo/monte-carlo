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
- per accepted block: worker-publication through coordinator-acceptance latency,
  including completion-queue residence, validation, and (for durable runs)
  result persistence and any checkpoint triggered by that block;
- scheduler: total time blocked because the bounded assignment queue was full;
- coordinator: completion-queue wait and result validation/consumption time;
- queues: maximum observed depth, updated while the existing queue mutex is
  held so no second synchronization mechanism is introduced;
- final fixed-position reduction time and total invocation time;
- maximum fixed-tree leaf backlog in blocks and bytes. The current final-only
  tree retains every accepted/recovered leaf, so a successful run reaches the
  full block universe; this is a memory-pressure metric, not a dirty-segment
  or queue-depth metric;
- durable open/recovery time;
- per newly installed result and per newly installed checkpoint latency;
- durable stage totals for write, file `fsync`, rename, and directory `fsync`,
  plus installed file counts and bytes.

Queue wait fields count condition-variable blocking caused by a full or empty
queue. An immediately successful push/pop contributes zero; mutex acquisition
and ordinary queue-operation overhead are intentionally excluded. Per-worker
counters accumulate in worker-local storage and publish once at thread exit,
while scheduler/coordinator totals remain local to their owning threads. This
avoids turning the measurement object itself into a false-sharing hotspot.

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
- `block_commit_ns[block_id] == 0` means the block was recovered, not accepted,
  or the invocation failed before commit samples could be finalized;
- an empty checkpoint vector means no checkpoint was installed.

All per-block, worker, and expected checkpoint storage is allocated before
threads start or before a durable directory is modified. Direct store users can
checkpoint more often than the configured cadence; once the preallocated sample
capacity is full, the manifest still commits and `checkpoint_samples_dropped`
increments without allocating or throwing. This preserves durability semantics
when observability capacity is exhausted.

Clock measurements of actual operations are expected to be positive on the
supported platforms. If the clock cannot resolve a positive interval, an actual
operation is recorded as one nanosecond so it cannot collide with the missing
sentinel. Percentile calculation ignores zero sentinels and returns
`null` when no observations remain. It uses the nearest-rank definition; the
accepted quantile range is `[0, 1]`, with zero selecting the minimum and one the
maximum. CLI summaries currently emit p50, p95, and p99.

The vectors are bounded by `max_materialized_blocks`, the same limit that guards
the eager block universe. This makes opt-in memory cost explicit: the three
per-block `uint64_t` vectors require 24 bytes per materialized block, with up to
another 8 bytes per expected checkpoint plus worker records and vector
bookkeeping. Commit timing stages a steady-clock marker in its already allocated
output slot while a run is active, avoiding a fourth per-block allocation. A
failed invocation clears all commit samples before throwing so a marker is never
reported as a latency. The disabled path allocates none of this storage.

## Command-line and benchmark use

Add `--metrics` to `run_simulation` to emit a nested JSON object. A completed
durable restart correctly reports no workers, no block/persistence latency
samples, and zero newly installed files, while still reporting recovery-open,
reduction, and total elapsed time.

`benchmark_scaling` pairs a metrics-disabled and metrics-enabled execution for
every repetition, alternating their order. It reports both throughputs, the
observed instrumentation throughput loss, median block/commit p50/p95/p99 latency,
queue peaks, scheduler wait, coordinator wait, and coordinator-consumption
columns. Parallel efficiency divides by the workers actually used, rather than
workers requested, because a run with fewer blocks cannot activate surplus
threads. Every pair is also checked for bitwise-identical final aggregates.

`metrics_throughput_loss_percent` is
`100 * (1 - metrics_rate / metrics_disabled_rate)`. It is deliberately named a
throughput loss: if instrumentation doubles elapsed time, the loss is 50%, while
elapsed-time overhead would be 100%. Negative values mean the observed run was
faster and should normally be treated as measurement noise.

Timing data is noisy and machine-specific. Use repeated runs, compare medians,
pin workload/build/power conditions, and treat small differences as noise.

Do not share one metrics object between overlapping runs or stores. A top-level
run and `DurableRunStore::open` reset the supplied object, and the individual
metrics fields are not a general-purpose concurrent aggregation API.

## Phase-1 acceptance gates

- Metrics-enabled and disabled executions produce bit-for-bit identical final
  aggregates.
- Worker block/scenario counters cover the executed universe exactly.
- Queue wait counters distinguish immediate operations from real blocking.
- Queue peaks never exceed configured bounded capacities.
- A fresh durable run reports the exact number of metadata, result, and
  checkpoint files installed under the selected checkpoint policy.
- A completed durable restart reports zero computation and zero new writes.
- Direct store use reports open/result/checkpoint timing; excess checkpoint
  samples degrade through an explicit dropped-sample counter.
- Invalid or non-finite percentile quantiles fail closed.
- Optimized, ASan/UBSan, ThreadSanitizer, and CMake/CTest suites pass.

## Work completed in R4 phase 2

The compute/queue, aggregation-strategy, durable checkpoint, real process-crash,
and 10,000-block recovery sweeps are specified in `R4_BENCHMARKS.md`, together
with captured local baselines and the resulting default-policy decisions.

## Deferred work

R4 does not add time-series queue sampling, HDR histograms, hardware performance
counters, cross-process metric aggregation, CPU affinity, or a deterministic
multi-worker crash scheduler. Process CPU utilization can exceed 100% because
it is total CPU time divided by wall time across all threads. Coordinator active
time is an explicit proxy for coordinator-core pressure, not an OS-attributed
per-thread CPU counter.
