# P0.0 baseline and core invariant audit

## Scope and frozen baseline

This audit is PR 0 of the P0 plan. It maps the current invariants and fixes only
correctness defects that were already present; it does not implement the
multi-worker crash campaign, live lease expiry, persistence-degraded retry
semantics, or syscall fault injection.

The pre-change baseline was commit
`f001810078ba14e51f2d5a55a0f80e4f11f4986f` on 2026-08-21. The host was an
Apple M1 MacBook Air running Darwin 23.5.0 arm64 with Apple Clang 15.0.0 and
`-O3 -ffp-contract=off`. The baseline optimized suite passed 46/46 tests and a
fresh Release CTest run passed 14/14 targets.

## Invariant map

### RNG

- Philox4x32-10 is stateless. Its 128-bit counter packs disjoint 40-bit
  scenario, 24-bit time-step, 8-bit dimension, and 24-bit draw-index fields;
  the 64-bit global seed is the Philox key.
- Run validation permits at most `2^40` scenarios and `2^24` time steps, so
  the largest realized zero-based coordinates still fit their fields.
- A variate is a pure function of
  `(global_seed, scenario_id, time_step, dimension, draw_index)`. Worker count,
  assignment order, and completion order do not enter that mapping.
- Antithetic pair `(2j, 2j+1)` consumes the even scenario's stream once and
  applies opposite shocks. Block and run boundaries are required to preserve
  complete pairs.
- GBM uses dimension 0. Heston uses dimensions 0 and 1 and forms correlation
  explicitly. A future draw must select a new explicit dimension or draw index;
  there is no mutable generator position that can shift unrelated streams.

The known-answer counter, raw-word, uniform-endpoint, inverse-normal, golden
block, worker-count, and external PractRand evidence support this invariant.
They do not prove statistical independence for every possible coordinate.

### Aggregation

- Every block owns a Welford accumulator. Antithetic runs accumulate pair
  means, so their observation count is half the scenario count.
- Blocks occupy stable leaves keyed by `block_id`. Final aggregation uses the
  same fixed pairwise tree regardless of completion order.
- The coordinator validates observation count, finiteness, nonnegative `m2`,
  min/mean/max consistency, payload hash, block range, and all identities before
  accepting a leaf.
- Deterministic aggregation and numerical stability are separate claims. The
  fixed tree gives deterministic operation order in a pinned build; Welford and
  pairwise merge improve stability but cannot make every finite mathematical
  result representable in binary64.
- P0.0 now validates merge inputs and the merged result. A binary64 overflow in
  pairwise statistics fails explicitly instead of escaping as `Inf` or `NaN`.
  An unrepresentable normal-approximation confidence interval is reported as
  unavailable rather than emitted as invalid JSON.

### Runtime and concurrency

The current non-reassigning lifecycle is:

```text
missing block
  -> bounded assignment queue
  -> exactly one worker computes a block-local result
  -> bounded completion queue
  -> coordinator validation
  -> accepted fixed leaf
  -> final fixed-tree reduction
```

The durable lifecycle adds two distinct persistence states:

```text
validated result
  -> immutable result file installed and directory-synced (durable orphan)
  -> coordinator accepts the leaf (pending commit)
  -> full manifest names the result and is directory-synced (committed)
```

- The scheduler owns assignment publication, workers own their local kernels
  and metrics, and one coordinator consumes completions. `commit_result` is not
  a concurrent API.
- Both queues are bounded. Closing a queue wakes blocked producers and
  consumers; consumers drain already-published items. Worker, scheduler, and
  coordinator exceptions close both queues, all created threads are joined,
  and the first error is rethrown.
- A missing completion therefore terminates with an error rather than
  returning a partial result. There is currently no live timeout or
  reassignment; that is the P0.2 gap, not an implicit behavior.
- A persistence exception propagates out of the coordinator, closes both
  queues, and stops dispatch. This is bounded fail-fast behavior, but it is not
  the retry/resume `PersistenceDegraded` state required by P0.3.

### Persistence and recovery

- Metadata, result records, and manifests use canonical little-endian binary
  envelopes with storage version, exact length, and CRC32C. Logical RunSpec,
  layout, build, and aggregate identities use SHA-256.
- Installation writes an exclusive temporary file, handles short writes and
  `EINTR`, file-syncs, closes, renames on the same filesystem, and syncs both
  destination and temporary directories.
- `run_spec.bin` is authoritative. Missing metadata in a nonempty store is not
  reconstructed from caller input. The store has a process-exclusive lock.
- Recovery scans manifest generations newest first. It falls back only for an
  invalid artifact; operator limits, capacity failures, permissions, and other
  I/O errors fail the open rather than changing the commit set.
- Every referenced result is re-decoded and checked against canonical block
  range, stochastic/layout/build identity, payload hash, observation count,
  run incarnation, and lease epoch. The manifest aggregate must equal a fresh
  fixed-tree reduction of those records.
- P0.0 additionally rejects a committed result from a future incarnation, a
  lease epoch above the manifest high-water mark, or a current-incarnation
  result whose lease is not exactly the current lease. These conditions were
  previously rejected by normal writers but were not fully enforced by the
  canonical manifest codec.
- Work is committed exactly when a valid, installed manifest names its
  immutable result record. A result file that is not named by that manifest is
  an orphan: recovery excludes and later removes it, then recomputes the block.

### Current crash-point table

| Boundary | Current expected recovery | Automated evidence |
|---|---|---|
| Before/during result temporary write | Ignore/remove temporary artifact; recompute | Process hook before result file `fsync`; truncated/corrupt record tests |
| Result file synced, before rename | Remove temporary artifact; recompute | Process hooks after file `fsync` and before rename |
| Result renamed, before result-directory sync | Result is not committed; recompute unless a later valid manifest names it | Process hook after rename |
| Result directory synced, before manifest names result | Treat result as an orphan; recompute | Protocol and orphan-recovery test; no dedicated post-directory-sync process hook yet |
| During manifest temporary write | Select previous valid generation | Process hook before manifest file `fsync`; corruption fallback |
| Manifest file synced, before rename | Select previous valid generation | Process hooks after file `fsync` and before rename |
| Manifest renamed, before manifest-directory sync | Select the highest valid state observable under the stated POSIX assumptions | Process hook after rename |
| Manifest directory synced, before in-memory update | Select the newly installed valid generation | `manifest.after_install_before_memory` process hook and replay |
| Newest manifest corrupt or references corrupt/missing result | Reject it and try the previous valid generation | Manifest and referenced-result corruption tests |
| No valid compatible manifest remains | Fail recovery clearly | Invalid-store tests |

The process harness does not emulate cached-write loss, torn sectors, device
write caches, or kernel/filesystem bugs. P0.4 adds deterministic syscall-level
faults; physical power-loss claims remain out of scope without a block-device or
VM fault harness.

### Models and numerical boundaries

- GBM uses the exact lognormal step and validates all derived constants and
  every path step. Heston uses pinned full-truncation Euler/log-asset evolution,
  two explicit drivers, and contextual non-finite checks.
- The Heston continuous-time oracle is independent of the simulation kernel,
  uses adaptive normalized Fourier tails, and fails closed when its finite work
  budget cannot establish convergence.
- Feller violations are valid runs with a structured warning. Heston zero-
  variance parameters and correlation endpoints are supported limits.
- Zero maturity and zero GBM volatility remain unsupported RunSpec domains.
  That is a current scope decision to document, not an unstated mathematical
  guarantee; changing it requires explicit model and oracle semantics.

### Observability

- Metrics are caller-owned, preallocated before execution or durable mutation,
  excluded from every persisted identity, and read only after worker joins.
- Per-worker slots and per-block samples have single writers; queue peaks are
  updated under each queue's existing lock. Duration totals saturate rather
  than wrap.
- Clock reads can perturb physical completion order, but metrics do not alter
  RNG coordinates, assignments, validation rules, tree positions, or numerical
  results. The metrics-on/off regression requires bit-identical aggregates.

## Ranked findings and phase gates

| ID | Severity | Finding | Subsystem | Resolution / next phase | Blocks next phase? |
|---|---|---|---|---|---|
| P0.0-1 | P0 | Pairwise merge could overflow otherwise valid finite leaves and produce non-finite final statistics | Aggregation | Fixed fail-closed with regression | No |
| P0.0-2 | P0 | Manifest codec did not itself enforce future-incarnation and lease high-water relationships | Persistence | Fixed in encode/decode semantics with regressions | No |
| P0.0-3 | P1 | A finite aggregate plus an extreme confidence critical value could expose an infinite interval | Statistics/CLI | Both bounds now become unavailable together | No |
| P0.1-1 | P0 evidence | R3 campaign is 1,000 single-worker cases, not 5,000 true multi-worker cases | Failure injection | Implement deterministic multi-worker campaign in P0.1 | Yes, blocks R3 closure |
| P0.2-1 | P0 capability | No live worker timeout, lease expiry, reassignment, or stale-attempt race | Runtime | Implement as isolated runtime semantic change in P0.2 | Yes, blocks R3 closure |
| P0.3-1 | P1 capability | Persistence failure is bounded fail-fast, not retryable `PersistenceDegraded` | Runtime/storage | Define thresholds and state semantics in P0.3 | Yes, blocks original R3 claim |
| P0.4-1 | P0 evidence | No injectable short-write/`EIO`/`ENOSPC`/sync/rename/read fault boundary | Persistence | Add faultable I/O and traceability matrix in P0.4 | Yes, blocks R3 closure |
| P0.4-2 | P1 evidence | No dedicated process hook after result directory sync and before in-memory publication | Persistence | Add or explicitly re-scope in P0.4 | Yes, blocks boundary-complete claim |
| P0.6-1 | P1 evidence | Eight-worker efficiency gate lacks homogeneous eight-core/native x86-64 evidence | Performance | Controlled external-host capture in P0.6 | No for P0.1; yes for R4 closure |
| P0.8-1 | P1 evidence | Checkpoint-loss gate lacks the frozen 20-pair isolated-host capture | Performance | Execute existing harness in P0.8 | No for P0.1; yes for R4 closure |
| P0.0-4 | P2 | Zero maturity and zero GBM volatility are mathematically meaningful but unsupported | Models | Retain as explicit scope unless a later model release adds their semantics | No |
| P0.0-5 | P2 | Eager block materialization caps runs at one million blocks | Resources | Accepted scaling boundary; lazy representation remains later work | No |

R3 is therefore open as an overall original-plan milestone even though its
single-worker application-level process-crash protocol and replay tooling are
implemented and tested. P0.1 may begin after this audit; it must not claim R3
closure because P0.2 through P0.4 remain outstanding.

## P0.0 verification

After the fixes above:

- optimized Makefile suite: 46/46 passed;
- fresh CMake Release: 14/14 CTest targets passed;
- fresh ASan/UBSan with `detect_leaks=0`: 14/14 passed;
- fresh ThreadSanitizer with race failures fatal and intentional crash-child
  thread-leak reports disabled: 14/14 passed;
- Apple Clang static analyzer over every runtime translation unit: no findings;
- compiler warning policy and `git diff --check`: clean.

Leak detection remains disabled because LeakSanitizer is unavailable in this
macOS runtime. TSan disables only thread-leak reports caused by deliberate
crash-child `_exit`; data-race reports remain fatal.
