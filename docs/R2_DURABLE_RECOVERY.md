# R2 durable run store and recovery protocol

R2 adds crash-consistent local persistence around the deterministic block engine. Computation remains at least once: a block may be recomputed after a crash. Contribution is at most once: a block enters the official aggregate only when an installed manifest names its immutable result file.

## Storage layout

```text
<run-directory>/
  .run.lock
  run_spec.bin
  manifests/
    manifest_00000000000000000000.bin
    manifest_00000000000000000001.bin
  block_results/
    block_00000000000000000000_inc_00000000000000000001_epoch_00000000000000000001.bin
  tmp/
```

`run_spec.bin` is immutable. It contains the canonical RunSpec, block layout, stochastic run hash, execution-layout hash, build/runtime fingerprint, and derived durable run ID. A run directory cannot be opened with different stochastic parameters, a different block layout, or an incompatible build/runtime identity.

Block-result files are immutable evidence. Their envelopes include the run ID, all three compatibility hashes, canonical block range, run incarnation, lease epoch, RNG/statistics versions, aggregate payload, and aggregate SHA-256 identity.

Manifests are full snapshots. Each contains the complete lease high-water table, sorted committed-block table, fixed-tree aggregate, run status, and optional failure diagnostics. Full snapshots are intentionally simpler than a delta log for the MVP; the resource preflight accounts for their worst-case storage cost.

## Checksums and identities

Every durable file uses a fixed-order, little-endian binary envelope with:

```text
8-byte type magic | storage schema | payload length | payload | CRC32C
```

CRC32C provides a fast accidental-corruption check over the exact envelope bytes. SHA-256 has a different job: it identifies logical RunSpec, layout, build, and aggregate content. Keeping both avoids using a relatively expensive content-identity hash as the only routine file-integrity check.

Neither checksum is authentication. An adversary able to replace a payload and its checksums can forge a self-consistent file. Hostile storage modification is outside this local-runtime threat model.

Golden SHA-256 tests pin the canonical v1 metadata, block-record, and manifest byte formats. A format change therefore requires an explicit storage-schema decision rather than silently invalidating old runs.

## Atomic installation protocol

Each file is installed through the same protocol:

1. Create a unique file under `tmp/` with exclusive creation.
2. Write the complete canonical envelope, retrying interrupted writes.
3. `fsync` the temporary file.
4. Close it.
5. Rename it into the destination directory.
6. `fsync` the destination directory and the source `tmp/` directory.

`tmp/`, `manifests/`, and `block_results/` are verified to reside on the same filesystem. Stale temporary files matching the runtime's private naming scheme are removed during startup and the directory is synced afterward.

The crash-recovery claim assumes a POSIX local filesystem whose `rename` and `fsync` implementations provide the required ordering and durability. NFS, object stores, overlay filesystems, network mounts, and unusual container storage drivers are outside the claim until tested independently.

## Commit and recovery

The durable linearization point is installation of a manifest, not completion of a calculation and not installation of a block-result file.

During normal execution:

1. The coordinator validates a worker result without side effects.
2. The immutable block-result file is installed and synced.
3. The result enters the in-memory pending set.
4. At the checkpoint interval, a full manifest containing all accepted blocks is installed atomically.
5. The final manifest uses status `Complete` and names every block.

If the process stops between steps 2 and 4, the result file is an orphan. Baseline recovery deliberately ignores it and recomputes the block. Counting orphans would create a second commit path and weaken the exactly-once-contribution argument. After selecting and, when necessary, durably installing the recovery snapshot, recovery removes canonical result files not referenced by that snapshot and syncs the result directory; unrelated filenames are never deleted. Cleanup is deliberately after compatibility checks, operator-policy checks, completion preflight, and recovery-manifest installation. A failed open therefore cannot delete previously committed block evidence.

Recovery performs these steps:

1. Acquire the exclusive run-directory coordinator lock.
2. Validate immutable run metadata against the requested specification, layout, build, runtime OS, and C-library identity.
3. Scan numbered manifests from highest to lowest.
4. Reject snapshots with a malformed envelope, bad CRC32C, incompatible identity, invalid lease/block tables, missing or corrupt referenced results, impossible aggregate, or fixed-tree reduction mismatch.
5. Select the highest remaining valid snapshot.
6. For a running snapshot, choose a new incarnation higher than both the prior incarnation and every observed manifest sequence, increment lease epochs, and durably install this recovery snapshot before scheduling work.
7. Restore committed leaves and schedule only the set difference between the canonical block universe and the manifest commit set.

Using the observed manifest-sequence high-water mark prevents an incarnation collision even when the latest manifest is corrupt and recovery falls back to an older snapshot. A completed run is loaded without advancing incarnation or starting workers.

Fallback is deliberately limited to invalid durable artifacts: malformed or semantically inconsistent manifests and missing or corrupt files they reference. Operator limits, insufficient completion budget, permission failures, and other storage I/O errors are fatal for that open and never cause fallback to an older snapshot. Treating a policy limit as corruption could select an older commit set and make valid newer evidence look orphaned.

`run_spec.bin` is the authoritative identity record. If it is missing while any other regular file remains in the run directory (apart from the lock created during open), the runtime refuses to reconstruct it from caller input. This conservative rule prevents an incompatible caller from poisoning an existing store. Recovery requires restoring `run_spec.bin` from the same store or choosing a new empty run directory.

A valid `Failed` manifest is terminal. In particular, a conflicting checksum for an already accepted block persists both checksums and stops the run; it is never degraded to first-result-wins behavior.

Duplicate results and stale incarnation/lease results are expected under at-least-once delivery, retries, and recovery. They are ignored rather than persisted as terminal failures. Corrupt payloads, identity/schema mismatches, invalid blocks or aggregates, and deterministic conflicts remain terminal. Storage schema v1 assigns explicit numeric codes to these validation statuses; serialization does not depend on the C++ enum declaration order.

## Resource and concurrency controls

The run store enforces:

- maximum total regular-file bytes;
- maximum regular-file count;
- minimum free space retained before writes;
- maximum accepted/produced manifest size;
- a completion preflight covering missing block files and worst-case full snapshots;
- the existing one-million materialized-block and 256-worker limits;
- one exclusive coordinator per run directory using a nonblocking advisory lock.

Defaults are 64 GiB total storage, 2,000,100 files, 64 MiB retained free space, and 128 MiB per manifest. Checkpoint cadence is automatic by default: at least 1,024 newly accepted blocks per checkpoint and, for large runs, a cadence chosen to produce no more than 1,024 periodic full snapshots. R4 raised the original 64-block floor after measuring excessive full-manifest write amplification; this increases the default worst-case recomputation window in exchange for removing a material throughput penalty. Automatic runs with at most 1,024 blocks install no periodic manifest between the initial and complete snapshots. An explicit nonzero `--checkpoint-blocks` value overrides the calculation and intentionally accepts the corresponding recovery-granularity/write-cost tradeoff.

Manifest encoding pre-reserves its bounded capacity, CRC32C uses a compile-time lookup table, and checkpoint construction scans the durable result set directly instead of copying a full optional-result snapshot. The format remains a full snapshot, so each checkpoint is still `O(block_count)`; the automatic cadence prevents the default configuration from turning that into an unbounded number of full scans.

These limits fail before work is scheduled when the configured full-snapshot policy cannot complete within its declared budget. They remain active before every individual atomic write as protection against concurrent external disk consumption.

## CLI usage

Durability is enabled by supplying a run directory:

```sh
./build/run_simulation \
  --scenarios 1000000 \
  --workers 8 \
  --block-size 2048 \
  --run-dir ./runs/european-seed-42 \
  --checkpoint-blocks 64 \
  --seed 42
```

Running the same command again resumes or loads the completed result. JSON output adds the durable run ID, manifest sequence, incarnation, resume flag, and block/scenario counts split between recovery and computation in the current process. For a durable invocation, `scenarios_per_second` uses only `computed_scenarios`; loading a completed run therefore reports zero compute throughput instead of presenting recovery speed as simulation speed.

The following options tune safety policy:

```text
--max-storage-bytes
--max-storage-files
--min-free-bytes
--max-manifest-bytes
```

Changing worker count or queue capacities is permitted on recovery because neither changes stochastic or block identity. Changing block size is rejected because it changes the durable block universe.

The build fingerprint includes a digest of runtime source/header content, effective build flags, optimizer/fast-math/FMA policy, compiled CPU feature policy, compiler and standard-library versions, architecture, OS release, and C-library identity. This deliberately makes stores created by behaviorally different builds incompatible, including otherwise similar `-O0` and `-O3` builds. Source or flag changes require a new run directory unless the old executable is retained for recovery.

## R2 acceptance evidence and R3 boundary

Automated tests cover canonical codec round trips and golden bytes, stable persisted failure codes, CRC corruption, orphan exclusion, exact clean-versus-recovered equality, completed-run idempotence, corrupt-manifest fallback, corrupt-referenced-result fallback, persisted determinism failure, benign retry classification, incompatible RunSpec/layout rejection, non-destructive policy failure, missing-metadata fail-closed behavior, storage limits, and exclusion of concurrent coordinators. Optimized and sanitizer evidence is refreshed after every implementation audit; see `IMPLEMENTATION_STATUS.md` for the latest recorded run.

R2 establishes the recovery protocol. R3 now validates it with real child-process termination at each result/manifest `fsync`, rename, and post-install in-memory boundary, plus replayable trace descriptors. Power-loss and filesystem/kernel fault simulation remain outside the application-level R3 claim.
