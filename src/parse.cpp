#include "mc/parse.hpp"

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>

namespace mc {
namespace {

std::invalid_argument parse_error(std::string_view field_name,
                                  std::string_view detail) {
    return std::invalid_argument(std::string(field_name) + ": " +
                                 std::string(detail));
}

}  // namespace

std::uint64_t parse_u64(std::string_view text, std::string_view field_name) {
    if (text.empty() || text.front() == '-' || text.front() == '+') {
        throw parse_error(field_name, "expected an unsigned base-10 integer");
    }
    std::uint64_t value = 0;
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), value, 10);
    if (error == std::errc::result_out_of_range) {
        throw parse_error(field_name, "integer is out of range");
    }
    if (error != std::errc{} || end != text.data() + text.size()) {
        throw parse_error(field_name,
                          "expected an unsigned base-10 integer with no trailing text");
    }
    return value;
}

std::size_t parse_size(std::string_view text, std::string_view field_name) {
    const std::uint64_t value = parse_u64(text, field_name);
    if (value > std::numeric_limits<std::size_t>::max()) {
        throw parse_error(field_name, "value does not fit size_t");
    }
    return static_cast<std::size_t>(value);
}

double parse_finite_double(std::string_view text, std::string_view field_name) {
    if (text.empty()) {
        throw parse_error(field_name, "expected a finite decimal number");
    }
    double value = 0.0;
    std::istringstream stream{std::string(text)};
    stream.imbue(std::locale::classic());
    stream >> std::noskipws >> value;
    if (!stream || !stream.eof() || !std::isfinite(value)) {
        throw parse_error(field_name,
                          "expected a finite decimal number with no trailing text");
    }
    return value;
}

}  // namespace mc
