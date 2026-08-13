#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace mc {

struct AggregateStats {
    std::uint64_t n = 0;
    double mean = 0.0;
    double m2 = 0.0;
    double min = 0.0;
    double max = 0.0;

    void add(double observation);
    [[nodiscard]] std::optional<double> sample_variance() const;
    [[nodiscard]] std::optional<double> standard_error() const;
    [[nodiscard]] std::optional<std::string> invariant_error(
        std::optional<std::uint64_t> expected_n = std::nullopt) const;
};

AggregateStats merge(const AggregateStats& left, const AggregateStats& right);

}  // namespace mc
