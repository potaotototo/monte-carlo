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

constexpr std::size_t kQuadratureOrder = 512U;
constexpr std::size_t kTimeQuadratureOrder = 64U;
constexpr Real kIntegrationLimit = 200.0L;

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

const QuadratureRule<kQuadratureOrder>& quadrature_rule() {
    static const auto rule = make_gauss_legendre_rule<kQuadratureOrder>();
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

Real probability(Real log_spot,
                 Real log_strike,
                 Real rate,
                 Real maturity,
                 Real initial_variance,
                 Real mean_reversion_rate,
                 Real long_run_variance,
                 Real volatility_of_variance,
                 Real correlation,
                 bool asset_probability) {
    const QuadratureRule<kQuadratureOrder>& rule = quadrature_rule();
    const QuadratureRule<kTimeQuadratureOrder>& time_rule =
        time_quadrature_rule();
    const Complex imaginary{0.0L, 1.0L};
    const Real xi_squared = volatility_of_variance * volatility_of_variance;
    const Real u = asset_probability ? 0.5L : -0.5L;
    const Real b = asset_probability
                       ? mean_reversion_rate -
                             correlation * volatility_of_variance
                       : mean_reversion_rate;
    Real weighted_sum = 0.0L;
    for (std::size_t index = 0U; index < kQuadratureOrder; ++index) {
        const Real phi = 0.5L * kIntegrationLimit *
                         (rule.nodes[index] + 1.0L);
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
        weighted_sum += rule.weights[index] * integrand;
    }
    return 0.5L + 0.5L * kIntegrationLimit * weighted_sum /
                      std::acos(-1.0L);
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
    if (xi == 0.0L || (v0 == 0.0L && kappa * theta == 0.0L)) {
        const Real integrated_variance = kappa == 0.0L
            ? v0 * t
            : theta * t +
                  (v0 - theta) * (-std::expm1(-kappa * t)) / kappa;
        price = deterministic_variance_call(s, k, r, t,
                                            integrated_variance);
    } else {
        const Real p1 = probability(std::log(s), std::log(k), r, t, v0,
                                    kappa, theta, xi, rho, true);
        const Real p2 = probability(std::log(s), std::log(k), r, t, v0,
                                    kappa, theta, xi, rho, false);
        price = s * p1 - k * std::exp(-r * t) * p2;
    }
    const double result = static_cast<double>(price);
    if (!std::isfinite(result)) {
        throw std::domain_error(
            "Heston analytic calculation produced a non-finite price");
    }
    // Numerical quadrature can miss an arbitrage bound by a few ulps. Preserve
    // the computed value unless it is only round-off outside the hard bounds.
    const double lower = std::max(spot - strike * std::exp(-rate * maturity), 0.0);
    const double scale = std::max({1.0, spot, strike});
    const double tolerance = 128.0 * std::numeric_limits<double>::epsilon() * scale;
    if (result < lower - tolerance || result > spot + tolerance) {
        throw std::domain_error("Heston analytic price violated call bounds");
    }
    return std::clamp(result, lower, spot);
}

}  // namespace mc
