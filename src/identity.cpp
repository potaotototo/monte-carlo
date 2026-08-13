#include "mc/identity.hpp"

#include "mc/run_spec.hpp"

#include <cstdint>
#include <string>
#include <sys/utsname.h>
#include <vector>

#if defined(__GLIBC__)
#include <gnu/libc-version.h>
#endif

#ifndef MC_FP_CONTRACT_MODE
#define MC_FP_CONTRACT_MODE "unspecified"
#endif

#ifndef MC_SOURCE_REVISION
#define MC_SOURCE_REVISION "untracked-source"
#endif

#ifndef MC_BUILD_FLAGS_ID
#define MC_BUILD_FLAGS_ID "untracked-build-flags"
#endif

#ifndef MC_BUILD_CONFIG
#define MC_BUILD_CONFIG "unspecified-build-config"
#endif

#ifndef MC_CPU_POLICY
#define MC_CPU_POLICY "unspecified-cpu-policy"
#endif

namespace mc {

BuildIdentity current_build_identity() {
    std::string compiler;
#if defined(__clang__)
    compiler = "clang-" __clang_version__;
#elif defined(__GNUC__)
    compiler = "gcc-" __VERSION__;
#elif defined(_MSC_VER)
    compiler = "msvc-" + std::to_string(_MSC_VER);
#else
    compiler = "unknown-compiler";
#endif

    std::string standard_library;
#if defined(_LIBCPP_VERSION)
    standard_library = "libc++-" + std::to_string(_LIBCPP_VERSION);
#elif defined(__GLIBCXX__)
    standard_library = "libstdc++-" + std::to_string(__GLIBCXX__);
#else
    standard_library = "unknown-standard-library";
#endif

    std::string architecture;
#if defined(__aarch64__) || defined(__arm64__)
    architecture = "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
    architecture = "x86_64";
#elif defined(__i386__) || defined(_M_IX86)
    architecture = "x86";
#else
    architecture = "unknown-architecture";
#endif

#if defined(__FAST_MATH__)
    constexpr const char* fast_math = "on";
#else
    constexpr const char* fast_math = "off";
#endif

#if defined(__OPTIMIZE__)
    constexpr const char* optimizer = "on";
#else
    constexpr const char* optimizer = "off";
#endif

    std::string compiled_cpu_features = "baseline";
#if defined(__AVX512F__)
    compiled_cpu_features += ",avx512f";
#endif
#if defined(__AVX2__)
    compiled_cpu_features += ",avx2";
#endif
#if defined(__AVX__)
    compiled_cpu_features += ",avx";
#endif
#if defined(__FMA__)
    compiled_cpu_features += ",fma";
#endif
#if defined(__ARM_FEATURE_FMA)
    compiled_cpu_features += ",arm-fma";
#endif

    std::string runtime_platform = "unknown-runtime-platform";
    struct utsname platform {};
    if (::uname(&platform) == 0) {
        runtime_platform = std::string(platform.sysname) + "-" +
                           platform.release + "-" + platform.machine;
    }

    std::string runtime_c_library;
#if defined(__GLIBC__)
    runtime_c_library = std::string("glibc-") + gnu_get_libc_version();
#elif defined(__APPLE__)
    // libm and libc are both provided by the OS-bundled libSystem on Apple
    // platforms; the uname release above pins the relevant runtime release.
    runtime_c_library = "libSystem";
#else
    runtime_c_library = "unknown-c-library";
#endif

    BuildIdentity identity;
    identity.description =
        "engine=" + std::to_string(kEngineVersion) + ";compiler=" + compiler +
        ";stdlib=" + standard_library + ";arch=" + architecture +
        ";cxx=" + std::to_string(__cplusplus) +
        ";fp_contract=" MC_FP_CONTRACT_MODE ";fast_math=" + fast_math +
        ";optimizer=" + optimizer +
        ";source=" MC_SOURCE_REVISION ";build_flags=" MC_BUILD_FLAGS_ID +
        ";build_config=" MC_BUILD_CONFIG +
        ";cpu_policy=" MC_CPU_POLICY ";cpu_features=" +
        compiled_cpu_features + ";runtime=" + runtime_platform +
        ";clib=" + runtime_c_library;
    const std::vector<std::uint8_t> bytes(identity.description.begin(),
                                          identity.description.end());
    identity.hash = sha256(bytes);
    return identity;
}

}  // namespace mc
