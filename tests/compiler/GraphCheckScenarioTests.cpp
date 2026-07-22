#include <catch2/catch_test_macros.hpp>
#include "../../apps/graphcheck/Scenarios.h"

TEST_CASE("valid subtractive graph compiles into both domains") {
    const auto scenario = nodsynth::graphcheck::makeScenario("valid");
    const auto result = nodsynth::compiler::GraphCompiler{}.compile(scenario.graph, scenario.registry);
    REQUIRE(result.graph.has_value());
    REQUIRE(result.graph->perVoiceOrder.size() == 5);
    REQUIRE(result.graph->globalOrder.size() == 2);
}

TEST_CASE("named invalid scenarios return their expected diagnostic") {
    REQUIRE(nodsynth::graphcheck::expectedDiagnostic("type-error") ==
            nodsynth::compiler::DiagnosticCode::portKindMismatch);
    REQUIRE(nodsynth::graphcheck::expectedDiagnostic("cycle") ==
            nodsynth::compiler::DiagnosticCode::cycleDetected);
}
