#include "mc/persistence.hpp"

#include "mc/codec.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace mc {
namespace {

constexpr std::array<std::uint8_t, 8> kMetadataMagic = {
    'M', 'C', 'R', '2', 'M', 'E', 'T', 'A'};
constexpr std::array<std::uint8_t, 8> kBlockMagic = {
    'M', 'C', 'R', '2', 'B', 'L', 'K', '_'};
constexpr std::array<std::uint8_t, 8> kManifestMagic = {
    'M', 'C', 'R', '2', 'M', 'A', 'N', '_'};
constexpr std::uint64_t kMaxMetadataBytes = 1024U * 1024U;
constexpr std::uint64_t kMaxBlockRecordBytes = 4096U;
constexpr std::uint32_t kMaxBuildDescriptionBytes = 64U * 1024U;
constexpr std::uint32_t kMaxFailureReasonBytes = 4096U;

constexpr std::array<std::uint32_t, 256> make_crc32c_table() {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t index = 0; index < table.size(); ++index) {
        std::uint32_t value = index;
        for (std::uint32_t bit = 0; bit < 8U; ++bit) {
            const std::uint32_t mask =
                static_cast<std::uint32_t>(0U - (value & 1U));
            value = (value >> 1U) ^ (0x82F63B78U & mask);
        }
        table[index] = value;
    }
    return table;
}

constexpr std::array<std::uint32_t, 256> kCrc32cTable =
    make_crc32c_table();

std::uint32_t crc32c_range(const std::uint8_t* data, std::size_t size) {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (std::size_t index = 0; index < size; ++index) {
        const std::uint8_t table_index =
            static_cast<std::uint8_t>(crc ^ data[index]);
        crc = kCrc32cTable[table_index] ^ (crc >> 8U);
    }
    return ~crc;
}

void append_u8(std::vector<std::uint8_t>& bytes, std::uint8_t value) {
    bytes.push_back(value);
}

void append_u32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    for (std::uint32_t shift = 0; shift < 32U; shift += 8U) {
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

void append_u64(std::vector<std::uint8_t>& bytes, std::uint64_t value) {
    for (std::uint32_t shift = 0; shift < 64U; shift += 8U) {
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

void append_digest(std::vector<std::uint8_t>& bytes,
                   const Sha256Digest& digest) {
    bytes.insert(bytes.end(), digest.begin(), digest.end());
}

void append_string(std::vector<std::uint8_t>& bytes,
                   const std::string& value,
                   std::uint32_t maximum_size) {
    if (value.size() > maximum_size ||
        value.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("durable string exceeds its schema limit");
    }
    append_u32(bytes, static_cast<std::uint32_t>(value.size()));
    bytes.insert(bytes.end(), value.begin(), value.end());
}

class Reader {
public:
    explicit Reader(const std::vector<std::uint8_t>& bytes) : bytes_(bytes) {}

    [[nodiscard]] std::size_t remaining() const noexcept {
        return bytes_.size() - offset_;
    }

    std::uint8_t u8() {
        require(1U);
        return bytes_[offset_++];
    }

    std::uint32_t u32() {
        require(4U);
        std::uint32_t value = 0;
        for (std::uint32_t index = 0; index < 4U; ++index) {
            value |= static_cast<std::uint32_t>(bytes_[offset_++]) <<
                     (index * 8U);
        }
        return value;
    }

    std::uint64_t u64() {
        require(8U);
        std::uint64_t value = 0;
        for (std::uint32_t index = 0; index < 8U; ++index) {
            value |= static_cast<std::uint64_t>(bytes_[offset_++]) <<
                     (index * 8U);
        }
        return value;
    }

    double binary64() {
        static_assert(sizeof(double) == sizeof(std::uint64_t));
        static_assert(std::numeric_limits<double>::is_iec559);
        return std::bit_cast<double>(u64());
    }

    Sha256Digest digest() {
        require(Sha256Digest{}.size());
        Sha256Digest value{};
        std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(offset_),
                    value.size(), value.begin());
        offset_ += value.size();
        return value;
    }

    std::vector<std::uint8_t> byte_vector(std::size_t size) {
        require(size);
        std::vector<std::uint8_t> value(
            bytes_.begin() + static_cast<std::ptrdiff_t>(offset_),
            bytes_.begin() + static_cast<std::ptrdiff_t>(offset_ + size));
        offset_ += size;
        return value;
    }

    std::string string(std::uint32_t maximum_size) {
        const std::uint32_t size = u32();
        if (size > maximum_size) {
            throw std::runtime_error("durable string exceeds its schema limit");
        }
        const std::vector<std::uint8_t> raw = byte_vector(size);
        return std::string(raw.begin(), raw.end());
    }

    void finish() const {
        if (remaining() != 0U) {
            throw std::runtime_error("durable payload has trailing bytes");
        }
    }

private:
    void require(std::size_t count) const {
        if (count > remaining()) {
            throw std::runtime_error("durable payload is truncated");
        }
    }

    const std::vector<std::uint8_t>& bytes_;
    std::size_t offset_ = 0;
};

std::vector<std::uint8_t> make_envelope(
    const std::array<std::uint8_t, 8>& magic,
    const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(8U + 4U + 8U + payload.size() + 4U);
    bytes.insert(bytes.end(), magic.begin(), magic.end());
    append_u32(bytes, kStorageSchemaVersion);
    append_u64(bytes, static_cast<std::uint64_t>(payload.size()));
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    append_u32(bytes, crc32c(bytes));
    return bytes;
}

std::vector<std::uint8_t> open_envelope(
    const std::vector<std::uint8_t>& bytes,
    const std::array<std::uint8_t, 8>& expected_magic,
    std::uint64_t maximum_size) {
    constexpr std::size_t overhead = 8U + 4U + 8U + 4U;
    if (bytes.size() < overhead || bytes.size() > maximum_size) {
        throw std::runtime_error("durable envelope size is invalid");
    }
    if (!std::equal(expected_magic.begin(), expected_magic.end(), bytes.begin())) {
        throw std::runtime_error("durable envelope magic mismatch");
    }

    Reader checksum_reader(bytes);
    static_cast<void>(checksum_reader.byte_vector(8U));
    const std::uint32_t schema = checksum_reader.u32();
    if (schema != kStorageSchemaVersion) {
        throw std::runtime_error("unsupported durable storage schema version");
    }
    const std::uint64_t payload_size = checksum_reader.u64();
    if (payload_size > maximum_size ||
        payload_size != bytes.size() - overhead) {
        throw std::runtime_error("durable envelope payload length mismatch");
    }
    static_cast<void>(checksum_reader.byte_vector(
        static_cast<std::size_t>(payload_size)));
    const std::uint32_t stored_crc = checksum_reader.u32();
    checksum_reader.finish();
    if (stored_crc != crc32c_range(bytes.data(), bytes.size() - 4U)) {
        throw std::runtime_error("durable envelope CRC32C mismatch");
    }

    return std::vector<std::uint8_t>(
        bytes.begin() + static_cast<std::ptrdiff_t>(8U + 4U + 8U),
        bytes.end() - 4);
}

RunSpec decode_run_spec_payload(const std::vector<std::uint8_t>& bytes) {
    Reader reader(bytes);
    if (reader.u32() != kRunSpecSchemaVersion) {
        throw std::runtime_error("unsupported persisted RunSpec schema");
    }
    RunSpec spec;
    spec.engine_version = reader.u64();
    spec.rng_version = reader.u64();
    spec.stats_schema_version = reader.u32();
    spec.model_type = static_cast<ModelType>(reader.u8());
    spec.payoff_type = static_cast<PayoffType>(reader.u8());
    const std::uint8_t antithetic = reader.u8();
    if (antithetic > 1U || reader.u8() != 0U) {
        throw std::runtime_error("persisted RunSpec flags are invalid");
    }
    spec.antithetic = antithetic == 1U;
    spec.global_seed = reader.u64();
    spec.total_scenarios = reader.u64();
    spec.num_time_steps = reader.u32();
    spec.maturity = reader.binary64();
    spec.spot = reader.binary64();
    spec.strike = reader.binary64();
    spec.rate = reader.binary64();
    switch (spec.model_type) {
        case ModelType::Gbm:
            spec.volatility = reader.binary64();
            break;
        case ModelType::Heston: {
            HestonParams heston;
            heston.discretization_version = reader.u32();
            heston.initial_variance = reader.binary64();
            heston.mean_reversion_rate = reader.binary64();
            heston.long_run_variance = reader.binary64();
            heston.volatility_of_variance = reader.binary64();
            heston.correlation = reader.binary64();
            spec.heston = heston;
            break;
        }
        default:
            throw std::runtime_error("persisted RunSpec model is unsupported");
    }
    reader.finish();
    spec.validate();
    return spec;
}

AggregateStats decode_aggregate_payload(
    const std::vector<std::uint8_t>& bytes,
    std::uint32_t expected_schema) {
    Reader reader(bytes);
    if (reader.u32() != 1U || reader.u32() != expected_schema) {
        throw std::runtime_error("persisted aggregate schema mismatch");
    }
    AggregateStats aggregate;
    aggregate.n = reader.u64();
    aggregate.mean = reader.binary64();
    aggregate.m2 = reader.binary64();
    aggregate.min = reader.binary64();
    aggregate.max = reader.binary64();
    reader.finish();
    if (const std::optional<std::string> error = aggregate.invariant_error();
        error.has_value()) {
        throw std::runtime_error("invalid persisted aggregate: " + *error);
    }
    return aggregate;
}

std::uint32_t status_code(ValidationStatus status) {
    // Storage codes are explicit compatibility constants. They intentionally
    // preserve schema-v1 values but do not depend on enum declaration order.
    switch (status) {
        case ValidationStatus::Accepted:
            return 0U;
        case ValidationStatus::Duplicate:
            return 1U;
        case ValidationStatus::InvalidRun:
            return 2U;
        case ValidationStatus::ExecutionLayoutMismatch:
            return 3U;
        case ValidationStatus::BuildMismatch:
            return 4U;
        case ValidationStatus::StaleIncarnation:
            return 5U;
        case ValidationStatus::RngVersionMismatch:
            return 6U;
        case ValidationStatus::StatsSchemaMismatch:
            return 7U;
        case ValidationStatus::InvalidAggregate:
            return 8U;
        case ValidationStatus::CorruptPayload:
            return 9U;
        case ValidationStatus::InvalidBlock:
            return 10U;
        case ValidationStatus::StaleLease:
            return 11U;
        case ValidationStatus::DeterminismError:
            return 12U;
    }
    throw std::invalid_argument("unknown validation status for persistence");
}

ValidationStatus validation_status(std::uint32_t code) {
    switch (code) {
        case 0U:
            return ValidationStatus::Accepted;
        case 1U:
            return ValidationStatus::Duplicate;
        case 2U:
            return ValidationStatus::InvalidRun;
        case 3U:
            return ValidationStatus::ExecutionLayoutMismatch;
        case 4U:
            return ValidationStatus::BuildMismatch;
        case 5U:
            return ValidationStatus::StaleIncarnation;
        case 6U:
            return ValidationStatus::RngVersionMismatch;
        case 7U:
            return ValidationStatus::StatsSchemaMismatch;
        case 8U:
            return ValidationStatus::InvalidAggregate;
        case 9U:
            return ValidationStatus::CorruptPayload;
        case 10U:
            return ValidationStatus::InvalidBlock;
        case 11U:
            return ValidationStatus::StaleLease;
        case 12U:
            return ValidationStatus::DeterminismError;
        default:
            throw std::runtime_error(
                "persisted failure has an unknown status code");
    }
}

DurableRunStatus durable_status(std::uint8_t code) {
    switch (code) {
        case static_cast<std::uint8_t>(DurableRunStatus::Running):
            return DurableRunStatus::Running;
        case static_cast<std::uint8_t>(DurableRunStatus::Complete):
            return DurableRunStatus::Complete;
        case static_cast<std::uint8_t>(DurableRunStatus::Failed):
            return DurableRunStatus::Failed;
        default:
            throw std::runtime_error("persisted manifest has an unknown run status");
    }
}

}  // namespace

std::uint32_t crc32c(const std::vector<std::uint8_t>& bytes) {
    return crc32c_range(bytes.data(), bytes.size());
}

Sha256Digest durable_run_id(const Sha256Digest& spec_hash,
                            const Sha256Digest& layout_hash) {
    const std::string tag = "mc-durable-run-v1";
    std::vector<std::uint8_t> bytes(tag.begin(), tag.end());
    append_digest(bytes, spec_hash);
    append_digest(bytes, layout_hash);
    return sha256(bytes);
}

std::vector<std::uint8_t> encode_run_metadata(const RunMetadata& metadata) {
    metadata.spec.validate();
    if (metadata.block_size == 0U || metadata.block_count == 0U) {
        throw std::invalid_argument("run metadata has an empty execution layout");
    }
    if (metadata.run_spec_hash != run_spec_hash(metadata.spec) ||
        metadata.run_id != durable_run_id(metadata.run_spec_hash,
                                          metadata.execution_layout_hash)) {
        throw std::invalid_argument("run metadata identity is inconsistent");
    }
    const std::vector<std::uint8_t> build_bytes(metadata.build_description.begin(),
                                                metadata.build_description.end());
    if (metadata.build_description.empty() ||
        sha256(build_bytes) != metadata.build_fingerprint) {
        throw std::invalid_argument("run metadata build identity is inconsistent");
    }

    const std::vector<std::uint8_t> spec_bytes =
        encode_run_spec_payload(metadata.spec);
    std::vector<std::uint8_t> payload;
    append_digest(payload, metadata.run_id);
    append_u32(payload, static_cast<std::uint32_t>(spec_bytes.size()));
    payload.insert(payload.end(), spec_bytes.begin(), spec_bytes.end());
    append_u64(payload, metadata.block_size);
    append_u64(payload, metadata.block_count);
    append_digest(payload, metadata.run_spec_hash);
    append_digest(payload, metadata.execution_layout_hash);
    append_digest(payload, metadata.build_fingerprint);
    append_string(payload, metadata.build_description,
                  kMaxBuildDescriptionBytes);
    return make_envelope(kMetadataMagic, payload);
}

RunMetadata decode_run_metadata(const std::vector<std::uint8_t>& bytes) {
    const std::vector<std::uint8_t> payload =
        open_envelope(bytes, kMetadataMagic, kMaxMetadataBytes);
    Reader reader(payload);
    RunMetadata metadata;
    metadata.run_id = reader.digest();
    const std::uint32_t spec_size = reader.u32();
    metadata.spec = decode_run_spec_payload(reader.byte_vector(spec_size));
    metadata.block_size = reader.u64();
    metadata.block_count = reader.u64();
    metadata.run_spec_hash = reader.digest();
    metadata.execution_layout_hash = reader.digest();
    metadata.build_fingerprint = reader.digest();
    metadata.build_description = reader.string(kMaxBuildDescriptionBytes);
    reader.finish();

    if (metadata.block_size == 0U || metadata.block_count == 0U ||
        metadata.run_spec_hash != run_spec_hash(metadata.spec) ||
        metadata.run_id != durable_run_id(metadata.run_spec_hash,
                                          metadata.execution_layout_hash)) {
        throw std::runtime_error("persisted run metadata identity is inconsistent");
    }
    const std::uint64_t expected_blocks =
        1U + (metadata.spec.total_scenarios - 1U) / metadata.block_size;
    if (metadata.block_count != expected_blocks) {
        throw std::runtime_error("persisted run metadata block count is invalid");
    }
    EngineConfig layout_config;
    layout_config.block_size = metadata.block_size;
    layout_config.max_materialized_blocks = metadata.block_count;
    if (metadata.execution_layout_hash !=
        execution_layout_hash(metadata.spec, layout_config)) {
        throw std::runtime_error("persisted execution-layout identity is invalid");
    }
    const std::vector<std::uint8_t> build_bytes(metadata.build_description.begin(),
                                                metadata.build_description.end());
    if (metadata.build_description.empty() ||
        metadata.build_fingerprint != sha256(build_bytes)) {
        throw std::runtime_error("persisted build identity is invalid");
    }
    return metadata;
}

std::vector<std::uint8_t> encode_block_record(
    const DurableBlockRecord& record) {
    if (record.result.block.lease_epoch == 0U ||
        record.result.payload_checksum != aggregate_payload_hash(
            record.result.aggregate, record.result.stats_schema_version)) {
        throw std::invalid_argument("block record has inconsistent payload identity");
    }

    std::vector<std::uint8_t> payload;
    append_digest(payload, record.run_id);
    append_digest(payload, record.result.run_spec_hash);
    append_digest(payload, record.result.execution_layout_hash);
    append_digest(payload, record.result.build_fingerprint);
    append_u64(payload, record.result.block.block_id);
    append_u64(payload, record.result.block.start_scenario);
    append_u64(payload, record.result.block.end_scenario);
    append_u64(payload, record.result.block.run_incarnation);
    append_u64(payload, record.result.block.lease_epoch);
    append_u64(payload, record.result.rng_version);
    append_u32(payload, record.result.stats_schema_version);
    append_u64(payload, record.result.worker_id);
    const std::vector<std::uint8_t> aggregate_bytes = encode_aggregate_payload(
        record.result.aggregate, record.result.stats_schema_version);
    append_u32(payload, static_cast<std::uint32_t>(aggregate_bytes.size()));
    payload.insert(payload.end(), aggregate_bytes.begin(), aggregate_bytes.end());
    append_digest(payload, record.result.payload_checksum);
    return make_envelope(kBlockMagic, payload);
}

DurableBlockRecord decode_block_record(const std::vector<std::uint8_t>& bytes) {
    const std::vector<std::uint8_t> payload =
        open_envelope(bytes, kBlockMagic, kMaxBlockRecordBytes);
    Reader reader(payload);
    DurableBlockRecord record;
    record.run_id = reader.digest();
    record.result.run_spec_hash = reader.digest();
    record.result.execution_layout_hash = reader.digest();
    record.result.build_fingerprint = reader.digest();
    record.result.block.block_id = reader.u64();
    record.result.block.start_scenario = reader.u64();
    record.result.block.end_scenario = reader.u64();
    record.result.block.run_incarnation = reader.u64();
    record.result.block.lease_epoch = reader.u64();
    record.result.rng_version = reader.u64();
    record.result.stats_schema_version = reader.u32();
    record.result.worker_id = reader.u64();
    const std::uint32_t aggregate_size = reader.u32();
    record.result.aggregate = decode_aggregate_payload(
        reader.byte_vector(aggregate_size), record.result.stats_schema_version);
    record.result.payload_checksum = reader.digest();
    reader.finish();
    if (record.result.block.lease_epoch == 0U ||
        record.result.payload_checksum != aggregate_payload_hash(
            record.result.aggregate, record.result.stats_schema_version)) {
        throw std::runtime_error("persisted block payload identity is invalid");
    }
    return record;
}

std::vector<std::uint8_t> encode_manifest(const RunManifest& manifest) {
    if (manifest.run_incarnation == 0U || manifest.block_count == 0U ||
        manifest.lease_epochs.size() != manifest.block_count) {
        throw std::invalid_argument("manifest block/lease universe is invalid");
    }
    if (std::any_of(manifest.lease_epochs.begin(), manifest.lease_epochs.end(),
                    [](std::uint64_t epoch) { return epoch == 0U; })) {
        throw std::invalid_argument("manifest contains a zero lease epoch");
    }
    if (manifest.status == DurableRunStatus::Failed) {
        if (!manifest.failure.has_value()) {
            throw std::invalid_argument("failed manifest is missing failure details");
        }
    } else if (manifest.failure.has_value()) {
        throw std::invalid_argument("non-failed manifest contains failure details");
    }
    if (manifest.status == DurableRunStatus::Complete &&
        manifest.committed_blocks.size() != manifest.block_count) {
        throw std::invalid_argument("complete manifest is missing committed blocks");
    }

    std::uint64_t previous_id = 0;
    bool first = true;
    for (const ManifestEntry& entry : manifest.committed_blocks) {
        if (entry.block_id >= manifest.block_count ||
            entry.result_incarnation == 0U || entry.lease_epoch == 0U ||
            (!first && entry.block_id <= previous_id)) {
            throw std::invalid_argument("manifest entries are not canonical");
        }
        first = false;
        previous_id = entry.block_id;
    }
    if (const std::optional<std::string> error =
            manifest.committed_aggregate.invariant_error();
        error.has_value()) {
        throw std::invalid_argument("manifest aggregate is invalid: " + *error);
    }

    std::vector<std::uint8_t> payload;
    const std::size_t failure_bytes = manifest.failure.has_value()
                                          ? manifest.failure->reason.size() + 96U
                                          : 0U;
    payload.reserve(256U + manifest.lease_epochs.size() * 8U +
                    manifest.committed_blocks.size() * 56U + failure_bytes);
    append_digest(payload, manifest.run_id);
    append_digest(payload, manifest.run_spec_hash);
    append_digest(payload, manifest.execution_layout_hash);
    append_digest(payload, manifest.build_fingerprint);
    append_u64(payload, manifest.sequence);
    append_u8(payload, static_cast<std::uint8_t>(manifest.status));
    for (std::uint32_t index = 0; index < 7U; ++index) {
        append_u8(payload, 0U);
    }
    append_u64(payload, manifest.run_incarnation);
    append_u64(payload, manifest.rng_version);
    append_u32(payload, manifest.stats_schema_version);
    append_u32(payload, 0U);
    append_u64(payload, manifest.block_count);
    append_u64(payload, static_cast<std::uint64_t>(manifest.lease_epochs.size()));
    for (const std::uint64_t epoch : manifest.lease_epochs) {
        append_u64(payload, epoch);
    }
    append_u64(payload,
               static_cast<std::uint64_t>(manifest.committed_blocks.size()));
    for (const ManifestEntry& entry : manifest.committed_blocks) {
        append_u64(payload, entry.block_id);
        append_u64(payload, entry.result_incarnation);
        append_u64(payload, entry.lease_epoch);
        append_digest(payload, entry.payload_checksum);
    }
    const std::vector<std::uint8_t> aggregate_bytes = encode_aggregate_payload(
        manifest.committed_aggregate, manifest.stats_schema_version);
    append_u32(payload, static_cast<std::uint32_t>(aggregate_bytes.size()));
    payload.insert(payload.end(), aggregate_bytes.begin(), aggregate_bytes.end());
    append_u8(payload, manifest.failure.has_value() ? 1U : 0U);
    if (manifest.failure.has_value()) {
        const FailureRecord& failure = *manifest.failure;
        append_u32(payload, status_code(failure.status));
        append_u64(payload, failure.block_id);
        append_u64(payload, failure.run_incarnation);
        append_u64(payload, failure.lease_epoch);
        append_digest(payload, failure.observed_checksum);
        append_digest(payload, failure.committed_checksum);
        append_string(payload, failure.reason, kMaxFailureReasonBytes);
    }
    return make_envelope(kManifestMagic, payload);
}

RunManifest decode_manifest(const std::vector<std::uint8_t>& bytes,
                            std::uint64_t max_manifest_bytes) {
    if (max_manifest_bytes < 256U ||
        max_manifest_bytes >
            static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        throw std::invalid_argument("max_manifest_bytes is outside host limits");
    }
    const std::vector<std::uint8_t> payload =
        open_envelope(bytes, kManifestMagic, max_manifest_bytes);
    Reader reader(payload);
    RunManifest manifest;
    manifest.run_id = reader.digest();
    manifest.run_spec_hash = reader.digest();
    manifest.execution_layout_hash = reader.digest();
    manifest.build_fingerprint = reader.digest();
    manifest.sequence = reader.u64();
    manifest.status = durable_status(reader.u8());
    for (std::uint32_t index = 0; index < 7U; ++index) {
        if (reader.u8() != 0U) {
            throw std::runtime_error("manifest reserved status bytes are nonzero");
        }
    }
    manifest.run_incarnation = reader.u64();
    manifest.rng_version = reader.u64();
    manifest.stats_schema_version = reader.u32();
    if (reader.u32() != 0U) {
        throw std::runtime_error("manifest reserved schema field is nonzero");
    }
    manifest.block_count = reader.u64();
    const std::uint64_t lease_count = reader.u64();
    if (manifest.block_count == 0U || lease_count != manifest.block_count ||
        lease_count > reader.remaining() / 8U) {
        throw std::runtime_error("manifest lease table size is invalid");
    }
    manifest.lease_epochs.reserve(static_cast<std::size_t>(lease_count));
    for (std::uint64_t index = 0; index < lease_count; ++index) {
        const std::uint64_t epoch = reader.u64();
        if (epoch == 0U) {
            throw std::runtime_error("manifest contains a zero lease epoch");
        }
        manifest.lease_epochs.push_back(epoch);
    }

    const std::uint64_t committed_count = reader.u64();
    constexpr std::uint64_t entry_size = 8U + 8U + 8U + 32U;
    if (committed_count > manifest.block_count ||
        committed_count > reader.remaining() / entry_size) {
        throw std::runtime_error("manifest committed table size is invalid");
    }
    manifest.committed_blocks.reserve(static_cast<std::size_t>(committed_count));
    std::uint64_t previous_id = 0;
    for (std::uint64_t index = 0; index < committed_count; ++index) {
        ManifestEntry entry;
        entry.block_id = reader.u64();
        entry.result_incarnation = reader.u64();
        entry.lease_epoch = reader.u64();
        entry.payload_checksum = reader.digest();
        if (entry.block_id >= manifest.block_count ||
            entry.result_incarnation == 0U || entry.lease_epoch == 0U ||
            (index != 0U && entry.block_id <= previous_id)) {
            throw std::runtime_error("manifest entries are not canonical");
        }
        previous_id = entry.block_id;
        manifest.committed_blocks.push_back(entry);
    }
    const std::uint32_t aggregate_size = reader.u32();
    manifest.committed_aggregate = decode_aggregate_payload(
        reader.byte_vector(aggregate_size), manifest.stats_schema_version);
    const std::uint8_t has_failure = reader.u8();
    if (has_failure > 1U) {
        throw std::runtime_error("manifest failure flag is invalid");
    }
    if (has_failure == 1U) {
        FailureRecord failure;
        failure.status = validation_status(reader.u32());
        failure.block_id = reader.u64();
        failure.run_incarnation = reader.u64();
        failure.lease_epoch = reader.u64();
        failure.observed_checksum = reader.digest();
        failure.committed_checksum = reader.digest();
        failure.reason = reader.string(kMaxFailureReasonBytes);
        manifest.failure = std::move(failure);
    }
    reader.finish();

    if (manifest.run_incarnation == 0U ||
        (manifest.status == DurableRunStatus::Failed) !=
            manifest.failure.has_value() ||
        (manifest.status == DurableRunStatus::Complete &&
         manifest.committed_blocks.size() != manifest.block_count)) {
        throw std::runtime_error("manifest run state is inconsistent");
    }
    return manifest;
}

}  // namespace mc
