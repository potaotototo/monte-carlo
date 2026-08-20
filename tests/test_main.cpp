#include "mc/aggregate.hpp"
#include "mc/bounded_queue.hpp"
#include "mc/codec.hpp"
#include "mc/coordinator.hpp"
#include "mc/engine.hpp"
#include "mc/failure_injection.hpp"
#include "mc/hash.hpp"
#include "mc/identity.hpp"
#include "mc/model.hpp"
#include "mc/parse.hpp"
#include "mc/persistence.hpp"
#include "mc/rng.hpp"
#include "mc/run_spec.hpp"

#include "../tools/process_watchdog.hpp"

#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

std::filesystem::path test_executable;

class TestFailure : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        static std::atomic<std::uint64_t> sequence{0};
        const std::filesystem::path parent =
            std::filesystem::temp_directory_path();
        for (std::uint32_t attempt = 0; attempt < 100U; ++attempt) {
            path_ = parent /
                    ("mc-r2-test-" +
                     std::to_string(sequence.fetch_add(1U)) + "-" +
                     std::to_string(attempt));
            std::error_code error;
            if (std::filesystem::create_directory(path_, error)) {
                return;
            }
        }
        throw TestFailure("could not create a temporary R2 test directory");
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    ~TemporaryDirectory() {
        std::error_code ignored;
        static_cast<void>(std::filesystem::remove_all(path_, ignored));
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

void check(bool condition, const std::string& message) {
    if (!condition) {
        throw TestFailure(message);
    }
}

void check_near(double actual, double expected, double tolerance,
                const std::string& message) {
    if (!std::isfinite(actual) || !std::isfinite(expected) ||
        !std::isfinite(tolerance) || tolerance < 0.0 ||
        std::abs(actual - expected) > tolerance) {
        throw TestFailure(message + ": actual=" + std::to_string(actual) +
                          " expected=" + std::to_string(expected));
    }
}

void check_aggregate_exact(const mc::AggregateStats& actual,
                           const mc::AggregateStats& expected,
                           const std::string& message) {
    check(actual.n == expected.n &&
              std::bit_cast<std::uint64_t>(actual.mean) ==
                  std::bit_cast<std::uint64_t>(expected.mean) &&
              std::bit_cast<std::uint64_t>(actual.m2) ==
                  std::bit_cast<std::uint64_t>(expected.m2) &&
              std::bit_cast<std::uint64_t>(actual.min) ==
                  std::bit_cast<std::uint64_t>(expected.min) &&
              std::bit_cast<std::uint64_t>(actual.max) ==
                  std::bit_cast<std::uint64_t>(expected.max),
          message);
}

mc::RunSpec r3_test_spec() {
    mc::RunSpec spec;
    spec.global_seed = 0x5A17C0DEU;
    spec.total_scenarios = 256U;
    spec.num_time_steps = 2U;
    return spec;
}

mc::RunSpec heston_test_spec() {
    mc::RunSpec spec;
    spec.model_type = mc::ModelType::Heston;
    spec.heston.emplace();
    spec.global_seed = 0x484553544F4EULL;
    spec.total_scenarios = 4'096U;
    spec.num_time_steps = 16U;
    return spec;
}

mc::RunSpec heston_replay_test_spec() {
    mc::RunSpec spec = heston_test_spec();
    spec.total_scenarios = 256U;
    spec.num_time_steps = 4U;
    return spec;
}

mc::EngineConfig r3_test_engine_config() {
    mc::EngineConfig config;
    // A single worker makes the hook trace itself deterministic. The runtime
    // correctness invariant is still checked independently across workers.
    config.worker_count = 1U;
    config.block_size = 64U;
    return config;
}

mc::RunStoreConfig r3_test_store_config(
    const std::filesystem::path& run_directory) {
    mc::RunStoreConfig config;
    config.run_directory = run_directory;
    config.checkpoint_interval_blocks = 1U;
    config.min_free_space_bytes = 0U;
    return config;
}

int run_r3_crash_child(int argc, char** argv) {
    if (argc != 7 && argc != 8) {
        return 96;
    }
    try {
        const bool use_heston = argc == 8 && std::string_view(argv[7]) == "heston";
        if (argc == 8 && !use_heston) {
            return 96;
        }
        mc::RunStoreConfig store_config = r3_test_store_config(argv[2]);
        mc::FailureInjectionConfig injection;
        injection.replay_descriptor_path = argv[3];
        injection.selected_point = mc::parse_failure_point(argv[4]);
        injection.selected_occurrence =
            mc::parse_u64(argv[5], "selected_occurrence");
        injection.failure_seed = mc::parse_u64(argv[6], "failure_seed");
        store_config.failure_injection = injection;
        static_cast<void>(mc::run_parallel_durable(
            use_heston ? heston_replay_test_spec() : r3_test_spec(),
            r3_test_engine_config(), store_config));
        return 95;
    } catch (const std::exception& error) {
        std::cerr << "R3 crash child failed before injection: "
                  << error.what() << '\n';
        return 94;
    }
}

int spawn_r3_crash_child_status(
    const std::filesystem::path& run_directory,
    const std::filesystem::path& descriptor_path,
    mc::FailurePoint point,
    std::uint64_t occurrence,
    std::uint64_t failure_seed,
    bool suppress_errors = false,
    bool use_heston = false) {
    const std::string occurrence_text = std::to_string(occurrence);
    const std::string seed_text = std::to_string(failure_seed);
    const std::string executable_text = test_executable.string();
    const std::string run_text = run_directory.string();
    const std::string descriptor_text = descriptor_path.string();
    const std::string point_text(mc::failure_point_name(point));
    const pid_t child = ::fork();
    if (child < 0) {
        throw TestFailure("could not fork R3 crash child");
    }
    if (child == 0) {
        if (suppress_errors) {
            const int null_descriptor = ::open("/dev/null", O_WRONLY);
            if (null_descriptor >= 0) {
                static_cast<void>(::dup2(null_descriptor, STDERR_FILENO));
                static_cast<void>(::close(null_descriptor));
            }
        }
        if (use_heston) {
            ::execl(executable_text.c_str(), executable_text.c_str(),
                    "--r3-crash-child", run_text.c_str(),
                    descriptor_text.c_str(), point_text.c_str(),
                    occurrence_text.c_str(), seed_text.c_str(), "heston",
                    static_cast<char*>(nullptr));
        } else {
            ::execl(executable_text.c_str(), executable_text.c_str(),
                    "--r3-crash-child", run_text.c_str(),
                    descriptor_text.c_str(), point_text.c_str(),
                    occurrence_text.c_str(), seed_text.c_str(),
                    static_cast<char*>(nullptr));
        }
        ::_exit(127);
    }
    return mc::tool::wait_for_child(
        child, std::chrono::seconds{30}, "R3 unit crash");
}

void spawn_r3_crash_child(const std::filesystem::path& run_directory,
                          const std::filesystem::path& descriptor_path,
                          mc::FailurePoint point,
                          std::uint64_t occurrence,
                          std::uint64_t failure_seed,
                          bool use_heston = false) {
    check(spawn_r3_crash_child_status(run_directory, descriptor_path, point,
                                      occurrence, failure_seed, false,
                                      use_heston) ==
              mc::kFailureInjectionExitCode,
          "R3 child did not terminate at the selected failure point");
}

void flip_file_byte(const std::filesystem::path& path,
                    std::streamoff offset) {
    std::fstream stream(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!stream) {
        throw TestFailure("could not open test file for corruption: " +
                          path.string());
    }
    stream.seekg(offset);
    char byte = 0;
    stream.read(&byte, 1);
    if (!stream) {
        throw TestFailure("could not read test byte for corruption");
    }
    byte = static_cast<char>(static_cast<unsigned char>(byte) ^ 0x80U);
    stream.seekp(offset);
    stream.write(&byte, 1);
    stream.flush();
    if (!stream) {
        throw TestFailure("could not write corrupted test byte");
    }
}

std::vector<std::uint8_t> read_test_file(
    const std::filesystem::path& path) {
    const std::uintmax_t size = std::filesystem::file_size(path);
    std::ifstream stream(path, std::ios::binary);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (!bytes.empty()) {
        stream.read(reinterpret_cast<char*>(bytes.data()),
                    static_cast<std::streamsize>(bytes.size()));
    }
    if (!stream) {
        throw TestFailure("could not read durable test file: " + path.string());
    }
    return bytes;
}

void write_test_file(const std::filesystem::path& path,
                     std::span<const std::uint8_t> bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!bytes.empty()) {
        stream.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
    }
    stream.flush();
    if (!stream) {
        throw TestFailure("could not write test file: " + path.string());
    }
}

template <typename Function>
void check_throws(Function&& function, const std::string& message) {
    try {
        function();
    } catch (const std::exception&) {
        return;
    }
    throw TestFailure(message);
}

void philox_known_answer() {
    const mc::PhiloxCounter actual = mc::philox4x32_10({0, 0, 0, 0}, {0, 0});
    const mc::PhiloxCounter expected = {
        0x6627E8D5U, 0xE169C58DU, 0xBC57AC4CU, 0x9B00DBD8U};
    check(actual == expected, "Philox4x32-10 Random123 known-answer vector mismatch");
}

void counter_layout_v1() {
    const mc::PhiloxCounter counter =
        mc::pack_counter_v1(0xAB12345678ULL, 0x654321U, 0xCDU, 0x123456U);
    check(counter[0] == 0x12345678U, "scenario low word is packed incorrectly");
    check(counter[1] == 0x654321ABU, "scenario/time word is packed incorrectly");
    check(counter[2] == 0x123456CDU, "dimension/draw word is packed incorrectly");
    check(counter[3] == 0U, "reserved counter word must be zero in RNG v1");
    check_throws(
        [] { static_cast<void>(mc::pack_counter_v1(mc::kMaxScenarios, 0, 0, 0)); },
        "counter packing should reject a 41-bit scenario ID");
}

void uniform_mapping_v2_boundaries() {
    check(mc::uniform_from_words_v2(0U, 0U) == 0x1.0p-54,
          "RNG v2 zero endpoint must map to the half-bin value");
    check(mc::uniform_from_words_v2(0U, 1U << 11U) == 0x1.0p-53,
          "RNG v2 first retained integer is mapped incorrectly");
    check(mc::uniform_from_words_v2(0xFFFFFFFFU, 0xFFFFFFFFU) ==
              1.0 - 0x1.0p-53,
          "RNG v2 maximum must remain strictly below one");
    check(mc::uniform_open01(0U, 0U, 0U) ==
              mc::uniform_from_words_v2(0x6627E8D5U, 0xE169C58DU),
          "RNG v2 counter-to-uniform golden mapping changed");
    check(mc::random_u64(0U, 0U, 0U) == 0x6627E8D5E169C58DULL,
          "raw RNG word diverged from the Random123 known answer");
    const std::uint64_t raw = mc::random_u64(99U, 17U, 3U, 2U, 11U);
    check(mc::uniform_open01(99U, 17U, 3U, 2U, 11U) ==
              mc::uniform_from_words_v2(
                  static_cast<std::uint32_t>(raw >> 32U),
                  static_cast<std::uint32_t>(raw)),
          "uniform conversion and exported raw word use different Philox data");
    const std::array<std::uint64_t, 4> interleaved_golden = {
        0x092C9B4478384BC0ULL,
        0x392FD6B226FC076CULL,
        0x389D999A609CF517ULL,
        0xEACE5522A93BE4CFULL,
    };
    for (std::size_t index = 0U; index < interleaved_golden.size(); ++index) {
        check(mc::random_u64(0x123456789ABCDEF0ULL, index / 2U, 0U,
                             static_cast<std::uint32_t>(index % 2U)) ==
                  interleaved_golden[index],
              "interleaved raw RNG stream golden mapping changed");
    }

    for (std::uint64_t scenario :
         std::array<std::uint64_t, 4>{0U, 1U, 17U, mc::kMaxScenarios - 1U}) {
        const double uniform = mc::uniform_open01(99U, scenario, 0U);
        check(uniform > 0.0 && uniform < 1.0,
              "RNG v2 produced a value outside the open unit interval");
    }
}

void inverse_normal_values() {
    check_near(mc::inverse_normal_cdf(0.5), 0.0, 1e-12,
               "inverse normal median mismatch");
    check_near(mc::inverse_normal_cdf(0.975), 1.959963984540054, 1e-8,
               "inverse normal 97.5th percentile mismatch");
    check_near(mc::inverse_normal_cdf(0.025), -1.959963984540054, 1e-8,
               "inverse normal 2.5th percentile mismatch");
}

void sha256_known_answers() {
    const std::vector<std::uint8_t> empty;
    check(mc::to_hex(mc::sha256(empty)) ==
              "e3b0c44298fc1c149afbf4c8996fb924"
              "27ae41e4649b934ca495991b7852b855",
          "SHA-256 empty-string known-answer mismatch");
    const std::vector<std::uint8_t> abc = {'a', 'b', 'c'};
    check(mc::to_hex(mc::sha256(abc)) ==
              "ba7816bf8f01cfea414140de5dae2223"
              "b00361a396177a9cb410ff61f20015ad",
          "SHA-256 abc known-answer mismatch");
    const std::string two_block_message =
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    const std::vector<std::uint8_t> two_block_bytes(two_block_message.begin(),
                                                    two_block_message.end());
    check(mc::to_hex(mc::sha256(two_block_bytes)) ==
              "248d6a61d20638b8e5c026930c3e6039"
              "a33ce45964ff2167f6ecedd419db06c1",
          "SHA-256 two-block padding known-answer mismatch");
    mc::Sha256Hasher streaming;
    streaming.update(std::span<const std::uint8_t>(two_block_bytes).first(7U));
    streaming.update(std::span<const std::uint8_t>(two_block_bytes).subspan(7U, 41U));
    streaming.update(std::span<const std::uint8_t>(two_block_bytes).subspan(48U));
    check(streaming.finalize() == mc::sha256(two_block_bytes) &&
              streaming.finalize() == mc::sha256(two_block_bytes),
          "incremental SHA-256 changed the one-shot digest or was destructive");
    std::vector<std::uint8_t> long_message(1000U);
    for (std::size_t index = 0U; index < long_message.size(); ++index) {
        long_message[index] = static_cast<std::uint8_t>(index & 0xFFU);
    }
    mc::Sha256Hasher chunked;
    chunked.update(std::span<const std::uint8_t>(long_message).first(13U));
    chunked.update(
        std::span<const std::uint8_t>(long_message).subspan(13U, 100U));
    chunked.update(std::span<const std::uint8_t>(long_message).subspan(113U));
    check(chunked.finalize() == mc::sha256(long_message),
          "incremental SHA-256 mishandled a chunk crossing data blocks");
}

void canonical_run_spec_hashing() {
    mc::RunSpec first;
    mc::RunSpec second = first;
    check(mc::encode_run_spec_payload(first) == mc::encode_run_spec_payload(second),
          "equal run specifications must have identical canonical bytes");
    check(mc::run_spec_hash(first) == mc::run_spec_hash(second),
          "equal run specifications must have identical hashes");
    const std::string default_run_hash = mc::to_hex(mc::run_spec_hash(first));
    check(default_run_hash ==
              "19a2aa7d5da2e20312af30c2dc418dce"
              "b3069429328f7406d2ec31f0d34b248a",
          "RunSpec v1 canonical byte contract changed unexpectedly: " +
              default_run_hash);
    second.global_seed += 1U;
    check(mc::run_spec_hash(first) != mc::run_spec_hash(second),
          "a seed change must change the run specification hash");

    second = first;
    first.rate = 0.0;
    second.rate = -0.0;
    check(mc::run_spec_hash(first) != mc::run_spec_hash(second),
          "canonical doubles must preserve distinct IEEE-754 bit patterns");

    mc::EngineConfig first_layout;
    mc::EngineConfig second_layout = first_layout;
    second_layout.block_size *= 2U;
    check(mc::execution_layout_hash(first, first_layout) !=
              mc::execution_layout_hash(first, second_layout),
          "block size must change execution-layout identity");
    const std::string default_layout_hash =
        mc::to_hex(mc::execution_layout_hash(first, first_layout));
    check(default_layout_hash ==
              "cfa142b5e8d0b22ecf3852a9711613bd"
              "6fab14dd238970c7cfecc95ba097ab07",
          "ExecutionLayout v1 canonical bytes changed: " + default_layout_hash);
    const mc::BuildIdentity build = mc::current_build_identity();
    check(!build.description.empty() && build.hash == mc::current_build_identity().hash,
          "build identity must be nonempty and stable within one executable");
    check(build.description.find("fp_contract=off") != std::string::npos &&
              build.description.find("fast_math=off") != std::string::npos &&
              build.description.find("optimizer=") != std::string::npos &&
              build.description.find("source=") != std::string::npos &&
              build.description.find("build_flags=") != std::string::npos &&
              build.description.find("build_config=") != std::string::npos &&
              build.description.find("cpu_policy=") != std::string::npos &&
              build.description.find("cpu_features=") != std::string::npos,
          "build identity does not record the pinned floating-point policy");
}

void strict_numeric_parsing() {
    check(mc::parse_u64("42", "value") == 42U,
          "strict unsigned parser rejected a valid integer");
    check_near(mc::parse_finite_double("-0.125", "value"), -0.125, 0.0,
               "strict floating parser rejected a valid decimal");
    check_throws([] { static_cast<void>(mc::parse_u64("10junk", "value")); },
                 "unsigned parser accepted trailing text");
    check_throws([] { static_cast<void>(mc::parse_u64("-1", "value")); },
                 "unsigned parser accepted a negative sign");
    check_throws([] { static_cast<void>(mc::parse_u64(" 1", "value")); },
                 "unsigned parser accepted leading whitespace");
    check_throws(
        [] { static_cast<void>(mc::parse_finite_double("0.2junk", "value")); },
        "floating parser accepted trailing text");
    check_throws(
        [] { static_cast<void>(mc::parse_finite_double("nan", "value")); },
        "floating parser accepted NaN");
}

void aggregate_and_merge() {
    mc::AggregateStats all;
    for (double value : {1.0, 2.0, 3.0, 4.0}) {
        all.add(value);
    }
    check(all.n == 4, "Welford count mismatch");
    check_near(all.mean, 2.5, 1e-15, "Welford mean mismatch");
    check(all.sample_variance().has_value(), "sample variance should be available");
    check_near(*all.sample_variance(), 5.0 / 3.0, 1e-15,
               "Welford sample variance mismatch");

    mc::AggregateStats left;
    left.add(1.0);
    left.add(2.0);
    mc::AggregateStats right;
    right.add(3.0);
    right.add(4.0);
    const mc::AggregateStats combined = mc::merge(left, right);
    check_near(combined.mean, all.mean, 1e-15, "pairwise mean mismatch");
    check_near(combined.m2, all.m2, 1e-15, "pairwise m2 mismatch");
}

void small_sample_and_aggregate_invariants() {
    mc::AggregateStats empty;
    check(!empty.sample_variance().has_value() &&
              !empty.standard_error().has_value(),
          "empty aggregate statistics should be unavailable");
    check(!empty.invariant_error().has_value(),
          "canonical empty aggregate should be valid");

    mc::AggregateStats one;
    one.add(7.0);
    check(!one.sample_variance().has_value() &&
              !one.standard_error().has_value(),
          "one observation must not report variance or standard error");
    mc::RunResult one_result{one, 1, 1, 1};
    check(!one_result.confidence_low().has_value() &&
              !one_result.confidence_high().has_value(),
          "one observation must not report a confidence interval");

    mc::AggregateStats two;
    two.add(7.0);
    two.add(7.0);
    check(two.standard_error().has_value() && *two.standard_error() == 0.0,
          "valid zero-variance sample should report zero standard error");
    mc::RunResult two_result{two, 2, 1, 1};
    check(!two_result.confidence_low().has_value(),
          "small samples should not report a normal-approximation interval");

    mc::AggregateStats thirty;
    for (std::uint64_t index = 0; index < mc::kMinNormalConfidenceObservations;
         ++index) {
        thirty.add(static_cast<double>(index));
    }
    mc::RunResult large_result{thirty, thirty.n, 1, 1};
    check(large_result.confidence_low().has_value() &&
              large_result.confidence_high().has_value(),
          "normal confidence interval should be available at the declared threshold");

    mc::AggregateStats invalid = one;
    invalid.m2 = -1.0;
    check(invalid.invariant_error().has_value(), "negative m2 was accepted");
    invalid = one;
    invalid.min = 8.0;
    check(invalid.invariant_error().has_value(),
          "single-observation field inconsistency was accepted");
    invalid = two;
    invalid.min = 9.0;
    invalid.max = 8.0;
    check(invalid.invariant_error().has_value(), "minimum above maximum was accepted");

    check_throws(
        [] {
            check_near(std::numeric_limits<double>::quiet_NaN(), 0.0, 1.0,
                       "NaN must fail");
        },
        "test tolerance helper accepted NaN");
}

void black_scholes_oracle() {
    const double price = mc::black_scholes_call_price(100.0, 100.0, 0.05, 0.2, 1.0);
    check_near(price, 10.450583572185565, 1e-12,
               "Black-Scholes reference price mismatch");
}

void deterministic_across_worker_counts() {
    mc::RunSpec spec;
    spec.global_seed = 0x0123456789ABCDEFULL;
    spec.total_scenarios = 16'384;
    spec.num_time_steps = 16;
    spec.payoff_type = mc::PayoffType::AsianCall;

    mc::EngineConfig config;
    config.worker_count = 1;
    config.block_size = 512;
    config.assignment_queue_capacity = 1;
    config.completion_queue_capacity = 1;
    const mc::RunResult reference = mc::run_parallel(spec, config);

    for (const std::size_t worker_count : {2U, 4U, 8U}) {
        config.worker_count = worker_count;
        const mc::RunResult candidate = mc::run_parallel(spec, config);
        check(reference.aggregate.n == candidate.aggregate.n,
              "deterministic count mismatch");
        check(reference.aggregate.mean == candidate.aggregate.mean,
              "fixed reduction mean changed with worker count");
        check(reference.aggregate.m2 == candidate.aggregate.m2,
              "fixed reduction m2 changed with worker count");
        check(reference.aggregate.min == candidate.aggregate.min,
              "minimum changed with worker count");
        check(reference.aggregate.max == candidate.aggregate.max,
              "maximum changed with worker count");
    }
}

void monte_carlo_converges_to_black_scholes() {
    mc::RunSpec spec;
    spec.global_seed = 42;
    spec.total_scenarios = 200'000;
    spec.num_time_steps = 1;

    mc::EngineConfig config;
    config.worker_count = 4;
    config.block_size = 2'048;
    const mc::RunResult result = mc::run_parallel(spec, config);
    const double analytic = mc::black_scholes_call_price(
        spec.spot, spec.strike, spec.rate, spec.volatility, spec.maturity);
    check(result.aggregate.standard_error().has_value(),
          "standard error should be available for the convergence test");
    check(std::abs(result.aggregate.mean - analytic) <=
              4.0 * *result.aggregate.standard_error(),
          "Monte Carlo estimate is more than four standard errors from Black-Scholes");
}

void antithetic_pair_means_reduce_error() {
    mc::RunSpec plain;
    plain.global_seed = 91;
    plain.total_scenarios = 200'000;

    mc::RunSpec paired = plain;
    paired.antithetic = true;

    mc::EngineConfig config;
    config.worker_count = 4;
    config.block_size = 2'048;
    const mc::RunResult plain_result = mc::run_parallel(plain, config);
    const mc::RunResult paired_result = mc::run_parallel(paired, config);

    check(paired_result.aggregate.n == paired.total_scenarios / 2U,
          "antithetic accumulator must count pair-means, not raw payoffs");
    check(paired_result.aggregate.standard_error().has_value() &&
              plain_result.aggregate.standard_error().has_value() &&
              *paired_result.aggregate.standard_error() <
                  *plain_result.aggregate.standard_error(),
          "antithetic pair-means did not reduce the measured standard error");
}

void fused_antithetic_pair_preserves_estimator() {
    mc::RunSpec spec;
    spec.total_scenarios = 100;
    spec.num_time_steps = 32;
    spec.antithetic = true;
    const mc::GbmKernel kernel(spec);
    const double unfused =
        0.5 * (kernel.discounted_payoff(20) + kernel.discounted_payoff(21));
    check_near(kernel.antithetic_pair_mean(20), unfused, 1e-15,
               "fused antithetic path changed the pair-mean estimator");
}

void heston_run_spec_identity_and_warnings() {
    mc::RunSpec missing;
    missing.model_type = mc::ModelType::Heston;
    check_throws([&] { missing.validate(); },
                 "Heston RunSpec accepted a missing parameter payload");

    mc::RunSpec unexpected;
    unexpected.heston.emplace();
    check_throws([&] { unexpected.validate(); },
                 "GBM RunSpec accepted inactive Heston parameters");

    mc::RunSpec spec = heston_test_spec();
    spec.heston->mean_reversion_rate = 1.0;
    spec.heston->long_run_variance = 0.04;
    spec.heston->volatility_of_variance = 0.5;
    spec.validate();
    const std::vector<mc::RunWarning> warnings = spec.warnings();
    check(warnings.size() == 1U &&
              warnings.front().code ==
                  mc::RunWarningCode::HestonFellerConditionViolated &&
              warnings.front().observed_value < warnings.front().threshold,
          "Feller violation did not produce structured RunSpec metadata");

    const mc::Sha256Digest original_hash = mc::run_spec_hash(spec);
    mc::RunSpec changed = spec;
    changed.heston->correlation = -0.25;
    check(mc::run_spec_hash(changed) != original_hash,
          "active Heston parameter did not change stochastic identity");
    changed = spec;
    changed.volatility = 0.9;
    check(mc::run_spec_hash(changed) == original_hash,
          "inactive GBM volatility changed a Heston stochastic identity");

    changed = spec;
    changed.heston->correlation = 1.0001;
    check_throws([&] { changed.validate(); },
                 "Heston accepted correlation outside [-1,1]");
    changed = spec;
    changed.heston->initial_variance = -0.01;
    check_throws([&] { changed.validate(); },
                 "Heston accepted negative initial variance");
    changed = spec;
    changed.heston->mean_reversion_rate = -0.01;
    check_throws([&] { changed.validate(); },
                 "Heston accepted negative mean reversion");
    changed = spec;
    changed.heston->long_run_variance = -0.01;
    check_throws([&] { changed.validate(); },
                 "Heston accepted negative long-run variance");
    changed = spec;
    changed.heston->volatility_of_variance = -0.01;
    check_throws([&] { changed.validate(); },
                 "Heston accepted negative volatility of variance");
    changed = spec;
    changed.heston->correlation =
        std::numeric_limits<double>::quiet_NaN();
    check_throws([&] { changed.validate(); },
                 "Heston accepted a non-finite correlation");
    changed = spec;
    ++changed.heston->discretization_version;
    check_throws([&] { changed.validate(); },
                 "Heston accepted an unpinned discretization version");

    changed = spec;
    changed.heston->volatility_of_variance = 0.0;
    check(changed.warnings().empty() &&
              std::isinf(changed.heston->feller_ratio()),
          "zero volatility of variance mishandled the Feller condition");

    mc::EngineConfig config;
    config.block_size = 256U;
    const mc::BuildIdentity build = mc::current_build_identity();
    mc::RunMetadata metadata;
    metadata.spec = spec;
    metadata.block_size = config.block_size;
    metadata.block_count =
        1U + (spec.total_scenarios - 1U) / config.block_size;
    metadata.run_spec_hash = original_hash;
    metadata.execution_layout_hash = mc::execution_layout_hash(spec, config);
    metadata.build_fingerprint = build.hash;
    metadata.build_description = build.description;
    metadata.run_id = mc::durable_run_id(metadata.run_spec_hash,
                                         metadata.execution_layout_hash);
    const mc::RunMetadata decoded =
        mc::decode_run_metadata(mc::encode_run_metadata(metadata));
    check(decoded.spec.heston.has_value() &&
              decoded.spec.heston->correlation == spec.heston->correlation &&
              decoded.run_spec_hash == original_hash &&
              decoded.warnings().size() == 1U,
          "durable Heston metadata lost parameters, identity, or warnings");
}

void heston_rng_dimension_smoke() {
    constexpr std::uint64_t samples = 65'536U;
    double sum_x = 0.0;
    double sum_y = 0.0;
    double sum_xx = 0.0;
    double sum_yy = 0.0;
    double sum_xy = 0.0;
    for (std::uint64_t scenario = 0U; scenario < samples; ++scenario) {
        const double x = mc::standard_normal(0xD1A3E510ULL, scenario, 0U, 0U);
        const double y = mc::standard_normal(0xD1A3E510ULL, scenario, 0U, 1U);
        sum_x += x;
        sum_y += y;
        sum_xx += x * x;
        sum_yy += y * y;
        sum_xy += x * y;
    }
    const double count = static_cast<double>(samples);
    const double mean_x = sum_x / count;
    const double mean_y = sum_y / count;
    const double variance_x = sum_xx / count - mean_x * mean_x;
    const double variance_y = sum_yy / count - mean_y * mean_y;
    const double covariance = sum_xy / count - mean_x * mean_y;
    const double correlation =
        covariance / std::sqrt(variance_x * variance_y);
    check(std::abs(mean_x) < 0.02 && std::abs(mean_y) < 0.02 &&
              variance_x > 0.95 && variance_x < 1.05 &&
              variance_y > 0.95 && variance_y < 1.05 &&
              std::abs(correlation) < 0.02,
          "Heston RNG dimensions failed the deterministic moment/correlation smoke test");
}

void heston_analytic_quantlib_grid() {
    struct Case {
        double spot;
        double strike;
        double rate;
        double maturity;
        mc::HestonParams parameters;
        double expected;
    };
    const std::vector<Case> cases = {
        {100.0, 100.0, 0.05, 1.0, {0.04, 1.5, 0.04, 0.3, -0.7},
         10.361869020966115},
        {100.0, 120.0, 0.05, 1.0, {0.04, 1.5, 0.04, 0.3, -0.7},
         2.193309940983057},
        {100.0, 100.0, 0.01, 2.0, {0.09, 1.2, 0.08, 0.9, -0.45},
         14.435003424669659},
        {80.0, 70.0, 0.03, 0.4, {0.06, 2.0, 0.05, 0.5, 0.4},
         11.579204967505497},
        // QuantLib's Kahl-Jaeckel long-maturity/deep-OTM stress case.
        {100.0, 200.0, 0.0, 10.0, {0.16, 1.0, 0.16, 2.0, -0.8},
         4.952114720879724},
    };
    for (const Case& test_case : cases) {
        check_near(mc::heston_european_call_price(
                       test_case.spot, test_case.strike, test_case.rate,
                       test_case.maturity, test_case.parameters),
                   test_case.expected, 5.0e-10,
                   "Heston analytic price diverged from QuantLib grid");
    }
}

void heston_analytic_limits_and_guards() {
    mc::HestonParams parameters;
    parameters.initial_variance = 0.09;
    parameters.long_run_variance = 0.04;
    parameters.mean_reversion_rate = 1.25;
    parameters.volatility_of_variance = 0.0;
    parameters.correlation = 1.0;
    constexpr double maturity = 2.0;
    const double integrated_variance =
        parameters.long_run_variance * maturity +
        (parameters.initial_variance - parameters.long_run_variance) *
            (-std::expm1(-parameters.mean_reversion_rate * maturity)) /
            parameters.mean_reversion_rate;
    const double effective_volatility =
        std::sqrt(integrated_variance / maturity);
    check_near(mc::heston_european_call_price(
                   100.0, 110.0, -0.01, maturity, parameters),
               mc::black_scholes_call_price(
                   100.0, 110.0, -0.01, effective_volatility, maturity),
               2.0e-13,
               "zero-xi Heston oracle mishandled deterministic variance");

    parameters.initial_variance = 0.04;
    parameters.long_run_variance = 0.04;
    parameters.mean_reversion_rate = 1.5;
    parameters.volatility_of_variance = 1.0e-10;
    parameters.correlation = -0.7;
    check_near(mc::heston_european_call_price(
                   100.0, 100.0, 0.05, 1.0, parameters),
               mc::black_scholes_call_price(100.0, 100.0, 0.05, 0.2, 1.0),
               2.0e-8,
               "small-xi Heston oracle lost its deterministic limit");

    parameters.initial_variance = 0.0;
    parameters.long_run_variance = 0.0;
    parameters.mean_reversion_rate = 0.0;
    parameters.volatility_of_variance = 0.8;
    check_near(mc::heston_european_call_price(
                   100.0, 90.0, 0.05, 1.0, parameters),
               100.0 - 90.0 * std::exp(-0.05), 1.0e-14,
               "absorbing-zero Heston oracle mishandled deterministic asset");

    parameters.initial_variance = -0.01;
    check_throws(
        [&] {
            static_cast<void>(mc::heston_european_call_price(
                100.0, 100.0, 0.05, 1.0, parameters));
        },
        "Heston analytic oracle accepted negative initial variance");
    parameters.initial_variance = 0.04;
    parameters.correlation = 1.01;
    check_throws(
        [&] {
            static_cast<void>(mc::heston_european_call_price(
                100.0, 100.0, 0.05, 1.0, parameters));
        },
        "Heston analytic oracle accepted invalid correlation");
}

void heston_constant_variance_matches_gbm_limit() {
    mc::RunSpec gbm;
    gbm.global_seed = 0xC01157A7ULL;
    gbm.total_scenarios = 256U;
    gbm.num_time_steps = 16U;
    gbm.volatility = 0.25;

    mc::RunSpec heston = gbm;
    heston.model_type = mc::ModelType::Heston;
    heston.heston.emplace();
    heston.heston->initial_variance = gbm.volatility * gbm.volatility;
    heston.heston->long_run_variance = heston.heston->initial_variance;
    heston.heston->mean_reversion_rate = 1.0;
    heston.heston->volatility_of_variance = 0.0;
    heston.heston->correlation = -0.8;

    for (const mc::PayoffType payoff :
         {mc::PayoffType::EuropeanCall, mc::PayoffType::AsianCall}) {
        gbm.payoff_type = payoff;
        heston.payoff_type = payoff;
        const mc::GbmKernel gbm_kernel(gbm);
        const mc::HestonKernel heston_kernel(heston);
        for (std::uint64_t scenario = 0U; scenario < gbm.total_scenarios;
             ++scenario) {
            check_near(heston_kernel.discounted_payoff(scenario),
                       gbm_kernel.discounted_payoff(scenario), 2.0e-12,
                       "constant-variance Heston diverged from the GBM limit");
        }
    }
}

void heston_determinism_and_antithetics() {
    mc::RunSpec spec = heston_test_spec();
    spec.payoff_type = mc::PayoffType::AsianCall;
    mc::EngineConfig config;
    config.worker_count = 1U;
    config.block_size = 256U;
    config.assignment_queue_capacity = 1U;
    config.completion_queue_capacity = 1U;
    const mc::RunResult reference = mc::run_parallel(spec, config);
    for (const std::size_t workers : {2U, 4U, 8U}) {
        config.worker_count = workers;
        check_aggregate_exact(mc::run_parallel(spec, config).aggregate,
                              reference.aggregate,
                              "Heston aggregate changed with worker count");
    }

    spec.antithetic = true;
    const mc::HestonKernel kernel(spec);
    const double unfused = 0.5 *
        (kernel.discounted_payoff(20U) + kernel.discounted_payoff(21U));
    check_near(kernel.antithetic_pair_mean(20U), unfused, 1.0e-15,
               "fused Heston antithetic path changed the estimator");
    config.worker_count = 1U;
    const mc::RunResult paired_reference = mc::run_parallel(spec, config);
    check(paired_reference.aggregate.n == spec.total_scenarios / 2U,
          "Heston antithetic observations were not pair means");
    config.worker_count = 4U;
    check_aggregate_exact(mc::run_parallel(spec, config).aggregate,
                          paired_reference.aggregate,
                          "antithetic Heston aggregate changed with workers");
}

void heston_durable_recovery_round_trip() {
    TemporaryDirectory directory;
    mc::RunSpec spec = heston_test_spec();
    spec.total_scenarios = 2'048U;
    mc::EngineConfig config;
    config.worker_count = 4U;
    config.block_size = 256U;
    const mc::RunResult clean = mc::run_parallel(spec, config);
    mc::RunStoreConfig store_config;
    store_config.run_directory = directory.path() / "heston-run";
    store_config.checkpoint_interval_blocks = 2U;
    store_config.min_free_space_bytes = 0U;
    const mc::DurableRunResult completed =
        mc::run_parallel_durable(spec, config, store_config);
    check_aggregate_exact(completed.run_result.aggregate, clean.aggregate,
                          "durable Heston result differed from clean execution");
    const mc::DurableRunResult reopened =
        mc::run_parallel_durable(spec, config, store_config);
    check(reopened.recovered_blocks == completed.run_result.block_count &&
              reopened.computed_blocks == 0U,
          "completed Heston recovery recomputed committed blocks");
    check_aggregate_exact(reopened.run_result.aggregate, clean.aggregate,
                          "reopened Heston aggregate changed");
}

void heston_descriptor_driven_crash_replay() {
    TemporaryDirectory directory;
    const std::filesystem::path first_run = directory.path() / "first-run";
    const std::filesystem::path descriptor_path =
        directory.path() / "heston.replay";
    spawn_r3_crash_child(
        first_run, descriptor_path, mc::FailurePoint::ResultAfterRename,
        1U, 0xA5A5U, true);
    const mc::ReplayDescriptor replay =
        mc::read_replay_descriptor(descriptor_path);
    const mc::RunSpec expected_spec = heston_replay_test_spec();
    check(replay.spec.model_type == mc::ModelType::Heston &&
              replay.spec.heston.has_value() &&
              replay.spec.heston->correlation ==
                  expected_spec.heston->correlation &&
              replay.run_spec_hash == mc::run_spec_hash(expected_spec),
          "Heston replay descriptor lost its tagged model identity");

    const std::filesystem::path replay_run = directory.path() / "replay-run";
    const std::filesystem::path observed_path =
        directory.path() / "heston-observed.replay";
    const pid_t child = ::fork();
    if (child < 0) {
        throw TestFailure("could not fork Heston descriptor replay child");
    }
    if (child == 0) {
        try {
            mc::RunStoreConfig store_config;
            store_config.run_directory = replay_run;
            store_config.checkpoint_interval_blocks =
                replay.checkpoint_interval_blocks;
            store_config.max_storage_bytes = replay.max_storage_bytes;
            store_config.max_storage_files = replay.max_storage_files;
            store_config.min_free_space_bytes = replay.min_free_space_bytes;
            store_config.max_manifest_bytes = replay.max_manifest_bytes;
            mc::FailureInjectionConfig injection = replay.injection;
            injection.replay_descriptor_path = observed_path;
            store_config.failure_injection = injection;
            static_cast<void>(mc::run_parallel_durable(
                replay.spec, replay.engine_config, store_config));
            ::_exit(95);
        } catch (...) {
            ::_exit(94);
        }
    }
    check(mc::tool::wait_for_child(
              child, std::chrono::seconds{30}, "Heston descriptor replay") ==
              mc::kFailureInjectionExitCode,
          "descriptor-driven Heston replay missed the recorded crash point");
    const mc::ReplayDescriptor observed =
        mc::read_replay_descriptor(observed_path);
    check(observed.run_spec_hash == replay.run_spec_hash &&
              observed.observed_trace_hash == replay.observed_trace_hash &&
              observed.observed_trace_events == replay.observed_trace_events &&
              observed.run_incarnation == replay.run_incarnation &&
              observed.block_id == replay.block_id &&
              observed.checkpoint_sequence == replay.checkpoint_sequence,
          "descriptor-driven Heston replay changed its crash identity");

    const mc::RunResult clean =
        mc::run_parallel(expected_spec, r3_test_engine_config());
    mc::RunStoreConfig recovery = r3_test_store_config(first_run);
    const mc::DurableRunResult recovered = mc::run_parallel_durable(
        expected_spec, r3_test_engine_config(), recovery);
    check_aggregate_exact(recovered.run_result.aggregate, clean.aggregate,
                          "crash-recovered Heston aggregate changed");
}

mc::BlockResult valid_result(const mc::RunSpec& spec,
                             const mc::EngineConfig& config,
                             const mc::ScenarioBlock& block) {
    mc::BlockResult result;
    result.block = block;
    result.aggregate = mc::compute_block(spec, block);
    result.run_spec_hash = mc::run_spec_hash(spec);
    result.execution_layout_hash = mc::execution_layout_hash(spec, config);
    result.build_fingerprint = mc::current_build_identity().hash;
    result.payload_checksum =
        mc::aggregate_payload_hash(result.aggregate, spec.stats_schema_version);
    result.rng_version = spec.rng_version;
    result.stats_schema_version = spec.stats_schema_version;
    result.worker_id = 7;
    return result;
}

void commit_test_block(mc::DurableRunStore& store,
                       const mc::RunSpec& spec,
                       const mc::EngineConfig& config,
                       std::size_t index,
                       mc::CoordinatorState& coordinator,
                       std::vector<mc::AggregateStats>& leaves,
                       std::vector<bool>& received) {
    const mc::BlockResult result =
        valid_result(spec, config, coordinator.blocks[index]);
    check(mc::validate_result(result, coordinator).status ==
              mc::ValidationStatus::Accepted,
          "test result was not valid before durable publication");
    store.record_result(result);
    check(mc::commit_result(result, coordinator).status ==
              mc::ValidationStatus::Accepted,
          "test result did not enter the pending commit set");
    leaves[index] = result.aggregate;
    received[index] = true;
}

void durable_codec_contract() {
    const std::string crc_message = "123456789";
    const std::vector<std::uint8_t> crc_bytes(crc_message.begin(),
                                              crc_message.end());
    check(mc::crc32c(crc_bytes) == 0xE3069283U,
          "CRC32C known-answer vector mismatch");

    mc::RunSpec spec;
    spec.total_scenarios = 8;
    mc::EngineConfig config;
    config.block_size = 4;
    mc::RunMetadata metadata;
    metadata.spec = spec;
    metadata.block_size = config.block_size;
    metadata.block_count = 2;
    metadata.run_spec_hash = mc::run_spec_hash(spec);
    metadata.execution_layout_hash = mc::execution_layout_hash(spec, config);
    metadata.build_description = "golden-build-v1";
    metadata.build_fingerprint = mc::sha256(std::vector<std::uint8_t>(
        metadata.build_description.begin(), metadata.build_description.end()));
    metadata.run_id = mc::durable_run_id(metadata.run_spec_hash,
                                         metadata.execution_layout_hash);

    const std::vector<std::uint8_t> metadata_bytes =
        mc::encode_run_metadata(metadata);
    const std::string metadata_hash =
        mc::to_hex(mc::sha256(metadata_bytes));
    check(metadata_hash ==
              "399111894879bdfc48b995eb543d0a2b"
              "f19ffec11c4253a77ad147aaf1eab838",
          "RunMetadata v1 canonical bytes changed: " + metadata_hash);
    check(metadata_bytes == mc::encode_run_metadata(metadata),
          "metadata serialization is not canonical");
    const mc::RunMetadata decoded_metadata =
        mc::decode_run_metadata(metadata_bytes);
    check(decoded_metadata.run_id == metadata.run_id &&
              decoded_metadata.block_size == metadata.block_size &&
              decoded_metadata.block_count == metadata.block_count,
          "metadata round trip changed identity fields");

    std::vector<mc::ScenarioBlock> blocks = mc::make_blocks(spec, config);
    blocks[0].run_incarnation = 1;
    mc::BlockResult result = valid_result(spec, config, blocks[0]);
    result.build_fingerprint = metadata.build_fingerprint;
    const mc::DurableBlockRecord record{metadata.run_id, result};
    const std::vector<std::uint8_t> record_bytes =
        mc::encode_block_record(record);
    const std::string record_hash = mc::to_hex(mc::sha256(record_bytes));
    check(record_hash ==
              "c255f5f1777d15ddea5735ae28e877ca"
              "8f2796559bf773f46177e65760666d8f",
          "BlockRecord v1 canonical bytes changed: " + record_hash);
    const mc::DurableBlockRecord decoded_record =
        mc::decode_block_record(record_bytes);
    check(decoded_record.run_id == metadata.run_id &&
              decoded_record.result.payload_checksum ==
                  result.payload_checksum,
          "block record round trip changed identity fields");
    check_aggregate_exact(decoded_record.result.aggregate, result.aggregate,
                          "block record round trip changed aggregate bits");

    mc::RunManifest manifest;
    manifest.run_id = metadata.run_id;
    manifest.run_spec_hash = metadata.run_spec_hash;
    manifest.execution_layout_hash = metadata.execution_layout_hash;
    manifest.build_fingerprint = metadata.build_fingerprint;
    manifest.sequence = 7;
    manifest.run_incarnation = 1;
    manifest.rng_version = spec.rng_version;
    manifest.stats_schema_version = spec.stats_schema_version;
    manifest.block_count = 2;
    manifest.lease_epochs = {1, 1};
    manifest.committed_blocks.push_back(
        mc::ManifestEntry{0, 1, 1, result.payload_checksum});
    manifest.committed_aggregate = result.aggregate;
    const std::vector<std::uint8_t> manifest_bytes =
        mc::encode_manifest(manifest);
    const std::string manifest_hash =
        mc::to_hex(mc::sha256(manifest_bytes));
    check(manifest_hash ==
              "288a0ab52ad5ef2b6e91e2b861e0e02c"
              "58722b12bde699ff673cc2665340e139",
          "Manifest v1 canonical bytes changed: " + manifest_hash);
    check(manifest_bytes == mc::encode_manifest(manifest),
          "manifest serialization is not canonical");
    const mc::RunManifest decoded_manifest =
        mc::decode_manifest(manifest_bytes);
    check(decoded_manifest.sequence == manifest.sequence &&
              decoded_manifest.committed_blocks.size() == 1U,
          "manifest round trip changed its commit set");
    check_aggregate_exact(decoded_manifest.committed_aggregate,
                          result.aggregate,
                          "manifest round trip changed aggregate bits");

    mc::RunManifest failed_manifest = manifest;
    failed_manifest.status = mc::DurableRunStatus::Failed;
    failed_manifest.failure = mc::FailureRecord{
        mc::ValidationStatus::DeterminismError,
        0U,
        1U,
        1U,
        result.payload_checksum,
        metadata.run_spec_hash,
        "golden determinism failure",
    };
    const std::vector<std::uint8_t> failed_manifest_bytes =
        mc::encode_manifest(failed_manifest);
    const std::string failed_manifest_hash =
        mc::to_hex(mc::sha256(failed_manifest_bytes));
    check(failed_manifest_hash ==
              "2688d280e9ee622475cd63830e697eaf9"
              "2ab002a5de64ba22b7c582374bf4037",
          "Failed Manifest v1 status-code contract changed: " +
              failed_manifest_hash);
    const mc::RunManifest decoded_failed_manifest =
        mc::decode_manifest(failed_manifest_bytes);
    check(decoded_failed_manifest.failure.has_value() &&
              decoded_failed_manifest.failure->status ==
                  mc::ValidationStatus::DeterminismError,
          "failed manifest round trip changed its stable failure status");

    std::vector<std::uint8_t> corrupted = manifest_bytes;
    corrupted[corrupted.size() / 2U] ^= 0x80U;
    check_throws([&] { static_cast<void>(mc::decode_manifest(corrupted)); },
                 "manifest CRC accepted corrupted bytes");
    corrupted = manifest_bytes;
    corrupted.pop_back();
    check_throws([&] { static_cast<void>(mc::decode_manifest(corrupted)); },
                 "manifest decoder accepted a truncated envelope");
}

void durable_recovery_exactly_once() {
    TemporaryDirectory directory;
    mc::RunSpec spec;
    spec.global_seed = 0x8877665544332211ULL;
    spec.total_scenarios = 4'096;
    spec.num_time_steps = 8;
    spec.payoff_type = mc::PayoffType::AsianCall;
    mc::EngineConfig config;
    config.worker_count = 3;
    config.block_size = 512;
    const mc::RunResult clean = mc::run_parallel(spec, config);

    mc::RunStoreConfig store_config;
    store_config.run_directory = directory.path();
    store_config.checkpoint_interval_blocks = 2;
    store_config.min_free_space_bytes = 0;
    std::filesystem::path orphan_path;
    {
        mc::DurableRunStore store =
            mc::DurableRunStore::open(spec, config, store_config);
        const std::vector<mc::ScenarioBlock> blocks =
            store.recovery_state().blocks;
        mc::CoordinatorState coordinator =
            mc::make_coordinator_state(spec, config, blocks);
        std::vector<mc::AggregateStats> leaves(blocks.size());
        std::vector<bool> received(blocks.size(), false);
        commit_test_block(store, spec, config, 0, coordinator, leaves, received);
        commit_test_block(store, spec, config, 1, coordinator, leaves, received);
        store.checkpoint(coordinator, leaves, received);

        // This durable result is intentionally not named by a manifest. A
        // restart must treat it as an orphan and recompute the block.
        const mc::BlockResult orphan =
            valid_result(spec, config, coordinator.blocks[2]);
        store.record_result(orphan);
        orphan_path = store.block_result_path(orphan.block);
    }

    const mc::DurableRunResult recovered =
        mc::run_parallel_durable(spec, config, store_config);
    check(recovered.resumed && recovered.run_incarnation == 2U,
          "recovery did not durably advance the run incarnation");
    check(recovered.recovered_blocks == 2U &&
              recovered.computed_blocks == 6U,
          "recovery did not ignore the orphan or schedule exactly the missing set");
    check(recovered.recovered_scenarios == 1'024U &&
              recovered.computed_scenarios == 3'072U,
          "durable scenario accounting does not match block recovery");
    check(!std::filesystem::exists(orphan_path),
          "recovery did not apply the orphan-result retention policy");
    check_aggregate_exact(recovered.run_result.aggregate, clean.aggregate,
                          "clean and recovered fixed-tree aggregates differ");

    const mc::DurableRunResult completed_restart =
        mc::run_parallel_durable(spec, config, store_config);
    check(completed_restart.recovered_blocks == 8U &&
              completed_restart.computed_blocks == 0U &&
              completed_restart.recovered_scenarios == 4'096U &&
              completed_restart.computed_scenarios == 0U &&
              completed_restart.run_result.workers_used == 0U,
          "completed restart recomputed an already committed block");
    check(completed_restart.manifest_sequence == recovered.manifest_sequence &&
              completed_restart.run_incarnation == recovered.run_incarnation,
          "completed restart unnecessarily changed durable identity");
    check_aggregate_exact(completed_restart.run_result.aggregate,
                          clean.aggregate,
                          "completed restart changed the final aggregate");

    mc::RunSpec incompatible = spec;
    ++incompatible.global_seed;
    check_throws(
        [&] {
            static_cast<void>(mc::run_parallel_durable(
                incompatible, config, store_config));
        },
        "recovery accepted an incompatible run specification");
    mc::EngineConfig wrong_layout = config;
    wrong_layout.block_size *= 2U;
    check_throws(
        [&] {
            static_cast<void>(mc::run_parallel_durable(
                spec, wrong_layout, store_config));
        },
        "recovery accepted an incompatible execution layout");
}

void durable_corruption_falls_back_to_previous_manifest() {
    TemporaryDirectory directory;
    mc::RunSpec spec;
    spec.global_seed = 1234;
    spec.total_scenarios = 1'024;
    mc::EngineConfig config;
    config.worker_count = 2;
    config.block_size = 256;
    const mc::RunResult clean = mc::run_parallel(spec, config);
    mc::RunStoreConfig store_config;
    store_config.run_directory = directory.path();
    store_config.checkpoint_interval_blocks = 1;
    store_config.min_free_space_bytes = 0;
    std::filesystem::path latest_manifest;
    {
        mc::DurableRunStore store =
            mc::DurableRunStore::open(spec, config, store_config);
        const std::vector<mc::ScenarioBlock> blocks =
            store.recovery_state().blocks;
        mc::CoordinatorState coordinator =
            mc::make_coordinator_state(spec, config, blocks);
        std::vector<mc::AggregateStats> leaves(blocks.size());
        std::vector<bool> received(blocks.size(), false);
        commit_test_block(store, spec, config, 0, coordinator, leaves, received);
        store.checkpoint(coordinator, leaves, received);
        commit_test_block(store, spec, config, 1, coordinator, leaves, received);
        store.checkpoint(coordinator, leaves, received);
        latest_manifest =
            store.manifest_path(store.recovery_state().manifest_sequence);
    }
    flip_file_byte(latest_manifest, 32);

    const mc::DurableRunResult recovered =
        mc::run_parallel_durable(spec, config, store_config);
    check(recovered.recovered_blocks == 1U &&
              recovered.computed_blocks == 3U,
          "corrupt latest manifest did not fall back to the prior snapshot");
    check(recovered.manifest_sequence > 2U,
          "recovery attempted to overwrite the corrupt manifest sequence");
    check_aggregate_exact(recovered.run_result.aggregate, clean.aggregate,
                          "fallback recovery changed the final aggregate");
}

void durable_referenced_result_corruption_falls_back() {
    TemporaryDirectory directory;
    mc::RunSpec spec;
    spec.global_seed = 4321;
    spec.total_scenarios = 1'024;
    mc::EngineConfig config;
    config.worker_count = 2;
    config.block_size = 256;
    const mc::RunResult clean = mc::run_parallel(spec, config);
    mc::RunStoreConfig store_config;
    store_config.run_directory = directory.path();
    store_config.checkpoint_interval_blocks = 1;
    store_config.min_free_space_bytes = 0;
    std::filesystem::path second_result_path;
    {
        mc::DurableRunStore store =
            mc::DurableRunStore::open(spec, config, store_config);
        const std::vector<mc::ScenarioBlock> blocks =
            store.recovery_state().blocks;
        mc::CoordinatorState coordinator =
            mc::make_coordinator_state(spec, config, blocks);
        std::vector<mc::AggregateStats> leaves(blocks.size());
        std::vector<bool> received(blocks.size(), false);
        commit_test_block(store, spec, config, 0, coordinator, leaves, received);
        store.checkpoint(coordinator, leaves, received);
        commit_test_block(store, spec, config, 1, coordinator, leaves, received);
        store.checkpoint(coordinator, leaves, received);
        second_result_path = store.block_result_path(coordinator.blocks[1]);
    }
    flip_file_byte(second_result_path, 32);

    const mc::DurableRunResult recovered =
        mc::run_parallel_durable(spec, config, store_config);
    check(recovered.recovered_blocks == 1U &&
              recovered.computed_blocks == 3U,
          "corrupt referenced result did not invalidate its manifest snapshot");
    check_aggregate_exact(recovered.run_result.aggregate, clean.aggregate,
                          "result-corruption fallback changed the aggregate");
}

void durable_determinism_failure_is_persisted() {
    TemporaryDirectory directory;
    mc::RunSpec spec;
    spec.total_scenarios = 8;
    mc::EngineConfig config;
    config.block_size = 4;
    mc::RunStoreConfig store_config;
    store_config.run_directory = directory.path();
    store_config.min_free_space_bytes = 0;
    std::filesystem::path failed_manifest_path;
    {
        mc::DurableRunStore store =
            mc::DurableRunStore::open(spec, config, store_config);
        const std::vector<mc::ScenarioBlock> blocks =
            store.recovery_state().blocks;
        mc::CoordinatorState coordinator =
            mc::make_coordinator_state(spec, config, blocks);
        std::vector<mc::AggregateStats> leaves(blocks.size());
        std::vector<bool> received(blocks.size(), false);
        const mc::BlockResult original =
            valid_result(spec, config, coordinator.blocks[0]);
        store.record_result(original);
        check(mc::commit_result(original, coordinator).status ==
                  mc::ValidationStatus::Accepted,
              "original result did not enter the durable pending set");
        leaves[0] = original.aggregate;
        received[0] = true;

        mc::BlockResult conflict = original;
        conflict.aggregate.mean += 1.0;
        conflict.aggregate.min += 1.0;
        conflict.aggregate.max += 1.0;
        conflict.payload_checksum = mc::aggregate_payload_hash(
            conflict.aggregate, conflict.stats_schema_version);
        const mc::Validation validation =
            mc::validate_result(conflict, coordinator);
        check(validation.status == mc::ValidationStatus::DeterminismError,
              "conflicting result was not classified as a determinism failure");
        mc::FailureRecord failure;
        failure.status = validation.status;
        failure.block_id = conflict.block.block_id;
        failure.run_incarnation = conflict.block.run_incarnation;
        failure.lease_epoch = conflict.block.lease_epoch;
        failure.observed_checksum = conflict.payload_checksum;
        failure.committed_checksum = original.payload_checksum;
        failure.reason = validation.reason;
        store.checkpoint(coordinator, leaves, received,
                         mc::DurableRunStatus::Failed, failure);
        failed_manifest_path =
            store.manifest_path(store.recovery_state().manifest_sequence);
    }

    const mc::RunManifest failed =
        mc::decode_manifest(read_test_file(failed_manifest_path));
    check(failed.status == mc::DurableRunStatus::Failed &&
              failed.failure.has_value() &&
              failed.failure->status ==
                  mc::ValidationStatus::DeterminismError &&
              failed.failure->observed_checksum !=
                  failed.failure->committed_checksum,
          "failed manifest did not retain determinism diagnostics");
    check_throws(
        [&] {
            static_cast<void>(
                mc::run_parallel_durable(spec, config, store_config));
        },
        "runtime resumed a durably failed run");
}

void durable_storage_limits_fail_closed() {
    check(mc::kMinimumAutomaticCheckpointBlocks == 1'024U,
          "R4 automatic checkpoint floor decision drifted");
    TemporaryDirectory directory;
    mc::RunSpec spec;
    spec.total_scenarios = 8;
    mc::EngineConfig config;
    config.block_size = 4;
    mc::RunStoreConfig store_config;
    store_config.run_directory = directory.path();
    store_config.min_free_space_bytes = 0;
    store_config.max_storage_files = 1;
    check_throws(
        [&] {
            static_cast<void>(
                mc::DurableRunStore::open(spec, config, store_config));
        },
        "run store exceeded its configured file-count limit");

    TemporaryDirectory locked_directory;
    store_config = mc::RunStoreConfig{};
    store_config.run_directory = locked_directory.path();
    store_config.min_free_space_bytes = 0;
    mc::DurableRunStore owner =
        mc::DurableRunStore::open(spec, config, store_config);
    check(owner.config().checkpoint_interval_blocks ==
              mc::kMinimumAutomaticCheckpointBlocks,
          "small runs did not resolve the automatic checkpoint cadence");
    check_throws(
        [&] {
            static_cast<void>(
                mc::DurableRunStore::open(spec, config, store_config));
        },
        "two coordinators acquired the same durable run directory");
}

void durable_policy_failure_preserves_committed_results() {
    TemporaryDirectory directory;
    mc::RunSpec spec;
    spec.global_seed = 99117U;
    spec.total_scenarios = 1'024U;
    mc::EngineConfig config;
    config.worker_count = 2U;
    config.block_size = 256U;
    mc::RunStoreConfig store_config;
    store_config.run_directory = directory.path();
    store_config.checkpoint_interval_blocks = 1U;
    store_config.min_free_space_bytes = 0U;

    const mc::DurableRunResult completed =
        mc::run_parallel_durable(spec, config, store_config);
    const std::filesystem::path block_directory =
        directory.path() / "block_results";
    std::vector<std::filesystem::path> committed_files;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(block_directory)) {
        if (entry.is_regular_file()) {
            committed_files.push_back(entry.path());
        }
    }
    check(committed_files.size() == completed.run_result.block_count,
          "completed test run did not publish one result per block");

    std::filesystem::path latest_manifest;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(directory.path() / "manifests")) {
        if (entry.is_regular_file() &&
            (latest_manifest.empty() ||
             entry.path().filename() > latest_manifest.filename())) {
            latest_manifest = entry.path();
        }
    }
    check(!latest_manifest.empty(),
          "completed test run did not publish a manifest");
    const std::uintmax_t latest_size =
        std::filesystem::file_size(latest_manifest);
    check(latest_size > 256U,
          "test manifest is too small to exercise the operator policy limit");
    mc::RunStoreConfig restrictive = store_config;
    restrictive.max_manifest_bytes =
        static_cast<std::uint64_t>(latest_size - 1U);
    check_throws(
        [&] {
            static_cast<void>(
                mc::run_parallel_durable(spec, config, restrictive));
        },
        "operator manifest-size policy unexpectedly fell back to old state");
    for (const std::filesystem::path& path : committed_files) {
        check(std::filesystem::exists(path),
              "failed recovery policy deleted a committed result file");
    }

    const mc::DurableRunResult reopened =
        mc::run_parallel_durable(spec, config, store_config);
    check(reopened.recovered_blocks == completed.run_result.block_count &&
              reopened.computed_blocks == 0U,
          "normal recovery failed after a restrictive policy was rejected");
}

void durable_missing_metadata_is_not_reconstructed() {
    TemporaryDirectory directory;
    const std::filesystem::path run_directory = directory.path() / "run";
    mc::RunSpec spec;
    spec.global_seed = 778899U;
    spec.total_scenarios = 512U;
    mc::EngineConfig config;
    config.worker_count = 2U;
    config.block_size = 128U;
    mc::RunStoreConfig store_config;
    store_config.run_directory = run_directory;
    store_config.min_free_space_bytes = 0U;
    const mc::DurableRunResult completed =
        mc::run_parallel_durable(spec, config, store_config);

    const std::filesystem::path metadata = run_directory / "run_spec.bin";
    const std::filesystem::path backup = directory.path() / "run_spec.backup";
    std::filesystem::rename(metadata, backup);
    mc::RunSpec wrong_spec = spec;
    ++wrong_spec.global_seed;
    check_throws(
        [&] {
            static_cast<void>(mc::run_parallel_durable(
                wrong_spec, config, store_config));
        },
        "missing metadata was replaced using incompatible caller input");
    check(!std::filesystem::exists(metadata),
          "failed open poisoned the store with caller-supplied metadata");

    std::filesystem::rename(backup, metadata);
    const mc::DurableRunResult reopened =
        mc::run_parallel_durable(spec, config, store_config);
    check(reopened.recovered_blocks == completed.run_result.block_count &&
              reopened.computed_blocks == 0U,
          "restored authoritative metadata did not recover the completed run");
}

bool is_manifest_write_point(mc::FailurePoint point) {
    switch (point) {
        case mc::FailurePoint::ManifestBeforeFileFsync:
        case mc::FailurePoint::ManifestAfterFileFsync:
        case mc::FailurePoint::ManifestBeforeRename:
        case mc::FailurePoint::ManifestAfterRename:
            return true;
        case mc::FailurePoint::ResultBeforeFileFsync:
        case mc::FailurePoint::ResultAfterFileFsync:
        case mc::FailurePoint::ResultBeforeRename:
        case mc::FailurePoint::ResultAfterRename:
        case mc::FailurePoint::ManifestAfterInstallBeforeMemory:
            return false;
    }
    throw TestFailure("unknown R3 failure point in test");
}

void r3_failure_point_contract() {
    for (const mc::FailurePoint point : mc::kFailurePoints) {
        const std::string_view name = mc::failure_point_name(point);
        check(!name.empty() && name != "unknown" &&
                  mc::parse_failure_point(name) == point,
              "R3 failure-point name is not a stable round trip");
    }
    TemporaryDirectory directory;
    const mc::FailureInjectionConfig first =
        mc::failure_injection_from_seed(1234567U,
                                        directory.path() / "first.replay");
    const mc::FailureInjectionConfig second =
        mc::failure_injection_from_seed(1234567U,
                                        directory.path() / "second.replay");
    check(first.selected_point == second.selected_point &&
              first.selected_occurrence == second.selected_occurrence,
          "failure seed does not map to a deterministic crash schedule");
    check_throws(
        [] { static_cast<void>(mc::parse_failure_point("manifest.unknown")); },
        "failure-point parser accepted an unknown hook");

    mc::RunStoreConfig store_config =
        r3_test_store_config(directory.path() / "multi-worker");
    store_config.failure_injection = first;
    mc::EngineConfig unsupported = r3_test_engine_config();
    unsupported.worker_count = 2U;
    check_throws(
        [&] {
            static_cast<void>(mc::DurableRunStore::open(
                r3_test_spec(), unsupported, store_config));
        },
        "R3 accepted nondeterministic multi-worker trace injection");

    store_config = r3_test_store_config(directory.path() / "nested-replay");
    mc::FailureInjectionConfig nested = first;
    nested.replay_descriptor_path =
        store_config.run_directory / "failure.replay";
    store_config.failure_injection = nested;
    check_throws(
        [&] {
            static_cast<void>(mc::DurableRunStore::open(
                r3_test_spec(), r3_test_engine_config(), store_config));
        },
        "R3 allowed replay evidence to perturb the injected run directory");
}

void r3_replay_descriptor_hardening() {
    TemporaryDirectory directory;
    const std::filesystem::path replay_path =
        directory.path() / "immutable.replay";
    spawn_r3_crash_child(
        directory.path() / "first-run", replay_path,
        mc::FailurePoint::ResultBeforeFileFsync, 1U, 4001U);
    const std::vector<std::uint8_t> original = read_test_file(replay_path);
    check(!original.empty() &&
              mc::read_replay_descriptor(replay_path).version ==
                  mc::kReplayDescriptorVersion,
          "schema-v2 descriptor was not readable after atomic installation");

    std::vector<std::uint8_t> corrupted = original;
    corrupted[corrupted.size() / 2U] ^= 1U;
    const std::filesystem::path corrupted_path =
        directory.path() / "corrupted.replay";
    write_test_file(corrupted_path, corrupted);
    check_throws(
        [&] { static_cast<void>(mc::read_replay_descriptor(corrupted_path)); },
        "R3 accepted a descriptor whose canonical body no longer matched its hash");

    std::string noncanonical(original.begin(), original.end());
    const std::size_t version_offset = noncanonical.find("version=2\n");
    check(version_offset != std::string::npos,
          "schema-v2 descriptor omitted its version field");
    noncanonical.replace(version_offset, std::string("version=2\n").size(),
                         "version=02\n");
    const std::size_t checksum_offset =
        noncanonical.rfind("descriptor_sha256=");
    check(checksum_offset != std::string::npos,
          "schema-v2 descriptor omitted its record checksum");
    const std::string noncanonical_body =
        noncanonical.substr(0U, checksum_offset);
    const std::vector<std::uint8_t> noncanonical_body_bytes(
        noncanonical_body.begin(), noncanonical_body.end());
    noncanonical.replace(
        checksum_offset, std::string::npos,
        "descriptor_sha256=" +
            mc::to_hex(mc::sha256(noncanonical_body_bytes)) + "\n");
    const std::vector<std::uint8_t> noncanonical_bytes(
        noncanonical.begin(), noncanonical.end());
    const std::filesystem::path noncanonical_path =
        directory.path() / "noncanonical.replay";
    write_test_file(noncanonical_path, noncanonical_bytes);
    check_throws(
        [&] { static_cast<void>(mc::read_replay_descriptor(noncanonical_path)); },
        "R3 accepted noncanonical fields after a valid record checksum");

    const std::string legacy_text = "format=mc-r3-replay-v1\nversion=1\n";
    const std::vector<std::uint8_t> legacy_bytes(legacy_text.begin(),
                                                 legacy_text.end());
    const std::filesystem::path legacy_path =
        directory.path() / "legacy.replay";
    write_test_file(legacy_path, legacy_bytes);
    check_throws(
        [&] { static_cast<void>(mc::read_replay_descriptor(legacy_path)); },
        "R3 silently accepted an integrity-ambiguous schema-v1 descriptor");

    const std::filesystem::path oversized_path =
        directory.path() / "oversized.replay";
    const std::vector<std::uint8_t> oversized(
        mc::kMaxReplayDescriptorBytes + 1U, static_cast<std::uint8_t>('x'));
    write_test_file(oversized_path, oversized);
    check_throws(
        [&] { static_cast<void>(mc::read_replay_descriptor(oversized_path)); },
        "R3 accepted a replay descriptor beyond its input budget");

    std::string long_body =
        "x=" + std::string(mc::kMaxReplayDescriptorLineBytes, 'a') + "\n";
    const std::vector<std::uint8_t> long_body_bytes(long_body.begin(),
                                                    long_body.end());
    std::string long_descriptor =
        long_body + "descriptor_sha256=" +
        mc::to_hex(mc::sha256(long_body_bytes)) + "\n";
    const std::vector<std::uint8_t> long_descriptor_bytes(
        long_descriptor.begin(), long_descriptor.end());
    const std::filesystem::path long_line_path =
        directory.path() / "long-line.replay";
    write_test_file(long_line_path, long_descriptor_bytes);
    check_throws(
        [&] { static_cast<void>(mc::read_replay_descriptor(long_line_path)); },
        "R3 accepted a replay descriptor line beyond its input budget");

    mc::FailureInjectionConfig unsupported_scheduler;
    unsupported_scheduler.replay_descriptor_path =
        directory.path() / "scheduler.replay";
    unsupported_scheduler.deterministic_scheduler_seed = 1U;
    check_throws([&] { unsupported_scheduler.validate(); },
                 "R3 accepted a scheduler seed that schema v2 cannot execute");

    const int overwrite_exit = spawn_r3_crash_child_status(
        directory.path() / "second-run", replay_path,
        mc::FailurePoint::ManifestAfterRename, 1U, 4002U, true);
    check(overwrite_exit == 94 && read_test_file(replay_path) == original,
          "R3 overwrote immutable evidence when a replay path was reused");
}

void r3_watchdog_terminates_hung_child() {
    const pid_t child = ::fork();
    if (child < 0) {
        throw TestFailure("could not fork R3 watchdog test child");
    }
    if (child == 0) {
        for (;;) {
            static_cast<void>(::pause());
        }
    }
    const auto started = std::chrono::steady_clock::now();
    check_throws(
        [&] {
            static_cast<void>(mc::tool::wait_for_child(
                child, std::chrono::milliseconds{50},
                "intentional watchdog test hang"));
        },
        "R3 watchdog did not classify and terminate a hung child");
    check(std::chrono::steady_clock::now() - started <
              std::chrono::seconds{2},
          "R3 watchdog exceeded its bounded test deadline");
}

void r3_crash_matrix_recovers_exactly() {
    const mc::RunSpec spec = r3_test_spec();
    const mc::EngineConfig engine_config = r3_test_engine_config();
    const mc::RunResult clean = mc::run_parallel(spec, engine_config);

    for (const mc::FailurePoint point : mc::kFailurePoints) {
        TemporaryDirectory directory;
        const std::filesystem::path run_directory = directory.path() / "run";
        const std::filesystem::path replay_path =
            directory.path() / "crash.replay";
        // Occurrence two bypasses the empty initial manifest and crashes the
        // first manifest that commits a result. The post-install hook exists
        // only in DurableRunStore::checkpoint and therefore uses occurrence one.
        const std::uint64_t occurrence =
            is_manifest_write_point(point) ? 2U : 1U;
        const std::uint64_t failure_seed =
            0xA300U + static_cast<std::uint8_t>(point);
        spawn_r3_crash_child(run_directory, replay_path, point, occurrence,
                             failure_seed);

        const mc::ReplayDescriptor replay =
            mc::read_replay_descriptor(replay_path);
        check(replay.injection.selected_point == point &&
                  replay.injection.selected_occurrence == occurrence &&
                  replay.injection.failure_seed == failure_seed &&
                  replay.run_spec_hash == mc::run_spec_hash(spec) &&
                  replay.build_fingerprint ==
                      mc::current_build_identity().hash &&
                  replay.observed_trace_hash != mc::Sha256Digest{} &&
                  replay.observed_trace_events > 0U,
              "R3 replay descriptor omitted crash schedule identity");
        if (is_manifest_write_point(point) ||
            point == mc::FailurePoint::ManifestAfterInstallBeforeMemory) {
            check(replay.checkpoint_sequence != mc::kNoFailureContext &&
                      replay.block_id == mc::kNoFailureContext,
                  "manifest failure descriptor has invalid context");
        } else {
            check(replay.block_id != mc::kNoFailureContext &&
                      replay.checkpoint_sequence == mc::kNoFailureContext,
                  "result failure descriptor has invalid context");
        }

        mc::RunStoreConfig recovery_config =
            r3_test_store_config(run_directory);
        const mc::DurableRunResult recovered =
            mc::run_parallel_durable(spec, engine_config, recovery_config);
        check(recovered.recovered_blocks + recovered.computed_blocks ==
                  clean.block_count,
              "R3 recovery did not account for every block exactly once");
        check_aggregate_exact(
            recovered.run_result.aggregate, clean.aggregate,
            "R3 crash recovery differs from the clean fixed-tree result at " +
                std::string(mc::failure_point_name(point)));

        const mc::DurableRunResult reopened =
            mc::run_parallel_durable(spec, engine_config, recovery_config);
        check(reopened.recovered_blocks == clean.block_count &&
                  reopened.computed_blocks == 0U,
              "R3 completed restart recomputed committed work");
        check_aggregate_exact(
            reopened.run_result.aggregate, clean.aggregate,
            "R3 completed restart changed the recovered aggregate");
    }
}

void r3_trace_replay_is_deterministic() {
    std::array<mc::ReplayDescriptor, 2> descriptors;
    for (std::size_t index = 0; index < descriptors.size(); ++index) {
        TemporaryDirectory directory;
        const std::filesystem::path replay_path =
            directory.path() / "trace.replay";
        spawn_r3_crash_child(
            directory.path() / "run", replay_path,
            mc::FailurePoint::ManifestAfterInstallBeforeMemory, 1U, 7654321U);
        descriptors[index] = mc::read_replay_descriptor(replay_path);
    }
    check(descriptors[0].observed_trace_hash ==
              descriptors[1].observed_trace_hash &&
              descriptors[0].observed_trace_events ==
                  descriptors[1].observed_trace_events &&
              descriptors[0].checkpoint_sequence ==
                  descriptors[1].checkpoint_sequence,
          "identical R3 schedules did not reproduce the same trace");
}

void coordinator_validation_state_machine() {
    mc::RunSpec spec;
    spec.total_scenarios = 8;
    mc::EngineConfig config;
    config.block_size = 4;
    const std::vector<mc::ScenarioBlock> blocks = mc::make_blocks(spec, config);

    mc::CoordinatorState state = mc::make_coordinator_state(spec, config, blocks);
    const mc::BlockResult original = valid_result(spec, config, blocks[0]);
    check(mc::validate_result(original, state).status ==
              mc::ValidationStatus::Accepted,
          "valid result should be accepted");
    check(mc::commit_result(original, state).status ==
              mc::ValidationStatus::Accepted,
          "valid result should commit");
    check(mc::commit_result(original, state).status ==
              mc::ValidationStatus::Duplicate,
          "matching committed result should be idempotent");
    check(mc::is_benign_rejection(mc::ValidationStatus::Duplicate) &&
              mc::is_benign_rejection(
                  mc::ValidationStatus::StaleIncarnation) &&
              mc::is_benign_rejection(mc::ValidationStatus::StaleLease) &&
              !mc::is_benign_rejection(mc::ValidationStatus::Accepted) &&
              !mc::is_benign_rejection(
                  mc::ValidationStatus::CorruptPayload) &&
              !mc::is_benign_rejection(
                  mc::ValidationStatus::DeterminismError),
          "durable retry classification confuses benign and terminal results");

    mc::BlockResult conflict = original;
    conflict.aggregate.mean += 1.0;
    conflict.aggregate.min += 1.0;
    conflict.aggregate.max += 1.0;
    conflict.payload_checksum = mc::aggregate_payload_hash(
        conflict.aggregate, conflict.stats_schema_version);
    check(mc::validate_result(conflict, state).status ==
              mc::ValidationStatus::DeterminismError,
          "conflicting committed payload should be a determinism error");

    mc::CoordinatorState fresh = mc::make_coordinator_state(spec, config, blocks);
    mc::BlockResult corrupt = original;
    corrupt.payload_checksum[0] ^= 0x01U;
    check(mc::validate_result(corrupt, fresh).status ==
              mc::ValidationStatus::CorruptPayload,
          "payload/checksum mismatch should be rejected as corruption");

    mc::BlockResult invalid_aggregate = original;
    invalid_aggregate.aggregate.m2 = -1.0;
    check(mc::validate_result(invalid_aggregate, fresh).status ==
              mc::ValidationStatus::InvalidAggregate,
          "mathematically invalid aggregate should be rejected before checksum use");

    mc::BlockResult stale_incarnation = original;
    stale_incarnation.block.run_incarnation += 1U;
    check(mc::validate_result(stale_incarnation, fresh).status ==
              mc::ValidationStatus::StaleIncarnation,
          "wrong run incarnation should be rejected");

    mc::BlockResult stale_lease = original;
    stale_lease.block.lease_epoch += 1U;
    check(mc::validate_result(stale_lease, fresh).status ==
              mc::ValidationStatus::StaleLease,
          "wrong lease epoch should be rejected");

    mc::BlockResult invalid_run = original;
    invalid_run.run_spec_hash[0] ^= 0xFFU;
    check(mc::validate_result(invalid_run, fresh).status ==
              mc::ValidationStatus::InvalidRun,
          "wrong run hash should be rejected");

    mc::BlockResult wrong_layout = original;
    wrong_layout.execution_layout_hash[0] ^= 0xFFU;
    check(mc::validate_result(wrong_layout, fresh).status ==
              mc::ValidationStatus::ExecutionLayoutMismatch,
          "wrong execution layout should be rejected");

    mc::BlockResult wrong_build = original;
    wrong_build.build_fingerprint[0] ^= 0xFFU;
    check(mc::validate_result(wrong_build, fresh).status ==
              mc::ValidationStatus::BuildMismatch,
          "wrong build fingerprint should be rejected");

    mc::BlockResult wrong_rng = original;
    wrong_rng.rng_version += 1U;
    check(mc::validate_result(wrong_rng, fresh).status ==
              mc::ValidationStatus::RngVersionMismatch,
          "wrong RNG version should be rejected");

    mc::BlockResult wrong_schema = original;
    wrong_schema.stats_schema_version += 1U;
    check(mc::validate_result(wrong_schema, fresh).status ==
              mc::ValidationStatus::StatsSchemaMismatch,
          "wrong statistics schema should be rejected");

    mc::BlockResult wrong_range = original;
    wrong_range.block.end_scenario -= 1U;
    check(mc::validate_result(wrong_range, fresh).status ==
              mc::ValidationStatus::InvalidBlock,
          "wrong block range should be rejected");

    mc::BlockResult wrong_count = original;
    wrong_count.aggregate.n -= 1U;
    wrong_count.payload_checksum = mc::aggregate_payload_hash(
        wrong_count.aggregate, wrong_count.stats_schema_version);
    check(mc::validate_result(wrong_count, fresh).status ==
              mc::ValidationStatus::InvalidAggregate,
          "wrong block observation count should be rejected");
}

void runtime_metrics_are_opt_in_and_result_neutral() {
    mc::RunSpec spec;
    spec.global_seed = 0x4D455452494353ULL;
    spec.total_scenarios = 4'096U;
    spec.num_time_steps = 8U;
    spec.payoff_type = mc::PayoffType::AsianCall;
    mc::EngineConfig config;
    config.worker_count = 4U;
    config.block_size = 256U;

    const mc::RunResult baseline = mc::run_parallel(spec, config);
    mc::RuntimeMetrics metrics;
    const mc::RunResult observed = mc::run_parallel(spec, config, &metrics);
    check_aggregate_exact(observed.aggregate, baseline.aggregate,
                          "metrics changed the deterministic aggregate");
    check(observed.block_count == baseline.block_count &&
              observed.workers_used == baseline.workers_used,
          "metrics changed the execution result metadata");
    check(metrics.workers.size() == observed.workers_used,
          "metrics did not create one record per active worker");
    check(metrics.block_compute_ns.size() == observed.block_count &&
              metrics.result_persist_ns.size() == observed.block_count &&
              metrics.block_commit_ns.size() == observed.block_count,
          "metrics vectors do not match the deterministic block universe");

    std::uint64_t completed_blocks = 0U;
    std::uint64_t completed_scenarios = 0U;
    for (const mc::WorkerMetrics& worker : metrics.workers) {
        completed_blocks += worker.blocks_completed;
        completed_scenarios += worker.scenarios_completed;
    }
    check(completed_blocks == observed.block_count &&
              completed_scenarios == spec.total_scenarios,
          "per-worker metrics do not cover the executed block universe");
    check(std::all_of(metrics.block_compute_ns.begin(),
                      metrics.block_compute_ns.end(),
                      [](std::uint64_t sample) { return sample > 0U; }),
          "an executed block has no compute-latency sample");
    check(std::all_of(metrics.block_commit_ns.begin(),
                      metrics.block_commit_ns.end(),
                      [](std::uint64_t sample) { return sample > 0U; }),
          "an accepted block has no publication-to-acceptance sample");
    check(std::all_of(metrics.result_persist_ns.begin(),
                      metrics.result_persist_ns.end(),
                      [](std::uint64_t sample) { return sample == 0U; }) &&
              metrics.checkpoint_ns.empty(),
          "non-durable execution reported durable latency samples");
    check(metrics.durable_io.bytes_written == 0U &&
              metrics.durable_io.result_files_installed == 0U,
          "non-durable execution reported durable I/O");

    const std::optional<std::uint64_t> p50 =
        mc::latency_percentile_ns(metrics.block_compute_ns, 0.50);
    const std::optional<std::uint64_t> p95 =
        mc::latency_percentile_ns(metrics.block_compute_ns, 0.95);
    const std::optional<std::uint64_t> p99 =
        mc::latency_percentile_ns(metrics.block_compute_ns, 0.99);
    check(p50.has_value() && p95.has_value() && p99.has_value() &&
              *p50 <= *p95 && *p95 <= *p99,
          "block latency percentiles are absent or non-monotonic");
    check(metrics.max_assignment_queue_depth > 0U &&
              metrics.max_assignment_queue_depth <=
                  observed.workers_used * 2U &&
              metrics.max_completion_queue_depth > 0U &&
              metrics.max_completion_queue_depth <=
                  observed.workers_used * 2U,
          "observed queue peak exceeds the resolved bounded capacity");
    check(metrics.max_reduction_backlog_blocks == observed.block_count &&
              metrics.max_reduction_backlog_bytes ==
                  observed.block_count * sizeof(mc::AggregateStats),
          "fixed-tree backlog metrics do not cover every accepted leaf");
    check(metrics.total_elapsed_ns > 0U &&
              metrics.fixed_tree_reduce_ns > 0U,
          "top-level metrics did not record elapsed time");

    const std::vector<std::uint64_t> no_samples{0U, 0U};
    check(!mc::latency_percentile_ns(no_samples, 0.50).has_value(),
          "zero sentinels were treated as latency observations");
    check_throws(
        [&] {
            static_cast<void>(mc::latency_percentile_ns(
                metrics.block_compute_ns,
                std::numeric_limits<double>::quiet_NaN()));
        },
        "latency percentile accepted a non-finite quantile");
}

void bounded_queue_reports_only_actual_blocking() {
    mc::BoundedQueue<int> queue(1U);
    std::uint64_t push_wait_ns = 999U;
    check(queue.push(1, &push_wait_ns) && push_wait_ns == 0U,
          "an immediately available queue push reported blocking");

    int value = 0;
    std::uint64_t pop_wait_ns = 999U;
    check(queue.pop(value, &pop_wait_ns) && value == 1 &&
              pop_wait_ns == 0U,
          "an immediately available queue pop reported blocking");

    std::uint64_t blocked_pop_ns = 0U;
    std::thread producer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds{25});
        static_cast<void>(queue.push(2));
    });
    check(queue.pop(value, &blocked_pop_ns) && value == 2,
          "blocked queue pop did not receive the produced value");
    producer.join();
    check(blocked_pop_ns > 0U,
          "a condition-variable wait was not reported as blocking");

    queue.close();
    pop_wait_ns = 999U;
    check(!queue.pop(value, &pop_wait_ns) && pop_wait_ns == 0U,
          "an already closed queue reported a blocking wait");
}

void direct_store_metrics_cover_open_and_preallocate() {
    TemporaryDirectory directory;
    mc::RunSpec spec;
    spec.total_scenarios = 512U;
    mc::EngineConfig config;
    config.worker_count = 3U;
    config.block_size = 64U;
    mc::RunStoreConfig store_config;
    store_config.run_directory = directory.path();
    store_config.checkpoint_interval_blocks = 2U;
    store_config.min_free_space_bytes = 0U;

    mc::RuntimeMetrics metrics;
    metrics.durable_open_ns = 999U;
    metrics.block_compute_ns = {999U};
    {
        mc::DurableRunStore store = mc::DurableRunStore::open(
            spec, config, store_config, &metrics);
        check(metrics.durable_open_ns > 0U,
              "direct durable-store open omitted its elapsed time");
        check(metrics.block_compute_ns.size() == 8U &&
                  metrics.result_persist_ns.size() == 8U &&
                  metrics.block_commit_ns.size() == 8U,
              "direct durable-store open did not reset the block universe");
        check(metrics.workers.empty() && metrics.workers.capacity() >= 3U,
              "worker metrics were not preallocated before durable writes");
        check(metrics.checkpoint_ns.empty() &&
                  metrics.checkpoint_ns.capacity() >= 4U,
              "checkpoint metrics were not preallocated before durable writes");
        check(metrics.durable_io.metadata_files_installed == 1U &&
                  metrics.durable_io.manifest_files_installed == 1U,
              "direct durable-store open did not count installed metadata");

        const std::vector<mc::ScenarioBlock> blocks =
            store.recovery_state().blocks;
        mc::CoordinatorState coordinator =
            mc::make_coordinator_state(spec, config, blocks);
        std::vector<mc::AggregateStats> leaves(blocks.size());
        std::vector<bool> received(blocks.size(), false);
        commit_test_block(store, spec, config, 0U, coordinator,
                          leaves, received);
        store.checkpoint(coordinator, leaves, received);
        check(metrics.result_persist_ns[0] > 0U &&
                  metrics.checkpoint_ns.size() == 1U &&
                  metrics.checkpoint_samples_dropped == 0U,
              "direct store operations omitted latency samples");

        // Direct store users may checkpoint more often than the configured
        // cadence. Once the preallocated sample budget is full, observability
        // must degrade without allocating or failing after durable commit.
        for (std::size_t index = 0;
             index < metrics.checkpoint_ns.capacity(); ++index) {
            store.checkpoint(coordinator, leaves, received);
        }
        check(metrics.checkpoint_ns.size() ==
                  metrics.checkpoint_ns.capacity() &&
                  metrics.checkpoint_samples_dropped == 1U,
              "excess direct checkpoints did not report a dropped sample");
    }

    mc::RunStoreConfig invalid_store_config;
    metrics.durable_open_ns = 999U;
    metrics.block_compute_ns = {999U};
    check_throws(
        [&] {
            static_cast<void>(mc::DurableRunStore::open(
                spec, config, invalid_store_config, &metrics));
        },
        "invalid durable-store configuration unexpectedly opened");
    check(metrics.durable_open_ns > 0U &&
              metrics.block_compute_ns.empty(),
          "failed direct open retained stale metrics or omitted elapsed time");
}

void durable_metrics_capture_only_new_io() {
    TemporaryDirectory directory;
    mc::RunSpec spec;
    spec.global_seed = 0x44555241424C45ULL;
    spec.total_scenarios = 1'024U;
    spec.num_time_steps = 4U;
    mc::EngineConfig config;
    config.worker_count = 2U;
    config.block_size = 128U;
    mc::RunStoreConfig store_config;
    store_config.run_directory = directory.path();
    store_config.checkpoint_interval_blocks = 2U;
    store_config.min_free_space_bytes = 0U;

    const mc::RunResult baseline = mc::run_parallel(spec, config);
    mc::RuntimeMetrics first_metrics;
    const mc::DurableRunResult first = mc::run_parallel_durable(
        spec, config, store_config, &first_metrics);
    check_aggregate_exact(first.run_result.aggregate, baseline.aggregate,
                          "durable metrics changed the deterministic result");
    check(first.computed_blocks == 8U && first.recovered_blocks == 0U,
          "fresh durable metrics test did not compute every block");
    check(first_metrics.durable_io.metadata_files_installed == 1U &&
              first_metrics.durable_io.result_files_installed == 8U &&
              first_metrics.durable_io.manifest_files_installed == 5U,
          "durable metrics file counts do not match the checkpoint policy");
    check(first_metrics.durable_io.bytes_written > 0U &&
              first_metrics.checkpoint_ns.size() == 4U,
          "durable metrics omitted installed bytes or checkpoints");
    check(mc::latency_percentile_ns(
              first_metrics.result_persist_ns, 0.95).has_value() &&
              mc::latency_percentile_ns(
                  first_metrics.checkpoint_ns, 0.95).has_value(),
          "durable latency samples are absent");

    mc::RuntimeMetrics restart_metrics;
    const mc::DurableRunResult restart = mc::run_parallel_durable(
        spec, config, store_config, &restart_metrics);
    check_aggregate_exact(restart.run_result.aggregate, baseline.aggregate,
                          "metrics-enabled completed restart changed the result");
    check(restart.computed_blocks == 0U && restart.recovered_blocks == 8U &&
              restart.run_result.workers_used == 0U,
          "completed restart performed unnecessary computation");
    check(restart_metrics.workers.empty() &&
              restart_metrics.checkpoint_ns.empty() &&
              restart_metrics.durable_io.metadata_files_installed == 0U &&
              restart_metrics.durable_io.result_files_installed == 0U &&
              restart_metrics.durable_io.manifest_files_installed == 0U &&
              restart_metrics.durable_io.bytes_written == 0U,
          "completed restart reported files that it did not install");
    check(std::all_of(restart_metrics.block_commit_ns.begin(),
                      restart_metrics.block_commit_ns.end(),
                      [](std::uint64_t sample) { return sample == 0U; }) &&
              restart_metrics.max_reduction_backlog_blocks == 8U,
          "completed restart misreported acceptance latency or leaf backlog");
    check(!mc::latency_percentile_ns(
               restart_metrics.block_compute_ns, 0.50).has_value() &&
              !mc::latency_percentile_ns(
                  restart_metrics.result_persist_ns, 0.50).has_value(),
          "recovered blocks were misreported as newly computed or persisted");
}

void block_universe_and_resource_preflight() {
    mc::RunSpec spec;
    spec.total_scenarios = 100;
    mc::EngineConfig config;
    config.block_size = 10;
    std::vector<mc::ScenarioBlock> blocks = mc::make_blocks(spec, config);

    std::vector<mc::ScenarioBlock> malformed = blocks;
    malformed[1].block_id = 99;
    check_throws(
        [&] {
            static_cast<void>(
                mc::make_coordinator_state(spec, config, std::move(malformed)));
        },
        "coordinator accepted a noncanonical block ID");

    malformed = blocks;
    malformed[1].run_incarnation += 1U;
    check_throws(
        [&] {
            static_cast<void>(
                mc::make_coordinator_state(spec, config, std::move(malformed)));
        },
        "coordinator accepted mixed run incarnations");

    mc::EngineConfig limited = config;
    limited.block_size = 1;
    limited.max_materialized_blocks = 10;
    check_throws([&] { static_cast<void>(mc::make_blocks(spec, limited)); },
                 "block materialization safety limit was ignored");

    limited = config;
    limited.worker_count = mc::kMaxWorkerThreads + 1U;
    check_throws([&] { static_cast<void>(mc::make_blocks(spec, limited)); },
                 "worker thread safety limit was ignored");

    mc::RunSpec tiny;
    tiny.total_scenarios = 1;
    mc::EngineConfig excessive_workers;
    excessive_workers.worker_count = 8;
    const mc::RunResult tiny_result = mc::run_parallel(tiny, excessive_workers);
    check(tiny_result.workers_used == 1U,
          "worker count was not capped at the number of blocks");
}

void numerical_parameter_guards() {
    mc::RunSpec extreme;
    extreme.rate = -1.0e308;
    extreme.maturity = 1.0e308;
    check_throws([&] { static_cast<void>(mc::GbmKernel(extreme)); },
                 "non-finite derived GBM constants were accepted");
    check_throws(
        [] {
            static_cast<void>(mc::black_scholes_call_price(
                std::numeric_limits<double>::infinity(), 100.0, 0.05, 0.2, 1.0));
        },
        "Black-Scholes accepted an infinite input");

    mc::RunSpec heston = heston_test_spec();
    heston.rate = 0.0;
    heston.maturity = 2.0;
    heston.num_time_steps = 1U;
    heston.heston->initial_variance =
        std::numeric_limits<double>::max();
    check_throws([&] { static_cast<void>(mc::HestonKernel(heston)); },
                 "Heston accepted an overflowing initial time-step scale");
}

void golden_block_payloads() {
    mc::RunSpec european;
    european.global_seed = 0x123456789ABCDEF0ULL;
    european.total_scenarios = 512;
    european.num_time_steps = 8;
    mc::EngineConfig config;
    config.block_size = 512;
    const mc::ScenarioBlock block = mc::make_blocks(european, config).front();
    const std::string european_hash = mc::to_hex(mc::aggregate_payload_hash(
        mc::compute_block(european, block), european.stats_schema_version));
    check(european_hash ==
              "10fec6585672f4c406238e420b3f4e2b"
              "3b9aadbb4320dff043fa208914aa3297",
          "European golden block payload changed: " + european_hash);

    mc::RunSpec asian = european;
    asian.payoff_type = mc::PayoffType::AsianCall;
    const std::string asian_hash = mc::to_hex(mc::aggregate_payload_hash(
        mc::compute_block(asian, block), asian.stats_schema_version));
    check(asian_hash ==
              "ace248a29e3767658bbb3ab6295276cb"
              "73caf8d57be6f0f63144b3071e534b1b",
          "Asian golden block payload changed: " + asian_hash);
}

void validation_rejects_split_antithetic_pairs() {
    mc::RunSpec odd_scenarios;
    odd_scenarios.total_scenarios = 101;
    odd_scenarios.antithetic = true;
    check_throws([&] { odd_scenarios.validate(); },
                 "odd antithetic scenario counts should be rejected");

    mc::RunSpec spec;
    spec.total_scenarios = 100;
    spec.antithetic = true;
    mc::EngineConfig config;
    config.block_size = 15;
    check_throws([&] { static_cast<void>(mc::make_blocks(spec, config)); },
                 "odd antithetic block sizes should be rejected");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::string_view(argv[1]) == "--r3-crash-child") {
        return run_r3_crash_child(argc, argv);
    }
    test_executable = mc::tool::resolve_executable_path(argv[0]);
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"philox_known_answer", philox_known_answer},
        {"counter_layout_v1", counter_layout_v1},
        {"uniform_mapping_v2_boundaries", uniform_mapping_v2_boundaries},
        {"inverse_normal_values", inverse_normal_values},
        {"sha256_known_answers", sha256_known_answers},
        {"canonical_run_spec_hashing", canonical_run_spec_hashing},
        {"strict_numeric_parsing", strict_numeric_parsing},
        {"aggregate_and_merge", aggregate_and_merge},
        {"small_sample_and_aggregate_invariants",
         small_sample_and_aggregate_invariants},
        {"black_scholes_oracle", black_scholes_oracle},
        {"deterministic_across_worker_counts", deterministic_across_worker_counts},
        {"monte_carlo_converges_to_black_scholes", monte_carlo_converges_to_black_scholes},
        {"antithetic_pair_means_reduce_error", antithetic_pair_means_reduce_error},
        {"fused_antithetic_pair_preserves_estimator",
         fused_antithetic_pair_preserves_estimator},
        {"heston_run_spec_identity_and_warnings",
         heston_run_spec_identity_and_warnings},
        {"heston_rng_dimension_smoke", heston_rng_dimension_smoke},
        {"heston_analytic_quantlib_grid", heston_analytic_quantlib_grid},
        {"heston_analytic_limits_and_guards",
         heston_analytic_limits_and_guards},
        {"heston_constant_variance_matches_gbm_limit",
         heston_constant_variance_matches_gbm_limit},
        {"heston_determinism_and_antithetics",
         heston_determinism_and_antithetics},
        {"heston_durable_recovery_round_trip",
         heston_durable_recovery_round_trip},
        {"heston_descriptor_driven_crash_replay",
         heston_descriptor_driven_crash_replay},
        {"durable_codec_contract", durable_codec_contract},
        {"durable_recovery_exactly_once", durable_recovery_exactly_once},
        {"durable_corruption_falls_back_to_previous_manifest",
         durable_corruption_falls_back_to_previous_manifest},
        {"durable_referenced_result_corruption_falls_back",
         durable_referenced_result_corruption_falls_back},
        {"durable_determinism_failure_is_persisted",
         durable_determinism_failure_is_persisted},
        {"durable_storage_limits_fail_closed",
         durable_storage_limits_fail_closed},
        {"durable_policy_failure_preserves_committed_results",
         durable_policy_failure_preserves_committed_results},
        {"durable_missing_metadata_is_not_reconstructed",
         durable_missing_metadata_is_not_reconstructed},
        {"r3_failure_point_contract", r3_failure_point_contract},
        {"r3_replay_descriptor_hardening",
         r3_replay_descriptor_hardening},
        {"r3_watchdog_terminates_hung_child",
         r3_watchdog_terminates_hung_child},
        {"r3_crash_matrix_recovers_exactly",
         r3_crash_matrix_recovers_exactly},
        {"r3_trace_replay_is_deterministic",
         r3_trace_replay_is_deterministic},
        {"coordinator_validation_state_machine",
         coordinator_validation_state_machine},
        {"runtime_metrics_are_opt_in_and_result_neutral",
         runtime_metrics_are_opt_in_and_result_neutral},
        {"bounded_queue_reports_only_actual_blocking",
         bounded_queue_reports_only_actual_blocking},
        {"direct_store_metrics_cover_open_and_preallocate",
         direct_store_metrics_cover_open_and_preallocate},
        {"durable_metrics_capture_only_new_io",
         durable_metrics_capture_only_new_io},
        {"block_universe_and_resource_preflight",
         block_universe_and_resource_preflight},
        {"numerical_parameter_guards", numerical_parameter_guards},
        {"golden_block_payloads", golden_block_payloads},
        {"validation_rejects_split_antithetic_pairs",
         validation_rejects_split_antithetic_pairs},
    };

    std::size_t failures = 0;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "[PASS] " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[FAIL] " << name << ": " << error.what() << '\n';
        }
    }

    std::cout << (tests.size() - failures) << '/' << tests.size()
              << " tests passed\n";
    return failures == 0 ? 0 : 1;
}
