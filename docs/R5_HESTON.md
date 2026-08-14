# R5 phase 1 — deterministic Heston simulation

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

## Verification and remaining R5 work

Phase-1 tests cover:

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
  descriptor and manifest protocol.

The built-in stream smoke test is not a replacement for TestU01 SmallCrush or
PractRand. A release-level external statistical battery and comparison against
an independent analytic/QuantLib Heston price grid remain R5 phase-2 gates.

The phase-1 handoff passes 42/42 optimized unit tests and all 9 Release,
ASan/UBSan, and ThreadSanitizer CTest targets. A 64-seed mixed-model crash
matrix passes all 9 failure hooks and the full topology/model-diversity gate.
