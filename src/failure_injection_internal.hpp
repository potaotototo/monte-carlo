#pragma once

#include "mc/failure_injection.hpp"

#include <cstdint>

namespace mc {

struct RunStoreConfig;

struct FailureContext {
    std::uint64_t run_incarnation = kNoFailureContext;
    std::uint64_t block_id = kNoFailureContext;
    std::uint64_t checkpoint_sequence = kNoFailureContext;
};

class FailureInjector {
public:
    FailureInjector(const RunSpec& spec,
                    const EngineConfig& engine_config,
                    const RunStoreConfig& store_config);

    [[nodiscard]] bool enabled() const noexcept;
    void hit(FailurePoint point, const FailureContext& context);

private:
    ReplayDescriptor descriptor_;
    Sha256Hasher trace_hasher_;
    bool enabled_ = false;
    std::uint64_t selected_hits_ = 0;
};

}  // namespace mc
