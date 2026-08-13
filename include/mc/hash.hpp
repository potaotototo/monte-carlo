#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace mc {

using Sha256Digest = std::array<std::uint8_t, 32>;

class Sha256Hasher {
public:
    Sha256Hasher() noexcept;

    void update(std::span<const std::uint8_t> bytes) noexcept;
    [[nodiscard]] Sha256Digest finalize() const noexcept;

private:
    std::array<std::uint32_t, 8> state_{};
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t buffered_bytes_ = 0;
    std::uint64_t total_bytes_ = 0;
};

Sha256Digest sha256(std::span<const std::uint8_t> bytes) noexcept;
std::string to_hex(const Sha256Digest& digest);

}  // namespace mc
