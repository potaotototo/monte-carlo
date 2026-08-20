#include "mc/parse.hpp"
#include "mc/rng.hpp"
#include "mc/run_spec.hpp"

#include <array>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

enum class OutputFormat {
    LittleEndianU64,
    Hex,
};

std::string require_value(int argc, char** argv, int& index) {
    if (index + 1 >= argc) {
        throw std::invalid_argument(std::string("missing value for ") +
                                    argv[index]);
    }
    ++index;
    return argv[index];
}

void print_help() {
    std::cout
        << "Usage: export_rng_stream [options]\n"
        << "  --words N             64-bit words to emit (default 1048576)\n"
        << "  --seed N              global Philox key (default 1)\n"
        << "  --scenario-start N    first logical scenario (default 0)\n"
        << "  --time-step N         fixed logical time step (default 0)\n"
        << "  --dimension-start N   first logical dimension (default 0)\n"
        << "  --dimensions N        dimensions interleaved per scenario (default 1)\n"
        << "  --draw-index N        fixed logical draw index (default 0)\n"
        << "  --format TYPE         le64 or hex (default le64)\n"
        << "  --help                show this message\n";
}

std::uint32_t parse_u32_field(std::string_view text,
                              std::string_view name,
                              std::uint64_t maximum) {
    const std::uint64_t value = mc::parse_u64(text, name);
    if (value > maximum) {
        throw std::invalid_argument(std::string(name) +
                                    " exceeds RNG layout v1");
    }
    return static_cast<std::uint32_t>(value);
}

void write_little_endian(std::uint64_t word) {
    std::array<char, 8> bytes{};
    for (std::size_t index = 0U; index < bytes.size(); ++index) {
        bytes[index] = static_cast<char>(
            static_cast<unsigned char>(word >> (8U * index)));
    }
    std::cout.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

}  // namespace

int main(int argc, char** argv) {
    try {
        std::uint64_t words = 1U << 20U;
        std::uint64_t seed = 1U;
        std::uint64_t scenario_start = 0U;
        std::uint32_t time_step = 0U;
        std::uint32_t dimension_start = 0U;
        std::uint32_t dimensions = 1U;
        std::uint32_t draw_index = 0U;
        OutputFormat format = OutputFormat::LittleEndianU64;

        constexpr std::uint64_t max24 = (std::uint64_t{1} << 24U) - 1U;
        for (int index = 1; index < argc; ++index) {
            const std::string_view argument = argv[index];
            if (argument == "--help") {
                print_help();
                return 0;
            }
            if (argument == "--words") {
                words = mc::parse_u64(require_value(argc, argv, index), "words");
            } else if (argument == "--seed") {
                seed = mc::parse_u64(require_value(argc, argv, index), "seed");
            } else if (argument == "--scenario-start") {
                scenario_start = mc::parse_u64(
                    require_value(argc, argv, index), "scenario-start");
            } else if (argument == "--time-step") {
                time_step = parse_u32_field(
                    require_value(argc, argv, index), "time-step", max24);
            } else if (argument == "--dimension-start") {
                dimension_start = parse_u32_field(
                    require_value(argc, argv, index), "dimension-start", 255U);
            } else if (argument == "--dimensions") {
                dimensions = parse_u32_field(
                    require_value(argc, argv, index), "dimensions", 256U);
            } else if (argument == "--draw-index") {
                draw_index = parse_u32_field(
                    require_value(argc, argv, index), "draw-index", max24);
            } else if (argument == "--format") {
                const std::string value = require_value(argc, argv, index);
                if (value == "le64") {
                    format = OutputFormat::LittleEndianU64;
                } else if (value == "hex") {
                    format = OutputFormat::Hex;
                } else {
                    throw std::invalid_argument("format must be le64 or hex");
                }
            } else {
                throw std::invalid_argument(std::string("unknown argument: ") +
                                            std::string(argument));
            }
        }
        if (words == 0U) {
            throw std::invalid_argument("words must be positive");
        }
        if (dimensions == 0U ||
            dimension_start > 256U - dimensions) {
            throw std::invalid_argument(
                "dimension range exceeds RNG layout v1's 8-bit field");
        }
        const std::uint64_t scenarios_used =
            1U + (words - 1U) / dimensions;
        if (scenario_start >= mc::kMaxScenarios ||
            scenarios_used > mc::kMaxScenarios - scenario_start) {
            throw std::invalid_argument(
                "requested stream exceeds RNG layout v1's scenario field");
        }

        if (format == OutputFormat::Hex) {
            std::cout << std::hex << std::setfill('0');
        }
        for (std::uint64_t index = 0U; index < words; ++index) {
            const std::uint64_t scenario =
                scenario_start + index / dimensions;
            const std::uint32_t dimension = dimension_start +
                static_cast<std::uint32_t>(index % dimensions);
            const std::uint64_t word = mc::random_u64(
                seed, scenario, time_step, dimension, draw_index);
            if (format == OutputFormat::Hex) {
                std::cout << std::setw(16) << word << '\n';
            } else {
                write_little_endian(word);
            }
        }
        if (!std::cout) {
            throw std::runtime_error("failed to write RNG stream");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
