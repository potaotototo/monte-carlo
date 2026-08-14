#include "mc/failure_injection.hpp"

#include "failure_injection_internal.hpp"

#include "mc/codec.hpp"
#include "mc/identity.hpp"
#include "mc/parse.hpp"
#include "mc/persistence.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace mc {
namespace {

constexpr std::string_view kReplayMagic = "mc-r3-replay-v2";
constexpr std::string_view kReplayV1Prefix = "format=mc-r3-replay-v1\n";
constexpr std::size_t kMaxReplayDescriptorFields = 64U;

bool path_is_within(const std::filesystem::path& candidate,
                    const std::filesystem::path& directory) {
    const std::filesystem::path normalized_candidate =
        std::filesystem::weakly_canonical(candidate);
    const std::filesystem::path normalized_directory =
        std::filesystem::weakly_canonical(directory);
    auto candidate_part = normalized_candidate.begin();
    for (auto directory_part = normalized_directory.begin();
         directory_part != normalized_directory.end();
         ++directory_part, ++candidate_part) {
        if (candidate_part == normalized_candidate.end() ||
            *candidate_part != *directory_part) {
            return false;
        }
    }
    return true;
}

std::runtime_error io_error(const std::string& operation,
                            const std::filesystem::path& path,
                            int error_number = errno) {
    return std::runtime_error(operation + " " + path.string() + ": " +
                              std::strerror(error_number));
}

void append_u64(std::array<std::uint8_t, 25>& bytes,
                std::size_t& offset,
                std::uint64_t value) {
    for (std::uint32_t shift = 0; shift < 64U; shift += 8U) {
        bytes[offset++] = static_cast<std::uint8_t>(value >> shift);
    }
}

std::array<std::uint8_t, 25> encode_trace_event(
    FailurePoint point,
    const FailureContext& context) {
    std::array<std::uint8_t, 25> bytes{};
    std::size_t offset = 0U;
    bytes[offset++] = static_cast<std::uint8_t>(point);
    append_u64(bytes, offset, context.run_incarnation);
    append_u64(bytes, offset, context.block_id);
    append_u64(bytes, offset, context.checkpoint_sequence);
    return bytes;
}

std::string bits_hex(double value) {
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::hex << std::setw(16) << std::setfill('0')
           << std::bit_cast<std::uint64_t>(value);
    return stream.str();
}

std::uint64_t parse_hex_u64(std::string_view text, std::string_view field) {
    if (text.size() != 16U) {
        throw std::runtime_error(std::string(field) +
                                 " must contain 16 hexadecimal digits");
    }
    std::uint64_t value = 0;
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), value, 16);
    if (error != std::errc{} || end != text.data() + text.size()) {
        throw std::runtime_error(std::string(field) +
                                 " is not canonical hexadecimal");
    }
    return value;
}

std::uint8_t parse_u8(std::string_view text, std::string_view field) {
    const std::uint64_t value = parse_u64(text, field);
    if (value > std::numeric_limits<std::uint8_t>::max()) {
        throw std::runtime_error(std::string(field) + " exceeds uint8");
    }
    return static_cast<std::uint8_t>(value);
}

std::uint32_t parse_u32(std::string_view text, std::string_view field) {
    const std::uint64_t value = parse_u64(text, field);
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error(std::string(field) + " exceeds uint32");
    }
    return static_cast<std::uint32_t>(value);
}

Sha256Digest parse_digest(std::string_view text, std::string_view field) {
    if (text.size() != 64U) {
        throw std::runtime_error(std::string(field) +
                                 " must contain 64 hexadecimal digits");
    }
    Sha256Digest digest{};
    for (std::size_t index = 0; index < digest.size(); ++index) {
        const std::string_view byte = text.substr(index * 2U, 2U);
        unsigned int value = 0;
        const auto [end, error] =
            std::from_chars(byte.data(), byte.data() + byte.size(), value, 16);
        if (error != std::errc{} || end != byte.data() + byte.size()) {
            throw std::runtime_error(std::string(field) +
                                     " is not canonical hexadecimal");
        }
        digest[index] = static_cast<std::uint8_t>(value);
    }
    return digest;
}

std::span<const std::uint8_t> byte_span(std::string_view text) noexcept {
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

std::string encode_descriptor_body(const ReplayDescriptor& descriptor) {
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << "format=" << kReplayMagic << '\n'
           << "version=" << descriptor.version << '\n'
           << "run_spec_hash=" << to_hex(descriptor.run_spec_hash) << '\n'
           << "build_fingerprint="
           << to_hex(descriptor.build_fingerprint) << '\n'
           << "engine_version=" << descriptor.spec.engine_version << '\n'
           << "rng_version=" << descriptor.spec.rng_version << '\n'
           << "stats_schema_version="
           << descriptor.spec.stats_schema_version << '\n'
           << "model_type="
           << static_cast<unsigned int>(descriptor.spec.model_type) << '\n'
           << "payoff_type="
           << static_cast<unsigned int>(descriptor.spec.payoff_type) << '\n'
           << "antithetic=" << (descriptor.spec.antithetic ? 1 : 0) << '\n'
           << "global_seed=" << descriptor.spec.global_seed << '\n'
           << "total_scenarios=" << descriptor.spec.total_scenarios << '\n'
           << "num_time_steps=" << descriptor.spec.num_time_steps << '\n'
           << "maturity_bits=" << bits_hex(descriptor.spec.maturity) << '\n'
           << "spot_bits=" << bits_hex(descriptor.spec.spot) << '\n'
           << "strike_bits=" << bits_hex(descriptor.spec.strike) << '\n'
           << "rate_bits=" << bits_hex(descriptor.spec.rate) << '\n';
    switch (descriptor.spec.model_type) {
        case ModelType::Gbm:
            stream << "volatility_bits="
                   << bits_hex(descriptor.spec.volatility) << '\n';
            break;
        case ModelType::Heston:
            stream << "heston_discretization_version="
                   << descriptor.spec.heston->discretization_version << '\n'
                   << "heston_initial_variance_bits="
                   << bits_hex(descriptor.spec.heston->initial_variance) << '\n'
                   << "heston_mean_reversion_rate_bits="
                   << bits_hex(descriptor.spec.heston->mean_reversion_rate) << '\n'
                   << "heston_long_run_variance_bits="
                   << bits_hex(descriptor.spec.heston->long_run_variance) << '\n'
                   << "heston_volatility_of_variance_bits="
                   << bits_hex(descriptor.spec.heston->volatility_of_variance)
                   << '\n'
                   << "heston_correlation_bits="
                   << bits_hex(descriptor.spec.heston->correlation) << '\n';
            break;
    }
    stream << "worker_count=" << descriptor.engine_config.worker_count << '\n'
           << "block_size=" << descriptor.engine_config.block_size << '\n'
           << "assignment_queue_capacity="
           << descriptor.engine_config.assignment_queue_capacity << '\n'
           << "completion_queue_capacity="
           << descriptor.engine_config.completion_queue_capacity << '\n'
           << "max_materialized_blocks="
           << descriptor.engine_config.max_materialized_blocks << '\n'
           << "checkpoint_interval_blocks="
           << descriptor.checkpoint_interval_blocks << '\n'
           << "max_storage_bytes=" << descriptor.max_storage_bytes << '\n'
           << "max_storage_files=" << descriptor.max_storage_files << '\n'
           << "min_free_space_bytes="
           << descriptor.min_free_space_bytes << '\n'
           << "max_manifest_bytes=" << descriptor.max_manifest_bytes << '\n'
           << "failure_seed=" << descriptor.injection.failure_seed << '\n'
           << "deterministic_scheduler_seed="
           << descriptor.injection.deterministic_scheduler_seed << '\n'
           << "selected_failure_point="
           << failure_point_name(descriptor.injection.selected_point) << '\n'
           << "selected_occurrence="
           << descriptor.injection.selected_occurrence << '\n'
           << "observed_trace_hash="
           << to_hex(descriptor.observed_trace_hash) << '\n'
           << "observed_trace_events="
           << descriptor.observed_trace_events << '\n'
           << "run_incarnation=" << descriptor.run_incarnation << '\n'
           << "block_id=" << descriptor.block_id << '\n'
           << "checkpoint_sequence="
           << descriptor.checkpoint_sequence << '\n';
    return stream.str();
}

std::string encode_descriptor(const ReplayDescriptor& descriptor) {
    std::string bytes = encode_descriptor_body(descriptor);
    const Sha256Digest body_hash = sha256(byte_span(bytes));
    bytes += "descriptor_sha256=";
    bytes += to_hex(body_hash);
    bytes.push_back('\n');
    return bytes;
}

void write_all(int descriptor,
               std::string_view bytes,
               const std::filesystem::path& path) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const std::size_t remaining = bytes.size() - offset;
        const std::size_t chunk = std::min<std::size_t>(
            remaining,
            static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
        const ssize_t written =
            ::write(descriptor, bytes.data() + offset, chunk);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw io_error("cannot write replay descriptor", path);
        }
        if (written == 0) {
            throw std::runtime_error("zero-length replay descriptor write");
        }
        offset += static_cast<std::size_t>(written);
    }
}

void sync_directory(const std::filesystem::path& path) {
    const int descriptor = ::open(path.c_str(), O_RDONLY);
    if (descriptor < 0) {
        throw io_error("cannot open replay directory", path);
    }
    if (::fsync(descriptor) != 0) {
        const int saved_error = errno;
        static_cast<void>(::close(descriptor));
        throw io_error("cannot fsync replay directory", path, saved_error);
    }
    if (::close(descriptor) != 0) {
        throw io_error("cannot close replay directory", path);
    }
}

void write_descriptor_atomic(const std::filesystem::path& path,
                             const ReplayDescriptor& descriptor) {
    if (path.empty() || path.filename().empty()) {
        throw std::invalid_argument("replay descriptor path is invalid");
    }
    const std::filesystem::path parent = path.parent_path().empty()
                                                 ? std::filesystem::path{"."}
                                                 : path.parent_path();
    std::error_code error;
    std::filesystem::create_directories(parent, error);
    if (error) {
        throw std::runtime_error("cannot create replay descriptor directory: " +
                                 error.message());
    }
    static std::atomic<std::uint64_t> nonce{0};
    std::filesystem::path temporary;
    int file = -1;
    for (std::uint32_t attempt = 0U; attempt < 64U; ++attempt) {
        temporary =
            parent / (path.filename().string() + ".tmp." +
                      std::to_string(static_cast<std::uint64_t>(::getpid())) +
                      "." + std::to_string(nonce.fetch_add(1U)));
        file = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (file >= 0 || errno != EEXIST) {
            break;
        }
    }
    if (file < 0) {
        throw io_error("cannot create replay descriptor", temporary);
    }
    try {
        const std::string bytes = encode_descriptor(descriptor);
        write_all(file, bytes, temporary);
        if (::fsync(file) != 0) {
            throw io_error("cannot fsync replay descriptor", temporary);
        }
        const int close_result = ::close(file);
        file = -1;
        if (close_result != 0) {
            throw io_error("cannot close replay descriptor", temporary);
        }
        if (::link(temporary.c_str(), path.c_str()) != 0) {
            if (errno == EEXIST) {
                throw std::runtime_error(
                    "replay descriptor already exists and is immutable: " +
                    path.string());
            }
            throw io_error("cannot install replay descriptor", path);
        }
        if (::unlink(temporary.c_str()) != 0) {
            throw io_error("cannot remove installed replay temporary", temporary);
        }
        sync_directory(parent);
    } catch (...) {
        if (file >= 0) {
            static_cast<void>(::close(file));
        }
        std::error_code ignored;
        static_cast<void>(std::filesystem::remove(temporary, ignored));
        throw;
    }
}

std::string read_descriptor_bytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("cannot open replay descriptor: " +
                                 path.string());
    }
    std::string bytes;
    bytes.reserve(4096U);
    std::array<char, 4096> buffer{};
    while (stream) {
        stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = stream.gcount();
        if (count < 0) {
            throw std::runtime_error("cannot read replay descriptor");
        }
        const std::size_t count_size = static_cast<std::size_t>(count);
        if (count_size > kMaxReplayDescriptorBytes - bytes.size()) {
            throw std::length_error("replay descriptor exceeds 64 KiB limit");
        }
        bytes.append(buffer.data(), count_size);
    }
    if (!stream.eof()) {
        throw std::runtime_error("cannot read complete replay descriptor");
    }
    if (bytes.empty() || bytes.back() != '\n') {
        throw std::runtime_error(
            "replay descriptor must be nonempty and newline-terminated");
    }
    return bytes;
}

std::vector<std::pair<std::string, std::string>> read_fields(
    std::string_view bytes) {
    std::vector<std::pair<std::string, std::string>> fields;
    std::size_t offset = 0U;
    while (offset < bytes.size()) {
        const std::size_t newline = bytes.find('\n', offset);
        if (newline == std::string_view::npos) {
            throw std::runtime_error(
                "replay descriptor must be newline-terminated");
        }
        const std::string_view line = bytes.substr(offset, newline - offset);
        if (line.size() > kMaxReplayDescriptorLineBytes) {
            throw std::length_error(
                "replay descriptor line exceeds 4 KiB limit");
        }
        const std::size_t separator = line.find('=');
        if (separator == std::string::npos || separator == 0U) {
            throw std::runtime_error("malformed replay descriptor line");
        }
        if (fields.size() == kMaxReplayDescriptorFields) {
            throw std::length_error(
                "replay descriptor exceeds the 64-field schema limit");
        }
        fields.emplace_back(std::string(line.substr(0U, separator)),
                            std::string(line.substr(separator + 1U)));
        offset = newline + 1U;
    }
    return fields;
}

Sha256Digest verify_descriptor_checksum(std::string_view bytes) {
    constexpr std::string_view prefix = "descriptor_sha256=";
    const std::size_t previous_newline =
        bytes.size() > 1U ? bytes.rfind('\n', bytes.size() - 2U)
                          : std::string_view::npos;
    const std::size_t final_line_start =
        previous_newline == std::string_view::npos ? 0U : previous_newline + 1U;
    const std::string_view final_line = bytes.substr(
        final_line_start, bytes.size() - final_line_start - 1U);
    if (!final_line.starts_with(prefix)) {
        throw std::runtime_error(
            "replay descriptor is missing its final SHA-256 checksum");
    }
    const Sha256Digest recorded = parse_digest(
        final_line.substr(prefix.size()), "descriptor_sha256");
    if (sha256(byte_span(bytes.substr(0U, final_line_start))) != recorded) {
        throw std::runtime_error("replay descriptor SHA-256 mismatch");
    }
    return recorded;
}

class FieldReader {
public:
    explicit FieldReader(
        const std::vector<std::pair<std::string, std::string>>& fields)
        : fields_(fields) {}

    std::string_view take(std::string_view expected_name) {
        if (offset_ >= fields_.size() ||
            fields_[offset_].first != expected_name) {
            throw std::runtime_error("replay descriptor field order/name mismatch: " +
                                     std::string(expected_name));
        }
        return fields_[offset_++].second;
    }

    void finish() const {
        if (offset_ != fields_.size()) {
            throw std::runtime_error("replay descriptor has trailing fields");
        }
    }

private:
    const std::vector<std::pair<std::string, std::string>>& fields_;
    std::size_t offset_ = 0;
};

}  // namespace

std::string_view failure_point_name(FailurePoint point) noexcept {
    switch (point) {
        case FailurePoint::ResultBeforeFileFsync:
            return "result.before_file_fsync";
        case FailurePoint::ResultAfterFileFsync:
            return "result.after_file_fsync";
        case FailurePoint::ResultBeforeRename:
            return "result.before_rename";
        case FailurePoint::ResultAfterRename:
            return "result.after_rename";
        case FailurePoint::ManifestBeforeFileFsync:
            return "manifest.before_file_fsync";
        case FailurePoint::ManifestAfterFileFsync:
            return "manifest.after_file_fsync";
        case FailurePoint::ManifestBeforeRename:
            return "manifest.before_rename";
        case FailurePoint::ManifestAfterRename:
            return "manifest.after_rename";
        case FailurePoint::ManifestAfterInstallBeforeMemory:
            return "manifest.after_install_before_memory";
    }
    return "unknown";
}

FailurePoint parse_failure_point(std::string_view name) {
    for (const FailurePoint point : kFailurePoints) {
        if (name == failure_point_name(point)) {
            return point;
        }
    }
    throw std::invalid_argument("unknown R3 failure point: " +
                                std::string(name));
}

void FailureInjectionConfig::validate() const {
    static_cast<void>(parse_failure_point(failure_point_name(selected_point)));
    if (selected_occurrence == 0U) {
        throw std::invalid_argument("failure occurrence must be positive");
    }
    if (replay_descriptor_path.empty()) {
        throw std::invalid_argument("failure injection requires a replay path");
    }
    if (deterministic_scheduler_seed != 0U) {
        throw std::invalid_argument(
            "replay schema v2 requires deterministic_scheduler_seed=0 until "
            "multi-worker scheduling replay is implemented");
    }
}

FailureInjectionConfig failure_injection_from_seed(
    std::uint64_t failure_seed,
    std::filesystem::path replay_descriptor_path) {
    // SplitMix64 gives a stable seed-to-schedule mapping without hidden global
    // PRNG state. The systematic matrix constructs enough blocks/checkpoints
    // for both occurrences to be reachable under every generated topology.
    std::uint64_t mixed = failure_seed + 0x9E3779B97F4A7C15ULL;
    mixed = (mixed ^ (mixed >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    mixed = (mixed ^ (mixed >> 27U)) * 0x94D049BB133111EBULL;
    mixed ^= mixed >> 31U;
    FailureInjectionConfig config;
    config.selected_point =
        kFailurePoints[static_cast<std::size_t>(mixed % kFailurePoints.size())];
    config.selected_occurrence = 1U + ((mixed >> 8U) & 1U);
    config.failure_seed = failure_seed;
    config.replay_descriptor_path = std::move(replay_descriptor_path);
    return config;
}

FailureInjector::FailureInjector(
    const RunSpec& spec,
    const EngineConfig& engine_config,
    const RunStoreConfig& store_config) {
    if (!store_config.failure_injection.has_value()) {
        return;
    }
    if (engine_config.worker_count != 1U) {
        throw std::invalid_argument(
            "deterministic failure replay currently requires one worker");
    }
    store_config.failure_injection->validate();
    if (path_is_within(
            store_config.failure_injection->replay_descriptor_path,
            store_config.run_directory)) {
        throw std::invalid_argument(
            "replay descriptor must be outside the injected run directory");
    }
    std::error_code replay_error;
    if (std::filesystem::exists(
            store_config.failure_injection->replay_descriptor_path,
            replay_error)) {
        throw std::invalid_argument(
            "replay descriptor already exists and is immutable: " +
            store_config.failure_injection->replay_descriptor_path.string());
    }
    if (replay_error) {
        throw std::runtime_error("cannot inspect replay descriptor path: " +
                                 replay_error.message());
    }
    descriptor_.spec = spec;
    descriptor_.engine_config = engine_config;
    descriptor_.checkpoint_interval_blocks =
        store_config.checkpoint_interval_blocks;
    descriptor_.max_storage_bytes = store_config.max_storage_bytes;
    descriptor_.max_storage_files = store_config.max_storage_files;
    descriptor_.min_free_space_bytes = store_config.min_free_space_bytes;
    descriptor_.max_manifest_bytes = store_config.max_manifest_bytes;
    descriptor_.injection = *store_config.failure_injection;
    descriptor_.run_spec_hash = run_spec_hash(spec);
    descriptor_.build_fingerprint = current_build_identity().hash;
    enabled_ = true;
}

bool FailureInjector::enabled() const noexcept {
    return enabled_;
}

void FailureInjector::hit(FailurePoint point, const FailureContext& context) {
    if (!enabled_) {
        return;
    }
    const std::array<std::uint8_t, 25> event =
        encode_trace_event(point, context);
    trace_hasher_.update(event);
    ++descriptor_.observed_trace_events;
    if (point != descriptor_.injection.selected_point) {
        return;
    }
    ++selected_hits_;
    if (selected_hits_ != descriptor_.injection.selected_occurrence) {
        return;
    }
    descriptor_.observed_trace_hash = trace_hasher_.finalize();
    descriptor_.run_incarnation = context.run_incarnation;
    descriptor_.block_id = context.block_id;
    descriptor_.checkpoint_sequence = context.checkpoint_sequence;
    write_descriptor_atomic(descriptor_.injection.replay_descriptor_path,
                            descriptor_);
    ::_exit(kFailureInjectionExitCode);
}

ReplayDescriptor read_replay_descriptor(const std::filesystem::path& path) {
    const std::string bytes = read_descriptor_bytes(path);
    if (std::string_view(bytes).starts_with(kReplayV1Prefix)) {
        throw std::runtime_error(
            "replay descriptor schema v1 is unsupported; regenerate it with "
            "the schema-v2 failure harness");
    }
    const Sha256Digest verified_checksum = verify_descriptor_checksum(bytes);
    const std::vector<std::pair<std::string, std::string>> fields =
        read_fields(bytes);
    FieldReader reader(fields);
    if (reader.take("format") != kReplayMagic) {
        throw std::runtime_error("unsupported replay descriptor format");
    }
    ReplayDescriptor descriptor;
    descriptor.version = parse_u32(reader.take("version"), "version");
    if (descriptor.version != kReplayDescriptorVersion) {
        throw std::runtime_error("unsupported replay descriptor version");
    }
    descriptor.run_spec_hash =
        parse_digest(reader.take("run_spec_hash"), "run_spec_hash");
    descriptor.build_fingerprint = parse_digest(
        reader.take("build_fingerprint"), "build_fingerprint");
    descriptor.spec.engine_version =
        parse_u64(reader.take("engine_version"), "engine_version");
    descriptor.spec.rng_version =
        parse_u64(reader.take("rng_version"), "rng_version");
    descriptor.spec.stats_schema_version = parse_u32(
        reader.take("stats_schema_version"), "stats_schema_version");
    descriptor.spec.model_type = static_cast<ModelType>(
        parse_u8(reader.take("model_type"), "model_type"));
    descriptor.spec.payoff_type = static_cast<PayoffType>(
        parse_u8(reader.take("payoff_type"), "payoff_type"));
    const std::uint64_t antithetic =
        parse_u64(reader.take("antithetic"), "antithetic");
    if (antithetic > 1U) {
        throw std::runtime_error("antithetic flag is invalid");
    }
    descriptor.spec.antithetic = antithetic == 1U;
    descriptor.spec.global_seed =
        parse_u64(reader.take("global_seed"), "global_seed");
    descriptor.spec.total_scenarios =
        parse_u64(reader.take("total_scenarios"), "total_scenarios");
    descriptor.spec.num_time_steps =
        parse_u32(reader.take("num_time_steps"), "num_time_steps");
    descriptor.spec.maturity = std::bit_cast<double>(
        parse_hex_u64(reader.take("maturity_bits"), "maturity_bits"));
    descriptor.spec.spot = std::bit_cast<double>(
        parse_hex_u64(reader.take("spot_bits"), "spot_bits"));
    descriptor.spec.strike = std::bit_cast<double>(
        parse_hex_u64(reader.take("strike_bits"), "strike_bits"));
    descriptor.spec.rate = std::bit_cast<double>(
        parse_hex_u64(reader.take("rate_bits"), "rate_bits"));
    switch (descriptor.spec.model_type) {
        case ModelType::Gbm:
            descriptor.spec.volatility = std::bit_cast<double>(
                parse_hex_u64(reader.take("volatility_bits"),
                              "volatility_bits"));
            break;
        case ModelType::Heston: {
            HestonParams heston;
            heston.discretization_version = parse_u32(
                reader.take("heston_discretization_version"),
                "heston_discretization_version");
            heston.initial_variance = std::bit_cast<double>(parse_hex_u64(
                reader.take("heston_initial_variance_bits"),
                "heston_initial_variance_bits"));
            heston.mean_reversion_rate = std::bit_cast<double>(parse_hex_u64(
                reader.take("heston_mean_reversion_rate_bits"),
                "heston_mean_reversion_rate_bits"));
            heston.long_run_variance = std::bit_cast<double>(parse_hex_u64(
                reader.take("heston_long_run_variance_bits"),
                "heston_long_run_variance_bits"));
            heston.volatility_of_variance = std::bit_cast<double>(
                parse_hex_u64(
                    reader.take("heston_volatility_of_variance_bits"),
                    "heston_volatility_of_variance_bits"));
            heston.correlation = std::bit_cast<double>(parse_hex_u64(
                reader.take("heston_correlation_bits"),
                "heston_correlation_bits"));
            descriptor.spec.heston = heston;
            break;
        }
        default:
            throw std::runtime_error(
                "replay descriptor contains an unsupported model type");
    }
    descriptor.engine_config.worker_count = parse_size(
        reader.take("worker_count"), "worker_count");
    descriptor.engine_config.block_size =
        parse_u64(reader.take("block_size"), "block_size");
    descriptor.engine_config.assignment_queue_capacity = parse_size(
        reader.take("assignment_queue_capacity"),
        "assignment_queue_capacity");
    descriptor.engine_config.completion_queue_capacity = parse_size(
        reader.take("completion_queue_capacity"),
        "completion_queue_capacity");
    descriptor.engine_config.max_materialized_blocks = parse_u64(
        reader.take("max_materialized_blocks"),
        "max_materialized_blocks");
    descriptor.checkpoint_interval_blocks = parse_u64(
        reader.take("checkpoint_interval_blocks"),
        "checkpoint_interval_blocks");
    descriptor.max_storage_bytes = parse_u64(
        reader.take("max_storage_bytes"), "max_storage_bytes");
    descriptor.max_storage_files = parse_u64(
        reader.take("max_storage_files"), "max_storage_files");
    descriptor.min_free_space_bytes = parse_u64(
        reader.take("min_free_space_bytes"), "min_free_space_bytes");
    descriptor.max_manifest_bytes = parse_u64(
        reader.take("max_manifest_bytes"), "max_manifest_bytes");
    descriptor.injection.failure_seed =
        parse_u64(reader.take("failure_seed"), "failure_seed");
    descriptor.injection.deterministic_scheduler_seed = parse_u64(
        reader.take("deterministic_scheduler_seed"),
        "deterministic_scheduler_seed");
    descriptor.injection.selected_point = parse_failure_point(
        reader.take("selected_failure_point"));
    descriptor.injection.selected_occurrence = parse_u64(
        reader.take("selected_occurrence"), "selected_occurrence");
    descriptor.observed_trace_hash = parse_digest(
        reader.take("observed_trace_hash"), "observed_trace_hash");
    descriptor.observed_trace_events = parse_u64(
        reader.take("observed_trace_events"), "observed_trace_events");
    descriptor.run_incarnation =
        parse_u64(reader.take("run_incarnation"), "run_incarnation");
    descriptor.block_id =
        parse_u64(reader.take("block_id"), "block_id");
    descriptor.checkpoint_sequence = parse_u64(
        reader.take("checkpoint_sequence"), "checkpoint_sequence");
    const Sha256Digest parsed_checksum = parse_digest(
        reader.take("descriptor_sha256"), "descriptor_sha256");
    reader.finish();

    if (parsed_checksum != verified_checksum) {
        throw std::runtime_error("replay descriptor checksum field mismatch");
    }

    descriptor.spec.validate();
    descriptor.engine_config.validate(descriptor.spec);
    if (descriptor.engine_config.worker_count != 1U) {
        throw std::runtime_error(
            "R3 replay v2 requires the recorded single-worker scheduler");
    }
    if (descriptor.checkpoint_interval_blocks == 0U ||
        descriptor.max_storage_bytes == 0U ||
        descriptor.max_storage_files == 0U ||
        descriptor.max_manifest_bytes < 256U ||
        descriptor.max_manifest_bytes > descriptor.max_storage_bytes) {
        throw std::runtime_error(
            "replay descriptor contains invalid durable-store policy");
    }
    if (run_spec_hash(descriptor.spec) != descriptor.run_spec_hash) {
        throw std::runtime_error("replay descriptor RunSpec hash mismatch");
    }
    if (encode_descriptor(descriptor) != bytes) {
        throw std::runtime_error("replay descriptor is not canonical schema v2");
    }
    descriptor.injection.replay_descriptor_path = path;
    descriptor.injection.validate();
    return descriptor;
}

}  // namespace mc
