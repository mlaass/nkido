#include <catch2/catch_test_macros.hpp>
#include "cedar/dsp/simd.hpp"

#include <string_view>

using namespace cedar;

TEST_CASE("simd dispatch: ISA ladder", "[simd]") {
    const simd::Isa saved = simd::active();
    CHECK(simd::has(simd::Isa::Scalar));
#if !defined(CEDAR_SIMD)
    CHECK(saved == simd::Isa::Scalar);  // default builds never dispatch
#endif
    simd::force(simd::Isa::Avx2);
    CHECK(simd::has(simd::Isa::Sse2));   // x86 levels nest
    CHECK_FALSE(simd::has(simd::Isa::Neon));
    simd::force(simd::Isa::Sse2);
    CHECK_FALSE(simd::has(simd::Isa::Avx2));
    simd::force(simd::Isa::Scalar);
    CHECK_FALSE(simd::has(simd::Isa::Sse2));
    CHECK(std::string_view(simd::name(simd::Isa::Wasm128)) == "wasm128");
    simd::force(saved);
}
