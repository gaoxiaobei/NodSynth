#include <catch2/catch_test_macros.hpp>
#include <nodsynth/model/BuildInfo.h>

TEST_CASE("build info exposes the core API version") {
    const auto info = nodsynth::model::buildInfo();
    REQUIRE(info.coreApiMajor == 1);
    REQUIRE(info.coreApiMinor == 0);
    REQUIRE(info.projectName == "NodSynth");
}
