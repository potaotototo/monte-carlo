# R5 — deterministic Heston simulation and independent validation

R5 phase 1 adds Heston stochastic volatility as a model-specific extension of
the existing deterministic, recoverable runtime. It deliberately leaves the
scheduler, block tree, aggregate schema, and durability protocol unchanged.

## Discretization contract

For each step, dimensions 0 and 1 of the scenario-keyed Philox coordinate
produce independent normals `Z1` and `Z2`. The correlated shocks are

```text
asset shock    = Z1
variance shock = rho * Z1 + sqrt(1 - rho^2) * Z2
```

Heston discretization version 1 uses full-truncation Euler for variance and a
log-Euler asset update:

```text
v_plus = max(v, 0)
log(S_next / S) = (r - 0.5 * v_plus) * dt
                  + sqrt(v_plus) * sqrt(dt) * asset_shock
v_next = v + kappa * (theta - v_plus) * dt
           + xi * sqrt(v_plus) * sqrt(dt) * variance_shock
```

Both updates use the same pre-step `v_plus`. The stored variance state is not
clamped: a negative Euler state remains negative, while the next step uses zero
in drift and diffusion. This is the full-truncation rule, not reflection or
absorption. The exponential asset update preserves mathematical positivity;
numeric underflow, overflow, NaN, or infinity fails the block with scenario and
step context.

The scheme is simple and robust, but it has time-discretization bias. More
advanced schemes such as Andersen's quadratic-exponential method are a future
model/discretization version, never an in-place change to version 1.

## Parameters and validation

The active Heston parameters are:

- initial variance `v0`, nonnegative;
- mean-reversion rate `kappa`, nonnegative;
- long-run variance `theta`, nonnegative;
- volatility of variance `xi`, nonnegative; and
- correlation `rho`, in `[-1, 1]`.

Zero values are accepted because they are useful, mathematically valid limiting
cases. In particular, `xi=0` and `v0=theta` reduce the variance path to a
constant and provide an independent GBM-limit oracle. Correlation endpoints are
accepted; their orthogonal weight is exactly zero.

The Feller ratio is `2*kappa*theta/(xi*xi)`. A ratio below one does not make a
continuous-time Heston model invalid, so validation does not reject it. Instead,
`RunSpec::warnings()` emits structured warning code
`heston_feller_condition_violated`, the observed ratio, and threshold 1. The
CLI includes the warning in JSON and explains the elevated discretization-bias
risk. When `xi=0`, the ratio is mathematically infinite and the condition is
satisfied; JSON represents the non-finite ratio as `null` while retaining the
boolean condition result.

Warnings are derived from the canonical parameters persisted in `run_spec.bin`
rather than duplicated as mutable bytes. `RunMetadata::warnings()` reconstructs
the same structured warning after recovery, avoiding a second field that could
disagree with the authoritative RunSpec.

## Identity and compatibility

RunSpec schema v1 was already tagged by `model_type`. R5 defines its
model-specific tails explicitly:

```text
GBM:    volatility
Heston: discretization_version, v0, kappa, theta, xi, rho
```

The common prefix and GBM tail are byte-for-byte unchanged, so existing GBM
RunSpec hashes, durable metadata, and golden records remain compatible. Heston
does not hash the inactive GBM volatility field. The CLI rejects
`--volatility` with `--model heston` to prevent a user from mistaking that
inactive value for a Heston input.

All active Heston doubles use canonical IEEE-754 binary64 bit patterns. The
discretization version and parameters therefore participate in the RunSpec
hash, durable run ID, build compatibility checks, and replay descriptor. The R3
descriptor remains byte-for-byte unchanged for GBM and uses a tagged Heston
tail for Heston runs.

## Determinism and antithetics

Worker count and completion order cannot change a Heston path. The two random
drivers use fixed dimensions rather than mutable draw order. Antithetic scenario
IDs negate both independent normals before correlation, which negates the
correlated two-driver shock consistently. The runtime aggregates the two
payoffs as one pair-mean observation. Its fused Heston implementation generates
each pair's two normals once and advances both paths together.

## Independent analytic oracle and reference grid

R5 phase 2 adds a dependency-free semi-analytic European-call oracle using the
Little Heston Trap characteristic function. The usual characteristic function
contains `(z-d)/xi^2`, which catastrophically cancels as `xi` tends to zero.
The implementation instead uses the algebraically equivalent Riccati factor
`-A/(z+d)` and integrates its smooth time term with a 64-point rule. There is
therefore no arbitrary small-`xi` cutoff. Exact `xi=0`, time-varying
deterministic variance, and absorbing zero-variance cases use closed-form
limits.

### Challenge found after phase 2: there is no universal Fourier cutoff

The original implementation used 512-point Gauss–Legendre quadrature on the
fixed raw-frequency interval `[0, 200]`. It passed the initial reference grid,
including the long-maturity Kahl–Jäckel case, but failed when expected
integrated variance became small. At short maturity the characteristic
function decays over a raw-frequency scale proportional to
`1/sqrt(E[integral(v dt)])`; the relevant domain therefore moves far beyond
200. Increasing the number of nodes inside `[0, 200]` cannot recover a tail
that was never integrated.

The failure was especially dangerous because the integral over `[0, 200]` was
internally accurate and the CLI labelled the result `available`. Independent
SciPy integrations reproduced the old value when truncated at 200 and
converged to a different value as the domain was expanded:

| maturity | old `[0,200]` price | converged price | understatement |
|---:|---:|---:|---:|
| 0.003 | 0.432048222354 | 0.444468816447 | 2.8% |
| 0.001 | 0.202837923941 | 0.254803539033 | 20.4% |
| 0.0001 | 0.025051624760 | 0.080038149854 | 68.7% |
| 0.00001 | 0.002564704292 | 0.025256315544 | 89.8% |

`R5_HESTON_ADAPTIVE_REFERENCE_GRID.csv` retains the full-precision regression
values. Additional low-variance/non-ATM cases were selected from a deterministic
parameter sweep because that combination adds both a long tail and rapid phase
oscillation.

### Research consulted

The implementation decision was informed by four separate kinds of evidence:

- [Heston's original characteristic-function pricing
  paper](https://doi.org/10.1093/rfs/6.2.327) establishes the semi-analytic
  inversion being evaluated; it does not supply one universal finite cutoff.
- Lord and Kahl's [Optimal Fourier Inversion in Semi-analytical Option
  Pricing](https://repub.eur.nl/pub/7915) treats contour selection, cancellation,
  characteristic-function explosion, strikes, and maturities as numerical
  concerns rather than assuming one inversion setup works uniformly.
- QuantLib's pinned
  [`AnalyticHestonEngine`](https://github.com/lballabio/QuantLib/blob/2a13d455048c748b9fb5c2ff3bb4f4cf56dfb9a9/ql/pricingengines/vanilla/analytichestonengine.hpp)
  provides adaptive integration choices and a model-dependent
  Andersen–Piterbarg integration limit. Its
  [official Heston tests](https://github.com/lballabio/QuantLib/blob/2a13d455048c748b9fb5c2ff3bb4f4cf56dfb9a9/test-suite/hestonmodel.cpp)
  remain our independent reference for the Kahl–Jäckel stress case.
- Cutoff sweeps in an independent SciPy implementation isolated this specific
  defect: the old C++ result agreed with SciPy at cutoff 200, while expanded
  domains converged to the new references. That distinguishes domain truncation
  from characteristic-function algebra or insufficient nodes on the retained
  interval.

The papers and QuantLib establish why inversion domain, contour, and adaptive
integration deserve explicit treatment. The exact normalization and stopping
policy below are this project's engineering decision, validated against the
independent cutoff sweeps; they are not presented as a theorem copied from
those sources.

### Decision: normalized, convergence-controlled expansion

The fixed raw cutoff is replaced by the dimensionless coordinate

```text
x = phi * sqrt(E[integral_0^T v(t) dt])
E[integral_0^T v(t) dt]
  = theta*T + (v0-theta)*(1-exp(-kappa*T))/kappa
```

with the continuous `kappa=0` limit `v0*T`. This scaling automatically widens
the raw-frequency domain at short maturity or low variance. It is a scale
choice, not by itself a convergence claim.

Computing that expectation also has a small-`kappa*T` edge. The direct formula
subtracts two nearly equal `theta*T` terms and can erase the long-run-variance
contribution completely. The implementation evaluates the initial-variance and
long-run-variance weights as a paired series below `kappa*T=1e-4`; a regression
at `kappa*T=1e-16` pins the nonzero limit. This is relevant both to the Fourier
scale and to the exact `xi=0` deterministic-variance branch.

The outer inversion now uses an embedded 15-point Kronrod/7-point Gauss error
estimate. It integrates normalized `[0,8]`, then doubles successive tail slabs.
To avoid hiding oscillation inside a large slab, log-forward-moneyness splits
each slab so the explicit strike phase advances by at most `pi` per segment.
The result is accepted only after three consecutive tail slabs are quiet and
the accumulated quadrature-error estimate stays inside its allocated price
error budget.

The declared option-price target is
`1e-9 + 1e-11*max(1, spot, discounted_strike)`. Each probability receives half
that budget. A tail slab is quiet only when both its magnitude and its local
error estimate are at most one thirty-second of the probability-integral
budget. Adaptive depth is limited to 20, expansion to 20 tail slabs, and each
probability to 500,000 integrand evaluations. These are safety limits, not
alternative cutoffs: exhaustion throws, and the CLI reports
`unavailable_numerical_domain`.

Three quiet slabs are a conservative operational convergence test, not a
rigorous analytic upper bound on every remaining Heston tail. The retained
independent regression grid is therefore part of the decision. Extremely deep
ITM/OTM requests can still exhaust the finite budget; publishing no oracle is
preferable to silently treating the last partial integral as converged.

Alternatives considered were (1) merely raising 200, which adds cost without
removing the same class of failure, (2) choosing only a parameter-derived
cutoff, which still lacks an observed convergence check, and (3) a fixed
semi-infinite quadrature rule, whose scaling and oscillation remain
parameter-dependent. The selected design combines parameter scaling,
error-estimating local quadrature, observed tail convergence, and a fail-closed
work cap while preserving the stable characteristic-function algebra.

The production path kernel does not call or share discretization code with this
oracle. The CLI reports `heston_analytic_price` and signed `analytic_error` for
European Heston calls; Asian calls correctly omit an inapplicable oracle.

`R5_HESTON_REFERENCE_GRID.csv` records five prices independently evaluated with
SciPy quadrature and QuantLib 1.43's `AnalyticHestonEngine` at order 192. Four
cases span Feller-safe/violating, ITM/ATM/OTM, and positive/negative correlation
regimes. The fifth is QuantLib's Kahl–Jäckel long-maturity, deep-OTM stress case,
whose published test tolerance targets `4.95212`. QuantLib source provenance is
pinned to commit
[`2a13d455`](https://github.com/lballabio/QuantLib/blob/2a13d455048c748b9fb5c2ff3bb4f4cf56dfb9a9/test-suite/hestonmodel.cpp).
The in-tree oracle must match every original full-precision reference within
`5e-10`. The adaptive edge grid uses declared case-specific tolerances no looser
than `2e-8`, and the observed differences are materially smaller.

## External RNG stream adapter

`export_rng_stream` exposes the exact raw 64-bit value made from Philox output
words 0 and 1 before RNG v2 retains its high 53 bits. It does not invent a
second generator or flatten mutable state. Consecutive output words advance
scenario ID, with an optional contiguous dimension range interleaved within
each scenario; time step and draw index are explicit fixed coordinates.

Binary output is explicitly little-endian `uint64`, independent of host byte
order. Hex output is for known-answer review and CI. All coordinate ranges are
checked before any output, including the final scenario, so a request cannot
wrap into a reused Philox counter. Example external runs:

```sh
# Each Heston driver separately.
./build/export_rng_stream --seed 1 --dimension-start 0 --dimensions 1 \
  --words 134217728 | RNG_test stdin64 -tf 2 -te 1 -tlmax 1GB -tlmaxonly
./build/export_rng_stream --seed 1 --dimension-start 1 --dimensions 1 \
  --words 134217728 | RNG_test stdin64 -tf 2 -te 1 -tlmax 1GB -tlmaxonly

# The exact two-driver interleaving consumed across Heston scenarios.
./build/export_rng_stream --seed 1 --dimension-start 0 --dimensions 2 \
  --words 134217728 | RNG_test stdin64 -tf 2 -te 1 -tlmax 1GB -tlmaxonly
```

PractRand is an external release tool, not a runtime dependency. The adapter's
golden hex output and range failures run in ordinary CI. The pinned 1 GiB
PractRand 0.96 battery, exploratory anomalies, predeclared confirmation rule,
and complete checkpoint summaries are recorded in
`R5_PRACTRAND_0_96_RESULTS.md`; absence of a statistical failure is evidence,
not a proof of generator quality.

Exposing `random_u64` and routing `uniform_open01` through it does not change
RNG semantics or increment `rng_version`: the same Philox words in the same
order feed the unchanged 53-bit conversion, as pinned by raw-word, uniform, and
golden block tests. It is an observability adapter, not a new RNG mapping.

## Verification and remaining R5 work

Tests cover:

- tagged RunSpec validation, identity sensitivity, and durable metadata round
  trips;
- Feller warnings, including the `xi=0` limiting case;
- a deterministic moment/variance/cross-dimension-correlation smoke test for
  the two realized Philox streams;
- pathwise agreement with the constant-variance GBM limit;
- exact fixed-tree results across 1, 2, 4, and 8 workers;
- fused and unfused antithetic estimator equality;
- durable completion and zero-work recovery; and
- mixed GBM/Heston seeded process-crash recovery through the existing R3
  descriptor and manifest protocol;
- five full-precision QuantLib/SciPy analytic prices, including the official
  Kahl–Jäckel stress case;
- five short-maturity and three low-variance/moneyness cutoff regressions, plus
  fail-closed extreme-strike budget exhaustion;
- exact zero-`xi`, tiny positive-`xi`, and absorbing-zero analytic limits; and
- raw-stream golden output plus adapter coordinate/range validation.

The built-in stream smoke test is not a replacement for an external battery.
The independent price-grid gate, external adapter, and predeclared 1 GiB
PractRand 0.96 confirmation are complete. A second suite such as TestU01 and
longer streams remain optional strengthening work rather than silently claimed
evidence.

The phase-1 handoff passes 42/42 optimized unit tests and all 9 Release,
ASan/UBSan, and ThreadSanitizer CTest targets. A 64-seed mixed-model crash
matrix passes all 9 failure hooks and the full topology/model-diversity gate.

The phase-2 handoff originally passed 44/44 optimized unit tests and all 12 Release,
ASan/UBSan, and ThreadSanitizer CTest targets. Both CLI JSON shapes validate,
compiler warnings are clean, and all validator/build directories created for
the phase are removed after evidence capture.

The adaptive-cutoff follow-up passes 45/45 optimized unit tests. Release,
ASan/UBSan, and ThreadSanitizer builds each pass all 12 CTest targets; the
complete follow-up evidence is also recorded in `IMPLEMENTATION_STATUS.md`.
