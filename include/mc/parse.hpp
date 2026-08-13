#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace mc {

std::uint64_t parse_u64(std::string_view text, std::string_view field_name);
std::size_t parse_size(std::string_view text, std::string_view field_name);
double parse_finite_double(std::string_view text, std::string_view field_name);

}  // namespace mc

