# R3 deterministic failure injection and replay

R3 validates the R2 application-level crash protocol by terminating real child processes at named persistence boundaries, recovering the resulting run store, and comparing the final fixed-tree aggregate bit for bit with a clean execution under the same pinned build.

## Scope and claim

The harness tests process death, not simulated exceptions. The selected hook first installs and syncs a replay descriptor, then calls `_exit(86)` without running C++ destructors. The operating system releases the run lock, while temporary files, renamed files, and in-memory state remain exactly at the selected application boundary. Tool children are supervised by a monotonic watchdog; a deadlock is classified as a bounded failure and its case directory is retained rather than hanging the complete matrix.

This is not a power-loss or kernel/filesystem fault simulator. In particular, a process crash after rename but before directory `fsync` does not force the host to discard cached directory updates. R3 validates the runtime's recovery decisions for states observable after process death. The R2 durability claim still depends on the documented local-POSIX `write`/`fsync`/`rename`/directory-`fsync` preconditions. Torn writes, device write-cache behavior, filesystem bugs, and machine power loss require a filesystem-level harness such as CrashMonkey or dm-flakey and remain outside this release.

## Stable failure points

R3 replay schema v2 defines nine points. Names and enum values are compatibility constants.

| Failure point | State at termination | Expected recovery |
|---|---|---|
| `result.before_file_fsync` | Complete result bytes exist only in a temporary file | Remove stale temporary file and recompute the block |
| `result.after_file_fsync` | Result temporary file is synced but not renamed | Remove stale temporary file and recompute |
| `result.before_rename` | Result temporary file is synced and closed | Remove stale temporary file and recompute |
| `result.after_rename` | Result name is visible but no manifest names it | Treat as an orphan, remove it after recovery is safe, and recompute |
| `manifest.before_file_fsync` | New full manifest exists only as a temporary file | Ignore it and recover the previous installed manifest |
| `manifest.after_file_fsync` | New manifest temporary file is synced but not renamed | Ignore it and recover the previous manifest |
| `manifest.before_rename` | New manifest is synced and closed | Ignore it and recover the previous manifest |
| `manifest.after_rename` | New manifest name is visible before directory sync | Validate the highest visible manifest; otherwise fall back |
| `manifest.after_install_before_memory` | Manifest file and directories are synced, but the coordinator has not updated its in-memory sequence/commit view | Recover the installed manifest without double contribution |

Every hook uses a selectable positive occurrence. The seeded matrix chooses occurrence one or two and constructs at least three blocks with a checkpoint interval no greater than `block_count - 1`. Therefore a second result and two installed checkpoints are always reachable, including for `manifest.after_install_before_memory`. The post-install hook remains present only in `DurableRunStore::checkpoint`; initial and recovery manifests do not publish a `DurableRunStore` object whose in-memory state can be observed before `open` returns.

## Replay descriptor

Before termination, the child atomically writes and directory-syncs a canonical, ordered text descriptor. Its path must be outside the injected run directory so writing the evidence cannot sync or otherwise mutate the result, manifest, or temporary directories whose boundary is being tested. Replay schema v2 contains:

- descriptor version, RunSpec hash, and pinned build fingerprint;
- every RunSpec field, with binary64 values stored as exact 16-digit bit patterns;
- worker count, block size, queue capacities, and materialization limit;
- checkpoint and durable storage policies;
- failure seed, selected named point, selected occurrence, and reserved scheduler seed;
- SHA-256 hash and length of the observed ordered hook trace;
- run incarnation, block ID, and checkpoint sequence context at the crash;
- a final SHA-256 over every preceding canonical descriptor byte.

The full configuration is retained because a hash alone can verify a RunSpec but cannot reconstruct it. A changed build is rejected by the replay tool before it creates a run store. The record-level SHA-256 detects accidental edits or corruption in fields outside `run_spec_hash`, such as block size, store policy, and failure occurrence; it is an integrity check, not authentication against an attacker who can replace both the record and its checksum. The parser verifies the checksum before interpreting fields and then requires exact canonical re-encoding.

Schema v2 intentionally rejects v1 descriptors. V1 did not protect the complete replay record, so silently accepting it would reintroduce the ambiguity v2 removes. Historical failures must be replayed with their original executable or regenerated under v2.

Replay evidence is immutable. The target path must not exist when injection is configured, and installation uses a synced temporary inode followed by an atomic no-replace hard link, temporary-name removal, and parent-directory `fsync`. A reused path fails closed without modifying the earlier descriptor. Hard-link installation is covered by the same local-POSIX filesystem scope as the durable run store.

Descriptor input is capped at 64 KiB, 64 fields, and 4 KiB per line. A normal v2 descriptor has 38 short fields and is far below these bounds; the limits leave substantial schema headroom while preventing a malformed CLI input from causing unbounded allocation. Hook traces are SHA-256-hashed incrementally in a fixed-size state. Each event is encoded into a temporary 25-byte buffer for one hash update, rather than accumulating approximately 100 bytes per completed block across the four result hooks.

Replay v2 intentionally requires one worker. Assignment order is deterministic, but completion order with multiple live workers is scheduled by the operating system and would make an occurrence-based trace nondeterministic. The normal runtime remains validated across worker counts; only the crash-trace harness is restricted. `deterministic_scheduler_seed` must be zero in v2; nonzero values are rejected until a scheduler capable of replaying controlled multi-worker completion order exists.

## Tools

Build the harnesses:

```sh
make r3-tools
```

Every self-spawning CLI resolves its parent executable to a canonical executable
file before creating a workspace. A bare invocation name is searched using
POSIX `PATH` semantics rather than being incorrectly treated as a path below the
current directory. The crash and replay children therefore re-execute the same
resolved binary whether the parent was launched as `./build/tool`, by absolute
path, or by name through `PATH`.

Run a randomized matrix. The workspace must be empty. Successful case directories are removed; a failing case and its replay descriptor are retained.

```sh
./build/run_crash_matrix \
  --workspace /tmp/mc-r3-matrix \
  --iterations 1000 \
  --first-seed 1 \
  --timeout-seconds 30
```

The seed is mapped through a pinned SplitMix64 transform to one of the nine points and occurrence one or two. A separate deterministic transform varies block size, block count, checkpoint interval, assignment/completion queue capacities, partial final-block size, time-step count, payoff, antithetic mode, and stochastic seed. Replay v2 still uses one worker because completion ordering is not yet controlled, but the persistence topology is no longer fixed. For matrices of at least 32 cases, the tool also enforces a topology-diversity gate. Each case checks:

1. the child exits at the selected hook with code 86;
2. the replay descriptor is valid;
3. recovery accounts for every block as recovered or recomputed exactly once;
4. the recovered aggregate is bitwise equal to the clean fixed-tree aggregate;
5. reopening the complete run computes zero blocks;
6. all nine failure points were covered by the overall matrix;
7. the injection subprocess and combined clean/recovery/completed-reopen validation subprocess stay within the configured watchdog;
8. larger matrices cover multiple block sizes, block counts, checkpoint intervals, queue modes, and a partial final block.

The default watchdog is 30 seconds per matrix phase and may be changed up to one day.

Reproduce a retained failure in a new empty directory:

```sh
./build/replay_failure failure.replay \
  --run-dir /tmp/reproduced-failure \
  --timeout-seconds 300
```

The replay default is 300 seconds because historical descriptors may contain larger workloads. The parent launches a fresh child and accepts reproduction only if the complete descriptor identity—including build, engine/store policy, schedule, ordered trace, and crash context—matches the original. The reproduced run store and immutable observed descriptor are retained for inspection. A timeout or mismatch also retains both paths for diagnosis.

## Acceptance evidence

Optimized, ASan/UBSan, and ThreadSanitizer builds exercise all nine points and pass 32/32 tests. The added tests pin schema-v2 corruption/canonicalization and input bounds, immutable evidence, reserved scheduler-seed rejection, chunked trace hashing, and watchdog termination/reaping. CMake Release/CTest also passes. A fresh 1,000-seed matrix passed exact clean-versus-recovered equality, completed-restart idempotence, and 9/9 point coverage while covering all six block sizes, all eight block counts, all four checkpoint intervals, all four assignment and completion queue modes, and partial final blocks. The descriptor-driven replay tool reproduced a saved `manifest.after_install_before_memory` failure with complete replay-identity equality and trace `b0514886f042433508499861e47e503895241ee8c9c9ba08ee03edf1013acfab`. LeakSanitizer is unavailable on the tested macOS runtime. TSan's thread-leak diagnostic is disabled for crash children because `_exit` intentionally abandons live threads; data-race detection remains enabled and fatal.

R4 can now measure persistence latency and recovery cost without changing the R3 correctness protocol.
