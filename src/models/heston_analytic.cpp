#include "mc/model.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <limits>
#include <stdexcept>

namespace mc {
namespace {

using Real = long double;
using Complex = std::complex<Real>;

constexpr std::size_t kTimeQuadratureOrder = 64U;
// The Fourier coordinate is x=phi*sqrt(E[integral(v dt)]), so 8 is a
// dimensionless starting domain rather than a universal raw-frequency cutoff.
constexpr Real kInitialNormalizedCutoff = 8.0L;
constexpr std::size_t kRequiredQuietTailSlabs = 3U;
constexpr std::size_t kMaximumTailSlabs = 20U;
constexpr std::size_t kMaximumAdaptiveDepth = 20U;
// This oracle is diagnostic. Bound pathological strike/parameter requests so
// they become "unavailable" instead of monopolizing a simulation invocation.
constexpr std::size_t kMaximumIntegrandEvaluations = 500'000U;
// The error target is in option-price units. It is deliberately looser than
// binary64 rounding noise but much tighter than Monte Carlo sampling error.
constexpr Real kAbsolutePriceTolerance = 1.0e-9L;
constexpr Real kRelativePriceTolerance = 1.0e-11L;

template <std::size_t Order>
struct QuadratureRule {
    std::array<Real, Order> nodes{};
    std::array<Real, Order> weights{};
};

template <std::size_t Order>
QuadratureRule<Order> make_gauss_legendre_rule() {
    QuadratureRule<Order> rule;
    constexpr std::size_t half = (Order + 1U) / 2U;
    const Real pi = std::acos(-1.0L);
    const Real tolerance = 64.0L * std::numeric_limits<Real>::epsilon();
    for (std::size_t index = 0U; index < half; ++index) {
        Real root = std::cos(pi *
            (static_cast<Real>(index) + 0.75L) /
            (static_cast<Real>(Order) + 0.5L));
        Real derivative = 0.0L;
        bool converged = false;
        for (std::size_t iteration = 0U; iteration < 100U; ++iteration) {
            Real previous_previous = 1.0L;
            Real previous = root;
            for (std::size_t degree = 2U; degree <= Order;
                 ++degree) {
                const Real current =
                    ((2.0L * static_cast<Real>(degree) - 1.0L) * root * previous -
                     (static_cast<Real>(degree) - 1.0L) * previous_previous) /
                    static_cast<Real>(degree);
                previous_previous = previous;
                previous = current;
            }
            derivative = static_cast<Real>(Order) *
                (root * previous - previous_previous) /
                (root * root - 1.0L);
            const Real next = root - previous / derivative;
            if (std::abs(next - root) <= tolerance) {
                root = next;
                converged = true;
                break;
            }
            root = next;
        }
        if (!converged || !std::isfinite(root) || !std::isfinite(derivative)) {
            throw std::runtime_error(
                "Heston analytic quadrature construction did not converge");
        }
        // Recompute P'_n at the converged root before deriving the weight.
        Real previous_previous = 1.0L;
        Real previous = root;
        for (std::size_t degree = 2U; degree <= Order; ++degree) {
            const Real current =
                ((2.0L * static_cast<Real>(degree) - 1.0L) * root * previous -
                 (static_cast<Real>(degree) - 1.0L) * previous_previous) /
                static_cast<Real>(degree);
            previous_previous = previous;
            previous = current;
        }
        derivative = static_cast<Real>(Order) *
            (root * previous - previous_previous) / (root * root - 1.0L);
        const Real weight =
            2.0L / ((1.0L - root * root) * derivative * derivative);
        rule.nodes[index] = -root;
        rule.nodes[Order - 1U - index] = root;
        rule.weights[index] = weight;
        rule.weights[Order - 1U - index] = weight;
    }
    return rule;
}

const QuadratureRule<kTimeQuadratureOrder>& time_quadrature_rule() {
    static const auto rule =
        make_gauss_legendre_rule<kTimeQuadratureOrder>();
    return rule;
}

Real normal_cdf(Real value) {
    return 0.5L * std::erfc(-value / std::sqrt(2.0L));
}

Real expected_integrated_variance(Real initial_variance,
                                  Real mean_reversion_rate,
                                  Real long_run_variance,
                                  Real maturity) {
    if (mean_reversion_rate == 0.0L) {
        return initial_variance * maturity;
    }
    const Real scaled_rate = mean_reversion_rate * maturity;
    Real initial_weight = 0.0L;
    Real long_run_weight = 0.0L;
    if (scaled_rate < 1.0e-4L) {
        // Evaluate (1-exp(-x))/x and its complement together. Subtracting
        // either from one would erase the O(x) long-run contribution here.
        const Real squared = scaled_rate * scaled_rate;
        const Real cubed = squared * scaled_rate;
        const Real fourth = cubed * scaled_rate;
        long_run_weight = 0.5L * scaled_rate - squared / 6.0L +
                          cubed / 24.0L - fourth / 120.0L;
        initial_weight = 1.0L - long_run_weight;
    } else {
        initial_weight = -std::expm1(-scaled_rate) / scaled_rate;
        long_run_weight = 1.0L - initial_weight;
    }
    return maturity * (initial_variance * initial_weight +
                       long_run_variance * long_run_weight);
}

Real deterministic_variance_call(Real spot,
                                 Real strike,
                                 Real rate,
                                 Real maturity,
                                 Real integrated_variance) {
    const Real discount = std::exp(-rate * maturity);
    if (!std::isfinite(discount) || discount <= 0.0L ||
        !std::isfinite(integrated_variance) || integrated_variance < 0.0L) {
        throw std::domain_error(
            "Heston analytic deterministic limit is numerically invalid");
    }
    if (integrated_variance == 0.0L) {
        return std::max(spot - strike * discount, 0.0L);
    }
    const Real scaled_volatility = std::sqrt(integrated_variance);
    const Real d1 = (std::log(spot / strike) + rate * maturity +
                     0.5L * integrated_variance) /
                    scaled_volatility;
    const Real d2 = d1 - scaled_volatility;
    return spot * normal_cdf(d1) - strike * discount * normal_cdf(d2);
}

struct IntegrationBudget {
    std::size_t evaluations = 0U;
};

struct IntegralEstimate {
    Real value = 0.0L;
    Real error = 0.0L;
};

template <typename Integrand>
IntegralEstimate gauss_kronrod_15(const Integrand& integrand,
                                  Real lower,
                                  Real upper,
                                  IntegrationBudget& budget) {
    // Kronrod's 15-point extension of the embedded 7-point Gauss rule.
    static constexpr std::array<Real, 8> nodes = {
        0.991455371120812639206854697526329L,
        0.949107912342758524526189684047851L,
        0.864864423359769072789712788640926L,
        0.741531185599394439863864773280788L,
        0.586087235467691130294144838258730L,
        0.405845151377397166906606412076961L,
        0.207784955007898467600689403773245L,
        0.0L,
    };
    static constexpr std::array<Real, 8> kronrod_weights = {
        0.022935322010529224963732008058970L,
        0.063092092629978553290700663189204L,
        0.104790010322250183839876322541518L,
        0.140653259715525918745189590510238L,
        0.169004726639267902826583426598550L,
        0.190350578064785409913256402421014L,
        0.204432940075298892414161999234649L,
        0.209482141084727828012999174891714L,
    };
    static constexpr std::array<Real, 4> gauss_weights = {
        0.129484966168869693270611432679082L,
        0.279705391489276667901467771423780L,
        0.381830050505118944950369775488975L,
        0.417959183673469387755102040816327L,
    };
    if (budget.evaluations > kMaximumIntegrandEvaluations - 15U) {
        throw std::domain_error(
            "Heston analytic integration exceeded its evaluation budget");
    }
    budget.evaluations += 15U;
    const Real midpoint = 0.5L * (lower + upper);
    const Real half_width = 0.5L * (upper - lower);
    const Real center = integrand(midpoint);
    if (!std::isfinite(center)) {
        throw std::domain_error(
            "Heston analytic quadrature sample became non-finite");
    }
    Real kronrod = kronrod_weights[7] * center;
    Real gauss = gauss_weights[3] * center;
    for (std::size_t index = 0U; index < 7U; ++index) {
        const Real offset = half_width * nodes[index];
        const Real pair = integrand(midpoint - offset) +
                          integrand(midpoint + offset);
        if (!std::isfinite(pair)) {
            throw std::domain_error(
                "Heston analytic quadrature pair became non-finite");
        }
        kronrod += kronrod_weights[index] * pair;
        if (index % 2U == 1U) {
            gauss += gauss_weights[(index - 1U) / 2U] * pair;
        }
    }
    const Real value = kronrod * half_width;
    const Real gauss_value = gauss * half_width;
    if (!std::isfinite(value) || !std::isfinite(gauss_value)) {
        throw std::domain_error(
            "Heston analytic quadrature estimate became non-finite");
    }
    const Real rounding_floor = 50.0L *
        std::numeric_limits<Real>::epsilon() * std::abs(value);
    return {value, std::max(std::abs(value - gauss_value), rounding_floor)};
}

template <typename Integrand>
IntegralEstimate integrate_adaptive(const Integrand& integrand,
                                    Real lower,
                                    Real upper,
                                    Real tolerance,
                                    std::size_t depth,
                                    IntegrationBudget& budget) {
    const IntegralEstimate whole =
        gauss_kronrod_15(integrand, lower, upper, budget);
    if (whole.error <= tolerance) {
        return whole;
    }
    if (depth == 0U) {
        // At machine-scale intervals, further bisection can demand a local
        // tolerance below representable rounding noise. Preserve the error
        // estimate; the caller's global error/tail gate still fails closed.
        return whole;
    }
    const Real midpoint = 0.5L * (lower + upper);
    const IntegralEstimate left = integrate_adaptive(
        integrand, lower, midpoint, 0.5L * tolerance, depth - 1U, budget);
    const IntegralEstimate right = integrate_adaptive(
        integrand, midpoint, upper, 0.5L * tolerance, depth - 1U, budget);
    return {left.value + right.value, left.error + right.error};
}

template <typename Integrand>
IntegralEstimate integrate_phase_safe_slab(const Integrand& integrand,
                                           Real lower,
                                           Real upper,
                                           Real phase_rate,
                                           Real tolerance,
                                           IntegrationBudget& budget) {
    const Real pi = std::acos(-1.0L);
    const Real phase_span = (upper - lower) * phase_rate;
    const Real raw_segments = phase_span > pi ? std::ceil(phase_span / pi)
                                               : 1.0L;
    if (!std::isfinite(raw_segments) || raw_segments < 1.0L ||
        raw_segments > static_cast<Real>(kMaximumIntegrandEvaluations / 15U)) {
        throw std::domain_error(
            "Heston analytic oscillation exceeds its evaluation budget");
    }
    const std::size_t segments = static_cast<std::size_t>(raw_segments);
    const Real width = (upper - lower) / static_cast<Real>(segments);
    IntegralEstimate total;
    for (std::size_t segment = 0U; segment < segments; ++segment) {
        const Real segment_lower =
            lower + static_cast<Real>(segment) * width;
        const Real segment_upper = segment + 1U == segments
            ? upper
            : segment_lower + width;
        const IntegralEstimate estimate = integrate_adaptive(
            integrand, segment_lower, segment_upper,
            tolerance / static_cast<Real>(segments),
            kMaximumAdaptiveDepth, budget);
        total.value += estimate.value;
        total.error += estimate.error;
    }
    return total;
}

Real fourier_integrand(Real phi,
                       Real log_spot,
                       Real log_strike,
                       Real rate,
                       Real maturity,
                       Real initial_variance,
                       Real mean_reversion_rate,
                       Real long_run_variance,
                       Real volatility_of_variance,
                       Real correlation,
                       bool asset_probability) {
    const QuadratureRule<kTimeQuadratureOrder>& time_rule =
        time_quadrature_rule();
    const Complex imaginary{0.0L, 1.0L};
    const Real xi_squared = volatility_of_variance * volatility_of_variance;
    const Real u = asset_probability ? 0.5L : -0.5L;
    const Real b = asset_probability
                       ? mean_reversion_rate -
                             correlation * volatility_of_variance
                       : mean_reversion_rate;
    const Complex z = b - correlation * volatility_of_variance *
                              imaginary * phi;
    Complex d = std::sqrt(z * z + xi_squared *
        (phi * phi - 2.0L * u * imaginary * phi));
    // Pin the square-root branch used by the Little Heston Trap.
    if (d.real() < 0.0L) {
        d = -d;
    }
    const Complex a = phi * phi - 2.0L * u * imaginary * phi;
    const Complex z_plus_d = z + d;
    // The usual expressions (z-d)/xi^2 and (z-d)/(z+d)
    // catastrophically cancel as xi approaches zero. These identities
    // are algebraically equivalent because d^2=z^2+xi^2*A.
    const Complex d_prefactor = -a / z_plus_d;
    const Complex g = -xi_squared * a / (z_plus_d * z_plus_d);
    const Complex exp_minus_dt = std::exp(-d * maturity);
    const Complex capital_d = d_prefactor *
        (1.0L - exp_minus_dt) / (1.0L - g * exp_minus_dt);
    // C(T)=r*i*phi*T+kappa*theta*integral_0^T D(t)dt. Integrating
    // this stable D form avoids the remaining log/xi^2 cancellation.
    Complex integrated_d{0.0L, 0.0L};
    if (mean_reversion_rate * long_run_variance != 0.0L) {
        for (std::size_t time_index = 0U;
             time_index < kTimeQuadratureOrder; ++time_index) {
            const Real tau = 0.5L * maturity *
                (time_rule.nodes[time_index] + 1.0L);
            const Complex exp_minus_d_tau = std::exp(-d * tau);
            integrated_d += time_rule.weights[time_index] * d_prefactor *
                (1.0L - exp_minus_d_tau) /
                (1.0L - g * exp_minus_d_tau);
        }
        integrated_d *= 0.5L * maturity;
    }
    const Complex c = rate * imaginary * phi * maturity +
        mean_reversion_rate * long_run_variance * integrated_d;
    const Complex characteristic =
        std::exp(c + capital_d * initial_variance +
                 imaginary * phi * log_spot);
    const Real integrand = std::real(
        std::exp(-imaginary * phi * log_strike) * characteristic /
        (imaginary * phi));
    if (!std::isfinite(integrand)) {
        throw std::domain_error(
            "Heston analytic integrand became non-finite");
    }
    return integrand;
}

Real adaptive_probability(Real log_spot,
                          Real log_strike,
                          Real rate,
                          Real maturity,
                          Real initial_variance,
                          Real mean_reversion_rate,
                          Real long_run_variance,
                          Real volatility_of_variance,
                          Real correlation,
                          bool asset_probability,
                          Real frequency_scale,
                          Real price_weight,
                          Real price_tolerance) {
    const Real pi = std::acos(-1.0L);
    const Real integral_tolerance =
        0.5L * price_tolerance * pi / price_weight;
    const Real slab_tolerance = integral_tolerance / 32.0L;
    const Real quiet_tail_tolerance = integral_tolerance / 32.0L;
    const Real phase_rate =
        std::abs(log_spot - log_strike + rate * maturity) /
        frequency_scale;
    IntegrationBudget budget;
    const auto normalized_integrand = [&](Real normalized_frequency) {
        const Real phi = normalized_frequency / frequency_scale;
        if (!std::isfinite(phi) || phi <= 0.0L) {
            throw std::domain_error(
                "Heston analytic frequency scaling overflowed");
        }
        return fourier_integrand(
                   phi, log_spot, log_strike, rate, maturity,
                   initial_variance, mean_reversion_rate, long_run_variance,
                   volatility_of_variance, correlation, asset_probability) /
               frequency_scale;
    };

    IntegralEstimate total = integrate_phase_safe_slab(
        normalized_integrand, 0.0L, kInitialNormalizedCutoff, phase_rate,
        slab_tolerance, budget);
    std::size_t quiet_slabs = 0U;
    Real lower = kInitialNormalizedCutoff;
    for (std::size_t slab = 0U; slab < kMaximumTailSlabs; ++slab) {
        const Real upper = 2.0L * lower;
        const IntegralEstimate tail = integrate_phase_safe_slab(
            normalized_integrand, lower, upper, phase_rate,
            slab_tolerance, budget);
        total.value += tail.value;
        total.error += tail.error;
        if (std::abs(tail.value) <= quiet_tail_tolerance &&
            tail.error <= slab_tolerance) {
            ++quiet_slabs;
        } else {
            quiet_slabs = 0U;
        }
        if (quiet_slabs >= kRequiredQuietTailSlabs &&
            total.error <= 0.75L * integral_tolerance) {
            return 0.5L + total.value / pi;
        }
        lower = upper;
    }
    throw std::domain_error(
        "Heston analytic Fourier tail did not converge within its budget");
}

}  // namespace

double heston_european_call_price(double spot,
                                  double strike,
                                  double rate,
                                  double maturity,
                                  const HestonParams& parameters) {
    if (!std::isfinite(spot) || !std::isfinite(strike) ||
        !std::isfinite(rate) || !std::isfinite(maturity) ||
        !std::isfinite(parameters.initial_variance) ||
        !std::isfinite(parameters.mean_reversion_rate) ||
        !std::isfinite(parameters.long_run_variance) ||
        !std::isfinite(parameters.volatility_of_variance) ||
        !std::isfinite(parameters.correlation) ||
        !(spot > 0.0 && strike > 0.0 && maturity > 0.0) ||
        parameters.initial_variance < 0.0 ||
        parameters.mean_reversion_rate < 0.0 ||
        parameters.long_run_variance < 0.0 ||
        parameters.volatility_of_variance < 0.0 ||
        parameters.correlation < -1.0 || parameters.correlation > 1.0) {
        throw std::invalid_argument(
            "Heston analytic inputs must be finite and satisfy model bounds");
    }

    const Real s = static_cast<Real>(spot);
    const Real k = static_cast<Real>(strike);
    const Real r = static_cast<Real>(rate);
    const Real t = static_cast<Real>(maturity);
    const Real v0 = static_cast<Real>(parameters.initial_variance);
    const Real kappa = static_cast<Real>(parameters.mean_reversion_rate);
    const Real theta = static_cast<Real>(parameters.long_run_variance);
    const Real xi = static_cast<Real>(parameters.volatility_of_variance);
    const Real rho = static_cast<Real>(parameters.correlation);

    Real price = 0.0L;
    const Real integrated_variance =
        expected_integrated_variance(v0, kappa, theta, t);
    const Real discount = std::exp(-r * t);
    const Real discounted_strike = k * discount;
    if (!std::isfinite(discount) || discount <= 0.0L ||
        !std::isfinite(discounted_strike) || discounted_strike <= 0.0L) {
        throw std::domain_error(
            "Heston analytic discounted strike is numerically invalid");
    }
    if (xi == 0.0L || (v0 == 0.0L && kappa * theta == 0.0L)) {
        price = deterministic_variance_call(s, k, r, t,
                                            integrated_variance);
    } else {
        if (!std::isfinite(integrated_variance) ||
            integrated_variance <= 0.0L) {
            throw std::domain_error(
                "Heston analytic expected integrated variance is invalid");
        }
        const Real price_scale = std::max({1.0L, s, discounted_strike});
        const Real price_tolerance = kAbsolutePriceTolerance +
            kRelativePriceTolerance * price_scale;
        const Real frequency_scale = std::sqrt(integrated_variance);
        const Real p1 = adaptive_probability(
            std::log(s), std::log(k), r, t, v0, kappa, theta, xi, rho,
            true, frequency_scale, s, price_tolerance);
        const Real p2 = adaptive_probability(
            std::log(s), std::log(k), r, t, v0, kappa, theta, xi, rho,
            false, frequency_scale, discounted_strike, price_tolerance);
        price = s * p1 - discounted_strike * p2;
    }
    const double result = static_cast<double>(price);
    if (!std::isfinite(result)) {
        throw std::domain_error(
            "Heston analytic calculation produced a non-finite price");
    }
    // Numerical quadrature can miss an arbitrage bound by a few ulps. Preserve
    // the computed value unless it is only round-off outside the hard bounds.
    const double lower = static_cast<double>(
        std::max(s - discounted_strike, 0.0L));
    const double scale = static_cast<double>(
        std::max({1.0L, s, discounted_strike}));
    const double tolerance = 128.0 * std::numeric_limits<double>::epsilon() * scale;
    if (result < lower - tolerance || result > spot + tolerance) {
        throw std::domain_error("Heston analytic price violated call bounds");
    }
    return std::clamp(result, lower, spot);
}

}  // namespace mc
