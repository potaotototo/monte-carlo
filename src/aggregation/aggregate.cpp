#include "mc/aggregate.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace mc {

void AggregateStats::add(double observation) {
    if (!std::isfinite(observation)) {
        throw std::domain_error("aggregate observation must be finite");
    }
    if (n == 0) {
        n = 1;
        mean = observation;
        m2 = 0.0;
        min = observation;
        max = observation;
        return;
    }

    ++n;
    const double delta = observation - mean;
    mean += delta / static_cast<double>(n);
    const double delta_after = observation - mean;
    m2 += delta * delta_after;
    min = std::min(min, observation);
    max = std::max(max, observation);
}

std::optional<double> AggregateStats::sample_variance() const {
    if (n < 2) {
        return std::nullopt;
    }
    return m2 / static_cast<double>(n - 1U);
}

std::optional<double> AggregateStats::standard_error() const {
    const std::optional<double> variance = sample_variance();
    if (!variance.has_value()) {
        return std::nullopt;
    }
    return std::sqrt(*variance / static_cast<double>(n));
}

std::optional<std::string> AggregateStats::invariant_error(
    std::optional<std::uint64_t> expected_n) const {
    if (expected_n.has_value() && n != *expected_n) {
        return "aggregate observation count does not match the expected count";
    }
    if (n == 0U) {
        if (mean != 0.0 || m2 != 0.0 || min != 0.0 || max != 0.0) {
            return "empty aggregate does not use the canonical zero representation";
        }
        return std::nullopt;
    }
    if (!std::isfinite(mean) || !std::isfinite(m2) ||
        !std::isfinite(min) || !std::isfinite(max)) {
        return "aggregate contains a non-finite value";
    }
    if (m2 < 0.0) {
        return "aggregate m2 must be nonnegative";
    }
    if (min > max) {
        return "aggregate minimum exceeds maximum";
    }

    const double scale = std::max({1.0, std::abs(mean), std::abs(min), std::abs(max)});
    const double tolerance = 64.0 * std::numeric_limits<double>::epsilon() * scale;
    if (mean < min - tolerance || mean > max + tolerance) {
        return "aggregate mean lies outside its minimum/maximum range";
    }
    if (n == 1U &&
        (m2 != 0.0 || std::abs(mean - min) > tolerance ||
         std::abs(mean - max) > tolerance)) {
        return "single-observation aggregate fields are inconsistent";
    }
    return std::nullopt;
}

AggregateStats merge(const AggregateStats& left, const AggregateStats& right) {
    if (const std::optional<std::string> error = left.invariant_error();
        error.has_value()) {
        throw std::invalid_argument("left aggregate is invalid: " + *error);
    }
    if (const std::optional<std::string> error = right.invariant_error();
        error.has_value()) {
        throw std::invalid_argument("right aggregate is invalid: " + *error);
    }
    if (left.n == 0) {
        return right;
    }
    if (right.n == 0) {
        return left;
    }
    if (std::numeric_limits<std::uint64_t>::max() - left.n < right.n) {
        throw std::overflow_error("aggregate observation count overflow");
    }

    AggregateStats result;
    result.n = left.n + right.n;
    const double left_n = static_cast<double>(left.n);
    const double right_n = static_cast<double>(right.n);
    const double total_n = static_cast<double>(result.n);
    const double delta = right.mean - left.mean;
    result.mean = left.mean + delta * (right_n / total_n);
    result.m2 = left.m2 + right.m2 + delta * delta * (left_n * right_n / total_n);
    result.min = std::min(left.min, right.min);
    result.max = std::max(left.max, right.max);
    if (const std::optional<std::string> error = result.invariant_error();
        error.has_value()) {
        throw std::overflow_error("merged aggregate is not representable: " +
                                  *error);
    }
    return result;
}

}  // namespace mc
