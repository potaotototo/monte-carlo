# Edge cases, rationale, and release gates

This document records boundary conditions found during the R1.5/R2 audits and
their resolution through R5. The durable implementation is described in
`R2_DURABLE_RECOVERY.md`; deterministic process-crash validation is described
in `R3_FAILURE_INJECTION.md`; and the Heston discretization contract is in
`R5_HESTON.md`.

## Status summary

| Area | Current behavior | Risk | Required by |
|---|---|---|---|
| Uniform precision | RNG v2 combines two Philox words into a 53-bit binary64 uniform | Golden endpoint and full-pipeline tests added | Complete |
| Small-sample statistics | Variance/SE are unavailable for `n < 2`; normal CI is unavailable below 30 observations | CLI emits JSON `null`, not false certainty | Complete |
| Aggregate validation | Count, finiteness, `m2`, and min/mean/max invariants are checked | Invalid aggregates are rejected before checksum classification | Complete |
| Execution identity | Stochastic, execution-layout, and build/runtime hashes are persisted separately | Recovery rejects every incompatible identity | Complete |
| Block materialization | Default maximum is one million blocks; checked before allocation | Lazy blocks remain a later scaling extension | Complete |
| Numeric parsing | Shared locale-independent parsers require complete valid input | Partial, signed-unsigned, overflow, and non-finite inputs are rejected | Complete |
| Test comparisons | Tolerance assertions reject NaN; European and Asian block hashes are golden | Numerical regressions cannot pass through NaN comparison behavior | Complete |
| Coordinator reconstruction | Canonical IDs/ranges, coverage, incarnations, epochs, and pair boundaries are checked after decode | Invalid snapshots fall back or fail closed | Complete |
| Extreme model parameters | Derived constants and every path step are checked contextually | Overflow and underflow fail closed | Complete |
| Crash handling | Atomic full manifests plus nine replayable process-crash hooks and immutable schema-v2 evidence | Power-loss/filesystem fault harness remains outside scope | R3 complete |
| Runtime metrics | Opt-in monotonic timings, bounded queue peaks, and durable stage counters | Scheduling perturbation and missing recovered-block samples are explicit | R4 phase 1 complete |
| Heston variance | Full-truncation Euler uses `max(v,0)` without rewriting the stored state | Discretization version is pinned and non-finite paths fail with context | R5 phase 1 complete |
| Feller violation | Valid run with a structured ratio warning below 1 | Bias risk remains visible in CLI and recovered metadata | R5 phase 1 complete |
| Heston analytic limit | Stable Riccati algebra avoids `(z-d)/xi^2` cancellation; exact `xi=0` uses integrated deterministic variance | QuantLib grid and tiny-`xi` regression pass | R5 phase 2 complete |
| Heston Fourier tail | Raw cutoff 200 silently understated short-maturity prices by up to 89.8% | Variance-normalized adaptive expansion, tail/error gates, finite work budget, and eight independent regressions | R5 phase 2 follow-up complete |
| Multi-driver RNG | Heston uses fixed dimensions 0 and 1, then explicit correlation | Individual and interleaved 1 GiB PractRand 0.96 confirmations pass | R5 phase 2 complete |

## 1. Uniform precision and why binary64 calls for 53 random bits

### Previous RNG version 1 mapping

Philox4x32-10 produces four 32-bit words, but RNG version 1 used only the first word:

```text
u = (word0 + 0.5) / 2^32
```

This guarantees `0 < u < 1`, but provides only `2^32` possible uniform values. The smallest possible value is `2^-33`, approximately `1.16e-10`. Applying the inverse normal CDF therefore limits realized shocks to roughly `|Z| <= 6.34`.

The effect is modest for ordinary vanilla-option runs, but it becomes material in three cases:

- scenario counts approach or exceed `2^32`, where repeated uniform values are unavoidable;
- tail-sensitive calculations such as VaR or CVaR are added;
- stress parameters make rare shocks contribute disproportionately to the estimator.

### Why 53 bits

IEEE-754 binary64 (`double`) has 53 bits of significand precision: 52 explicitly stored fraction bits plus the implicit leading bit for normal values. Consequently:

- an integer from `0` through `2^53 - 1` is represented exactly as a `double`;
- multiplying such an integer by the exact power of two `2^-53` introduces no decimal formatting or locale dependency;
- using more than 53 random bits cannot create more uniformly spaced binary64 values in `[0,1)` through this mapping, because the destination type cannot retain the extra low bits;
- using fewer bits unnecessarily reduces resolution and tail reach.

Thus, 53 bits match the useful precision of the destination type. This is not a claim that every binary64 value in `(0,1)` is equally likely; floating-point values are not evenly distributed by representable-value count. It is the conventional fixed-grid mapping with one random grid point per `2^-53` interval.

### Implemented RNG version 2 mapping

The runtime now uses two words from the same Philox output block:

```text
raw64 = (uint64(word0) << 32) | word1
r53   = raw64 >> 11                       // keep the high 53 bits
u     = double(r53) * 2^-53               // exact and in [0, 1)
if r53 == 0:
    u = 2^-54                             // deterministic half-bin endpoint rule
```

Properties:

- every retained integer is exactly representable;
- scaling by `2^-53` is an exact binary operation;
- `u` can never equal 1;
- the special zero rule ensures `u` is strictly positive for inverse-CDF use;
- the largest value is `1 - 2^-53`, extending the practical inverse-normal range to about eight standard deviations;
- all `2^53` input integers still map to distinct binary64 values.

The zero rule makes the first grid interval slightly asymmetric. That is preferable to returning zero, clamping to the smallest positive subnormal, or using decimal epsilon values, all of which create worse statistical or portability behavior. The exact rule must be included in the RNG version definition.

Changing from the 32-bit mapping changes every scenario path. The implementation therefore increments `rng_version` to 2. The counter bit layout remains version 1 and is one versioned component of the complete RNG interpretation.

### Acceptance tests

- Known Philox answer vectors remain unchanged.
- The 53-bit extraction has golden vectors for counters at zero and at field boundaries.
- Uniform values always satisfy `0 < u < 1`.
- The zero-output rule returns exactly `2^-54`.
- The maximum retained integer returns exactly `1 - 2^-53`.
- A golden block payload checksum pins the complete counter-to-normal-to-payoff path.
- Worker count and completion order do not change any block checksum.

## 2. Small-sample variance and confidence intervals

Sample variance divides by `n - 1`; it is undefined for fewer than two independent observations. Returning zero for `n < 2` incorrectly communicates certainty. This is especially easy to trigger with antithetic simulation because two raw scenarios form only one statistical observation.

The number of observations is:

```text
plain run:       observations = total_scenarios
antithetic run:  observations = total_scenarios / 2
```

Required behavior:

- `n == 0`: no estimate is available;
- `n == 1`: the mean exists, but sample variance, standard error, and confidence interval are unavailable;
- `n >= 2`: sample variance and standard error are available;
- for a user-facing confidence interval with very small `n`, either use an appropriate Student-t critical value or label the normal approximation explicitly;
- production-scale Monte Carlo runs may continue to use the normal approximation once the observation count is sufficiently large.

The API now represents unavailable values with `std::optional<double>` rather than encoding “unknown” as zero. The CLI emits JSON `null`. A normal-approximation interval is emitted only from 30 independent observations onward and is labelled `normal_approximation`.

Acceptance tests must cover zero observations at the accumulator level, one plain scenario, one antithetic pair, two observations, and a zero-variance but valid multi-observation sample.

## 3. Aggregate semantic validation

SHA-256 answers “are these the same canonical bytes?” It does not answer “do these bytes describe valid statistics?” A worker could produce an aggregate with the expected count and a matching checksum but impossible fields.

For `MeanVarianceV1`, validation now includes:

- `n` equals the block's expected observation count;
- `mean`, `m2`, `min`, and `max` are finite when `n > 0`;
- `m2` is nonnegative, allowing only a documented rounding tolerance if necessary;
- `min <= max`;
- `mean` lies within `[min, max]`, again allowing a scale-aware rounding tolerance;
- when `n == 1`, `m2 == 0` and `min == mean == max` within the declared comparison policy;
- empty aggregates use one canonical representation rather than arbitrary unused float fields.

This validation belongs at the coordinator boundary and again when durable payloads are decoded. A checksum stored beside a file detects accidental corruption but is not authentication: an adversary able to rewrite both the payload and checksum can create a new valid digest. Deliberate hostile storage modification is outside the MVP threat model.

## 4. Stochastic identity, execution identity, and build identity

One hash should not be made responsible for three different compatibility questions.

### Stochastic run identity

`run_spec_hash` identifies the logical experiment:

- model and payoff types and parameters;
- global seed and RNG version;
- scenario count and time grid;
- statistics schema and engine semantics version.

### Execution-layout identity

The `execution_layout_hash` identifies how the scenario space is committed and reduced:

- block size and resulting block count;
- block-partition schema version;
- reduction-tree version;
- antithetic boundary rules;
- any future work-partition policy that changes durable block identity.

`block_size` should remain outside the stochastic identity because it does not change the intended experiment, but it must be persisted and verified. Two layouts with the same `run_spec_hash` must not share a manifest or block-result namespace accidentally.

### Build identity

The result protocol also includes a build fingerprint covering:

- compiler and version;
- optimization and floating-point flags, including FMA contraction;
- standard library;
- target architecture and relevant CPU feature policy;
- engine semantics version, supported by golden European and Asian block payloads.

This matters even if the default final-result contract is tolerance-based. Blocks committed before a crash and blocks computed after recovery must not silently use different `exp`, inverse-CDF, or rounding behavior. Otherwise, the runtime can create a mixed-build aggregate without ever seeing a duplicate block whose checksum exposes the mismatch.

The fingerprint records compiler/version, standard library, architecture, C++ language level, optimizer state, FMA-contraction and fast-math policy, compiled CPU features, engine version, a digest of runtime source/header content, a digest of effective build flags, runtime OS release, and runtime C-library identity. On Apple platforms, the OS release pins the bundled `libSystem` implementation that supplies libc and libm. Recovery hard-fails on execution-layout or build/runtime incompatibility; it does not silently recompute from scratch in the same run directory. This closes the prior hole where `-O0` and `-O3` could share an identity despite producing different floating-point code.

## 5. Resource-exhaustion boundaries

The RNG layout supports up to `2^40` scenario IDs, but addressability is not an instruction to allocate one block record per scenario. With block size one, the current eager representation attempts to create `2^40` blocks plus coordinator hashes, received flags, leaves, and reduction scratch space.

The runtime should distinguish logical limits from execution limits:

- RNG coordinate limits prevent counter collisions.
- Execution limits prevent memory, thread, disk, or metadata exhaustion.

Implemented controls and later scaling options:

- `max_materialized_blocks` defaults to one million and has an explicit CLI override;
- block count is computed and checked before vector allocation;
- the effective worker count is capped at the number of blocks;
- requested worker threads are limited to 256 before reservation or creation;
- preflight worst-case block files, full manifests, bytes, file count, and retained free space before scheduling;
- eventually generate assignments lazily and use compact committed-block bitmaps or segments rather than duplicating the full block vector;
- enforce per-write byte/file/free-space limits in addition to the completion preflight.

Full manifests remain an intentional R2 simplicity decision, but the default cadence is adaptive: it is never more frequent than every 1,024 blocks and targets at most 1,024 periodic snapshots per run. The original 64-block floor produced 158 manifests in the R4 10,000-block target run and reduced durable throughput by about 25% versus final-only checkpointing; an explicit 1,024-block cadence removed the measurable penalty. The decision increases automatic-mode duplicate work after a crash from at most 64 to at most 1,024 blocks. Runs of 1,024 blocks or fewer therefore have no periodic snapshot between the initial and complete manifests; callers needing a tighter recovery window must select a smaller explicit interval. Explicitly requesting a smaller interval remains supported when recovery granularity is more valuable than write cost. Manifest CRC32C is table-driven, serialization reserves its expected capacity, and checkpointing no longer copies the full optional-result vector.

An out-of-memory exception is not an adequate normal control path because the operating system may terminate the process before C++ can report it.

## 6. Strict command-line parsing

The earlier use of `std::stoull` and `std::stod` accepted valid prefixes such as `10junk` and allowed surprising unsigned-sign behavior. Both tools now use shared strict conversion helpers.

The parsing helpers:

- require the entire argument to be consumed;
- reject leading minus signs for unsigned fields;
- reject overflow before narrowing to `size_t` or `uint32_t`;
- reject NaN and infinity through both parser and run-spec validation;
- use locale-independent conversion;
- distinguish malformed input from a valid value outside the supported range.

Integers use `std::from_chars`. The current libc++ lacks floating-point `from_chars`, so floating values use a classic-locale, no-whitespace stream with complete-consumption and finiteness checks.

## 7. NaN-safe test assertions

The common tolerance assertion must not be written only as:

```text
abs(actual - expected) > tolerance
```

If `actual` is NaN, the comparison is false, so the old test incorrectly passed. The helper now requires finite inputs and a valid tolerance before comparing, and a negative test pins this behavior.

Golden payload hashes are also necessary. A golden `run_spec_hash` protects serialization layout, but not GBM evolution, inverse-normal coefficients, discounting, or payoff evaluation. Engine version 2 now has golden European and Asian block checksums that pin the complete deterministic pipeline.

## 8. Coordinator and recovery-table invariants

The coordinator constructor validates the in-memory block universe before it becomes expected state. R2 reconstructs the same canonical universe and validates every decoded manifest/result against it:

- block IDs are unique, contiguous, and match their stable leaf positions;
- ranges are ordered, non-overlapping, gap-free, and cover exactly `[0, total_scenarios)`;
- every range is nonempty;
- antithetic block boundaries are even;
- all active blocks use the expected run incarnation;
- lease epoch high-water marks do not move backward;
- committed block records refer to files with matching internal metadata;
- the execution-layout hash matches the reconstructed block universe.

The coordinator remains single-owner in the MVP. `commit_result` is not a thread-safe concurrent object and must only be invoked by the coordinator thread unless synchronization is added explicitly.

## 9. Extreme numerical parameters

Finite inputs can still produce non-finite derived values. Examples include volatility squared overflowing, `rate * maturity` overflowing, a discount factor underflowing to zero, or repeated path exponentials overflowing during simulation.

The GBM kernel now precomputes and validates:

- `dt`;
- drift per step;
- diffusion per step;
- discount exponent and discount factor.

Worker errors include scenario ID and time step. A zero path price is rejected because GBM is strictly positive mathematically, so numerical underflow cannot silently become a valid zero payoff.

Heston applies the same fail-closed policy to its derived time-step scales,
discount factor, asset exponent, price, variance state, and Asian running sum.
A negative variance state is not itself an error: full truncation deliberately
uses `max(v,0)` in the next update. Treating every negative Euler state as a
failed path would silently turn the documented discretization into a different
algorithm. Correlation endpoints `-1` and `1`, zero volatility of variance, and
zero initial or long-run variance are valid limiting cases rather than divide-
by-zero errors.

The Heston Fourier oracle has a separate cancellation hazard: the common
closed-form factors `(z-d)/xi^2` and a log difference lose nearly all precision
for small positive `xi`. The implementation rewrites the first factor as
`-A/(z+d)` and evaluates `C(T)=r*i*phi*T+kappa*theta*integral(D(t),0,T)` using
stable `D(t)`. It does not switch to an approximate deterministic model at an
undocumented epsilon. Exact `xi=0` uses the analytic integrated variance
`theta*T + (v0-theta)*(1-exp(-kappa*T))/kappa`, with its `kappa=0` limit.

A second, independent hazard is truncating the Fourier domain. The former
512-node rule was accurate *within* `[0,200]`, so adding nodes did not expose
the omitted tail. For default parameters at maturity `1e-5`, it returned
`0.002564704292` instead of the independently converged `0.025256315544`, yet
looked numerically well behaved. Short maturity and low expected variance move
the characteristic-function decay to higher raw frequency; non-ATM strikes
also increase oscillation. A cutoff that is adequate for a long-maturity stress
case is therefore not automatically conservative for a short one.

The replacement normalizes frequency by the square root of expected integrated
variance, adaptively estimates local error with an embedded Gauss–Kronrod pair,
splits intervals by strike phase, and doubles the tail until three consecutive
slabs meet both magnitude and error criteria. Acceptance uses an explicit
option-price tolerance, not “the result stopped changing when printed.” Work,
depth, and tail expansion are bounded. If any bound is exhausted, the analytic
reference is unavailable; the Monte Carlo run remains valid and the CLI emits
`null` instead of a suspect benchmark.

This stopping rule is strong operational evidence but not a proof of an
analytic remainder bound. The release gate therefore also retains the five
short-maturity and three low-variance/moneyness SciPy cutoff-sweep references in
`R5_HESTON_ADAPTIVE_REFERENCE_GRID.csv`. The research background, alternatives,
constants, and exact decision are documented in `R5_HESTON.md`.

Expected integrated variance itself is evaluated with paired small-`kappa*T`
series weights. Using only `expm1` is not sufficient when `v0=0`: the direct
formula still subtracts nearly equal `theta*T` terms and can turn a positive
`O(kappa*T^2)` variance integral into zero. The series branch begins at
`kappa*T < 1e-4` and is pinned at `1e-16`.

## 10. R2 completion checklist

The durable schema is pinned by the following completed gates:

- [x] RNG version 2's 53-bit uniform mapping is implemented and pinned by golden vectors.
- [x] Small-sample variance and confidence intervals cannot report false certainty.
- [x] Aggregate semantic invariants are validated at the message boundary and after durable decode.
- [x] `execution_layout_hash` and build metadata are specified and validated in results.
- [x] Block/thread, durable byte/file, manifest-size, and free-space preflight limits exist.
- [x] CLI parsing is strict and locale-independent.
- [x] Test comparisons reject NaN, and golden block payload hashes cover European and Asian kernels.
- [x] Reconstructed block universes are structurally validated before scheduling.
- [x] Derived GBM constants and path evolution are checked before acceptance.
- [x] Execution/layout/build identities are persisted in every relevant envelope and verified during recovery.
- [x] CRC32C corruption, orphan exclusion, corrupt-snapshot fallback, and terminal failure behavior are tested.
- [x] Clean and recovered fixed-tree aggregates are exactly equal in the pinned build.
- [x] Optimized, ASan/UBSan, and ThreadSanitizer suites pass the complete test matrix recorded in the implementation status.

## 11. Recovery failure taxonomy and metadata authority

Manifest fallback is safe only when the candidate durable artifact itself is invalid. R2 therefore falls back for malformed envelopes, checksum failures, semantic inconsistencies, and missing or corrupt referenced result files. It does not fall back for operator size limits, capacity preflight failures, permission errors, or general storage I/O failures. Those conditions say the current process cannot safely recover; they do not prove the newest snapshot is bad.

Orphan cleanup happens only after a valid snapshot has been selected, every policy and capacity check has passed, and any required recovery manifest has been installed. This ordering prevents a failed open from turning a temporary policy choice into durable data loss.

`run_spec.bin` is not reconstructed when the directory otherwise contains regular files. Caller arguments are untrusted reconstruction input until checked against that immutable record; writing them first could permanently poison an existing store. The operator must restore the original metadata file or use a new empty directory.

Duplicate results and stale incarnations/leases are benign at-least-once-delivery events. They do not justify a terminal `Failed` manifest. Every other validation rejection remains terminal because it indicates incompatible identity, invalid data, corruption, or conflicting deterministic output. Persisted validation statuses use explicit schema-v1 numbers rather than enum ordinals so source reordering cannot silently rewrite the wire format.

R3 exercises this protocol by terminating real child processes at every result/manifest `fsync`, rename, and post-install in-memory boundary. The matrix covers all nine hooks and the replay tool verifies the ordered trace. This does not simulate loss of cached writes after machine power failure; that requires a filesystem-level fault harness.

## 12. R3 replay evidence and harness resource bounds

Replay descriptors are debugging evidence, so they must not silently drift or be replaced. Schema v2 appends SHA-256 over every preceding canonical byte, verifies that digest before parsing, and then requires exact canonical re-encoding. This checksum detects accidental corruption and edits; it is deliberately not described as authentication against an actor able to rewrite both content and checksum. Schema v1 is rejected because it did not protect fields outside the RunSpec and trace hashes.

Descriptor paths are single-assignment. Configuration rejects an existing path, while installation independently uses a synced temporary inode and an atomic no-replace hard link to close the check/install race. Reusing a path fails without changing the original evidence. This decision favors forensic preservation over a convenient overwrite mode; callers that want another sample must choose a unique path.

A generated v2 descriptor has 38 short ordered fields for GBM and 43 for
Heston. The reader allows 64 KiB total, 64 fields, and 4 KiB per line,
providing ample format headroom without allowing arbitrary CLI input to drive
unbounded allocation. Trace events are fed incrementally into SHA-256 through a
temporary 25-byte encoding, so choosing a late occurrence does not retain the
entire hook history in memory.

`deterministic_scheduler_seed` remains reserved and must be zero. Accepting a nonzero value before a controlled multi-worker scheduler exists would record an input that has no effect and create a false replay claim.

Both injection and recovery execute in supervised subprocesses. The matrix defaults to 30 seconds per phase, descriptor replay defaults to 300 seconds, and the accepted range is one second through one day. A timeout sends `SIGKILL`, reaps the process, stops the matrix, and retains the case. The upper bound prevents accidental effectively-unbounded waits while still permitting deliberately large historical workloads.

The matrix keeps one worker because v2 does not control operating-system completion scheduling, but it no longer equates seed count with structural coverage. It deterministically varies block size/count, checkpoint cadence, queue capacities, partial final blocks, and model inputs. Generated block counts and checkpoint intervals guarantee occurrences one and two are reachable. Runs of at least 32 cases must also pass a topology-diversity gate.

## 13. Measurement edge cases

Wall-clock observations are not deterministic outputs. Enabling metrics adds
monotonic-clock reads and a queue-depth update under locks that already exist,
so it can perturb the scheduling and latency it observes. This is acceptable
only because scenario-keyed RNG, stable block leaves, and fixed-tree reduction
make the numerical result independent of that schedule. Tests compare metrics
on and off bit-for-bit.

Recovered blocks have no computation or result-install latency in the current
process. Encoding that fact as zero keeps samples indexed by stable `block_id`;
percentile summaries must filter zeros rather than treating recovery as an
instantaneous operation. An entirely recovered run therefore emits `null`
latency percentiles, zero queue peaks, and no worker records—not misleading
zero-latency percentiles. If a monotonic clock cannot distinguish the beginning
and end of a real operation, the runtime records one nanosecond to preserve the
zero-sentinel distinction.

Queue peaks are sampled inside the queue's existing mutex and cannot exceed the
resolved capacity. They are high-water marks, not a backlog time series: a peak
of one does not say how long the queue stayed occupied. Per-worker fields are
written only by their owning worker and read after joins; block fields are
written once by the worker assigned that stable block ID.

Queue wait totals include only condition-variable blocking caused by a full or
empty queue. Fast operations contribute zero, and mutex acquisition is outside
this boundary. Worker totals are accumulated in worker-local objects and
published once at exit; scheduler and coordinator totals likewise remain owned
by their threads. This prevents adjacent shared counters from manufacturing the
contention the metrics are intended to diagnose.

Durable file counters name only newly and fully installed artifacts. Stage
timers may include a successful write or sync from an attempt that later fails,
whereas file counts and bytes advance after the full installation protocol.
Consequently, stage time must not be used as evidence that a manifest committed.

The caller owns `RuntimeMetrics`. If it is passed to `DurableRunStore::open`, it
must outlive the store. Reusing an object is supported because each top-level
run resets it. The same object must not be shared by overlapping runs or stores.
Metrics are intentionally absent from hashes and durable records;
however, adding their implementation changes the runtime source digest and thus
the build fingerprint, so an R3 store still requires its R3-tagged binary.
Additive duration and byte totals saturate at `UINT64_MAX`; wrapping to a small
and plausible-looking value would be more dangerous than an explicit ceiling.

Metrics storage is allocated before execution or before a durable directory is
modified. The checkpoint vector is sized from the configured cadence. A direct
store caller may deliberately exceed that cadence; excess latency observations
increment `checkpoint_samples_dropped` while the manifest operation continues.
Metrics exhaustion must never turn a successfully installed manifest into an
exception after the fact.
