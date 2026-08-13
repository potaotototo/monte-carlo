#pragma once

#include "mc/hash.hpp"

#include <string>

namespace mc {

struct BuildIdentity {
    std::string description;
    Sha256Digest hash{};
};

BuildIdentity current_build_identity();

}  // namespace mc

