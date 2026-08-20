# R5 PractRand 0.96 external battery

## Scope and provenance

The external validator was official
[PractRand 0.96](https://sourceforge.net/projects/pracrand/files/PractRand_0.96.zip)
(`PractRand_0.96.zip`, SHA-256
`e4caf7fda98b2c597bbda3b576753cf5a0f6047aab837c82be370ab798a672e1`).
It was built on macOS 14.5 arm64 with Apple clang 15.0.0. The upstream archive
unconditionally included x86 intrinsics under Clang and compiled both sides of
a constant `BITS_PER_BLOCK` branch. The temporary validator build only:

- guarded the x86 includes/`rdtsc` path by x86 architecture and used the
  existing chrono fallback on ARM64; and
- changed that constant branch to C++17 `if constexpr` so its unreachable
  `1 << 64` expression was not instantiated.

No PractRand statistical algorithm, threshold, calibration, or project source
was changed. The validator build directory was removed after capturing these
results.

All runs used raw little-endian words from `export_rng_stream`, `stdin64`, the
expanded test set, extra folding, and a 1 GiB maximum:

```sh
export_rng_stream [coordinates] --words 134217728 \
  | RNG_test stdin64 -tf 2 -te 1 -tlmax 1GB -tlmaxonly
```

Each 1 GiB stream therefore contained exactly 134,217,728 raw 64-bit words.
The coordinates advanced scenario ID; individual streams fixed dimension 0 or
1, while the interleaved stream emitted dimensions 0 then 1 for each scenario.

## Exploratory runs

The exploratory seed-1 runs were deliberately not promoted into a post-hoc
pass. Dimension 0 had no anomaly at 32, 64, 128, 256, 512 MiB, or 1 GiB. At
32 MiB, dimension 1 reported:

```text
[Low1/64]mod3n(0):(8,9-6)  R=+19.7  p=1.4e-7  mildly suspicious
```

It was absent at every later checkpoint through 1 GiB. The seed-1 interleaved
run had no anomaly through 512 MiB and reported at 1 GiB:

```text
[Low1/8][C8]DC6-5x4Bytes-1  R=+8.1  p=5.7e-5  unusual
```

Seed-2 replications of dimension 1 and the interleaved stream had no anomaly at
any checkpoint through 1 GiB. PractRand documents an average false-positive
rate of 0.1 `unusual` evaluations per results summary. Neither exploratory
event persisted or repeated, but acceptance was still evaluated on a newly
declared confirmation set.

## Predeclared confirmation and acceptance

Before confirmation, the fixed key was declared as decimal `3517179152`
(`0xD1A3E510`) and the three stream shapes and 1 GiB length were frozen. A run
would hard-fail on any `suspicious` or worse evaluation. A `mildly suspicious`
result would require an independent replication and could neither persist into
the next checkpoint nor repeat. An `unusual` result alone would be recorded but
would not fail, following PractRand's documented expected rate.

All three confirmation streams had no anomalies at every checkpoint that
PractRand emitted:

| Stream | Checkpoints and result counts | Transcript SHA-256 |
|---|---|---|
| dimension 0 | 32 MiB: 1529; 64: 1642; 128: 1747; 256: 1845; 512: 1948; 1 GiB: 2055 | `7f8089132af4ab2a754ac581984709de459878f2ce915603d500b0786dea7b83` |
| dimension 1 | 64 MiB: 1649; 128: 1747; 256: 1845; 512: 1949; 1 GiB: 2056 | `01cf3dd2ab84ad59b9460aee1bbe9206cc5c50ac0596768bb3ad01931d2ad66c` |
| dimensions 0/1 interleaved | 64 MiB: 1641; 128: 1743; 256: 1845; 512: 1949; 1 GiB: 2054 | `bfe5f069c1fbec7d0c163074422ccdc6a3e76ccd02306651e15259a51b012644` |

The 32 MiB report is absent from two transcripts because PractRand reached its
next reporting threshold before printing; this is normal reporting behavior,
not omitted output. Every printed confirmation summary said `no anomalies`.

Result: the declared 1 GiB PractRand 0.96 R5 gate passes for both individual
Heston drivers and their scenario-wise interleaving. This finite battery cannot
prove randomness; longer streams, more keys, and a separate TestU01 battery can
strengthen future release evidence without changing the current deterministic
contract.
