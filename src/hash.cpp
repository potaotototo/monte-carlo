#include "mc/hash.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace mc {
namespace {

constexpr std::array<std::uint32_t, 64> kRoundConstants = {
    0x428A2F98U, 0x71374491U, 0xB5C0FBCFU, 0xE9B5DBA5U,
    0x3956C25BU, 0x59F111F1U, 0x923F82A4U, 0xAB1C5ED5U,
    0xD807AA98U, 0x12835B01U, 0x243185BEU, 0x550C7DC3U,
    0x72BE5D74U, 0x80DEB1FEU, 0x9BDC06A7U, 0xC19BF174U,
    0xE49B69C1U, 0xEFBE4786U, 0x0FC19DC6U, 0x240CA1CCU,
    0x2DE92C6FU, 0x4A7484AAU, 0x5CB0A9DCU, 0x76F988DAU,
    0x983E5152U, 0xA831C66DU, 0xB00327C8U, 0xBF597FC7U,
    0xC6E00BF3U, 0xD5A79147U, 0x06CA6351U, 0x14292967U,
    0x27B70A85U, 0x2E1B2138U, 0x4D2C6DFCU, 0x53380D13U,
    0x650A7354U, 0x766A0ABBU, 0x81C2C92EU, 0x92722C85U,
    0xA2BFE8A1U, 0xA81A664BU, 0xC24B8B70U, 0xC76C51A3U,
    0xD192E819U, 0xD6990624U, 0xF40E3585U, 0x106AA070U,
    0x19A4C116U, 0x1E376C08U, 0x2748774CU, 0x34B0BCB5U,
    0x391C0CB3U, 0x4ED8AA4AU, 0x5B9CCA4FU, 0x682E6FF3U,
    0x748F82EEU, 0x78A5636FU, 0x84C87814U, 0x8CC70208U,
    0x90BEFFFAU, 0xA4506CEBU, 0xBEF9A3F7U, 0xC67178F2U,
};

constexpr std::uint32_t rotate_right(std::uint32_t value,
                                     std::uint32_t count) noexcept {
    return (value >> count) | (value << (32U - count));
}

void transform(const std::uint8_t* block,
               std::array<std::uint32_t, 8>& state) noexcept {
    std::array<std::uint32_t, 64> words{};
    for (std::size_t index = 0; index < 16; ++index) {
        const std::size_t offset = index * 4U;
        words[index] = (static_cast<std::uint32_t>(block[offset]) << 24U) |
                       (static_cast<std::uint32_t>(block[offset + 1U]) << 16U) |
                       (static_cast<std::uint32_t>(block[offset + 2U]) << 8U) |
                       static_cast<std::uint32_t>(block[offset + 3U]);
    }
    for (std::size_t index = 16; index < words.size(); ++index) {
        const std::uint32_t s0 = rotate_right(words[index - 15U], 7U) ^
                                 rotate_right(words[index - 15U], 18U) ^
                                 (words[index - 15U] >> 3U);
        const std::uint32_t s1 = rotate_right(words[index - 2U], 17U) ^
                                 rotate_right(words[index - 2U], 19U) ^
                                 (words[index - 2U] >> 10U);
        words[index] = words[index - 16U] + s0 + words[index - 7U] + s1;
    }

    std::uint32_t a = state[0];
    std::uint32_t b = state[1];
    std::uint32_t c = state[2];
    std::uint32_t d = state[3];
    std::uint32_t e = state[4];
    std::uint32_t f = state[5];
    std::uint32_t g = state[6];
    std::uint32_t h = state[7];

    for (std::size_t index = 0; index < words.size(); ++index) {
        const std::uint32_t sum1 = rotate_right(e, 6U) ^ rotate_right(e, 11U) ^
                                   rotate_right(e, 25U);
        const std::uint32_t choice = (e & f) ^ (~e & g);
        const std::uint32_t temp1 =
            h + sum1 + choice + kRoundConstants[index] + words[index];
        const std::uint32_t sum0 = rotate_right(a, 2U) ^ rotate_right(a, 13U) ^
                                   rotate_right(a, 22U);
        const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t temp2 = sum0 + majority;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

}  // namespace

Sha256Hasher::Sha256Hasher() noexcept
    : state_({
        0x6A09E667U, 0xBB67AE85U, 0x3C6EF372U, 0xA54FF53AU,
        0x510E527FU, 0x9B05688CU, 0x1F83D9ABU, 0x5BE0CD19U,
    }) {}

void Sha256Hasher::update(std::span<const std::uint8_t> bytes) noexcept {
    total_bytes_ += static_cast<std::uint64_t>(bytes.size());
    std::size_t offset = 0U;
    if (buffered_bytes_ != 0U) {
        const std::size_t copied =
            std::min(buffer_.size() - buffered_bytes_, bytes.size());
        for (std::size_t index = 0U; index < copied; ++index) {
            buffer_[buffered_bytes_ + index] = bytes[index];
        }
        buffered_bytes_ += copied;
        offset += copied;
        if (buffered_bytes_ == buffer_.size()) {
            transform(buffer_.data(), state_);
            buffered_bytes_ = 0U;
        }
    }

    while (bytes.size() - offset >= buffer_.size()) {
        transform(bytes.data() + offset, state_);
        offset += buffer_.size();
    }
    while (offset < bytes.size()) {
        buffer_[buffered_bytes_++] = bytes[offset++];
    }
}

Sha256Digest Sha256Hasher::finalize() const noexcept {
    std::array<std::uint32_t, 8> state = state_;
    std::array<std::uint8_t, 128> tail{};
    for (std::size_t index = 0U; index < buffered_bytes_; ++index) {
        tail[index] = buffer_[index];
    }
    tail[buffered_bytes_] = 0x80U;
    const std::size_t padded_size = buffered_bytes_ < 56U ? 64U : 128U;
    const std::uint64_t bit_length = total_bytes_ * 8U;
    for (std::size_t index = 0; index < 8U; ++index) {
        tail[padded_size - 1U - index] =
            static_cast<std::uint8_t>(bit_length >> (index * 8U));
    }
    transform(tail.data(), state);
    if (padded_size == 128U) {
        transform(tail.data() + 64U, state);
    }

    Sha256Digest digest{};
    for (std::size_t index = 0; index < state.size(); ++index) {
        digest[index * 4U] = static_cast<std::uint8_t>(state[index] >> 24U);
        digest[index * 4U + 1U] = static_cast<std::uint8_t>(state[index] >> 16U);
        digest[index * 4U + 2U] = static_cast<std::uint8_t>(state[index] >> 8U);
        digest[index * 4U + 3U] = static_cast<std::uint8_t>(state[index]);
    }
    return digest;
}

Sha256Digest sha256(std::span<const std::uint8_t> bytes) noexcept {
    Sha256Hasher hasher;
    hasher.update(bytes);
    return hasher.finalize();
}

std::string to_hex(const Sha256Digest& digest) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.resize(digest.size() * 2U);
    for (std::size_t index = 0; index < digest.size(); ++index) {
        result[index * 2U] = digits[digest[index] >> 4U];
        result[index * 2U + 1U] = digits[digest[index] & 0x0FU];
    }
    return result;
}

}  // namespace mc
