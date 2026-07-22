#include <catch2/catch_test_macros.hpp>
#include "../../apps/graphcheck/Scenarios.h"

#include <algorithm>
#include <stdexcept>
#include <vector>

namespace {
const nodsynth::compiler::BufferAssignment& audioAssignmentFor(
    const nodsynth::compiler::CompiledGraph& graph,
    const nodsynth::model::Endpoint& endpoint) {
    const auto found = std::ranges::find(graph.audioBuffers, endpoint,
                                         &nodsynth::compiler::BufferAssignment::output);
    if (found == graph.audioBuffers.end()) {
        throw std::logic_error{"missing scenario audio buffer assignment"};
    }
    return *found;
}
} // namespace

TEST_CASE("valid subtractive graph compiles into both domains") {
    const auto scenario = nodsynth::graphcheck::makeScenario("valid");
    const auto result = nodsynth::compiler::GraphCompiler{}.compile(scenario.graph, scenario.registry);
    REQUIRE(result.graph.has_value());
    REQUIRE(result.graph->perVoiceOrder == std::vector<nodsynth::model::NodeId>{
        nodsynth::model::NodeId{"midi"},
        nodsynth::model::NodeId{"envelope"},
        nodsynth::model::NodeId{"note-to-frequency"},
        nodsynth::model::NodeId{"oscillator"},
        nodsynth::model::NodeId{"gain"},
    });
    REQUIRE(result.graph->globalOrder == std::vector<nodsynth::model::NodeId>{
        nodsynth::model::NodeId{"voice-mix"},
        nodsynth::model::NodeId{"audio-output"},
    });
    const auto& voiceMix = audioAssignmentFor(
        *result.graph,
        {nodsynth::model::NodeId{"voice-mix"}, nodsynth::model::PortId{"audio"}});
    REQUIRE(voiceMix.domain == nodsynth::model::NodeScope::global);
    REQUIRE(voiceMix.channels == 2);
}

TEST_CASE("named invalid scenarios return their expected diagnostic") {
    REQUIRE(nodsynth::graphcheck::expectedDiagnostic("type-error") ==
            nodsynth::compiler::DiagnosticCode::portKindMismatch);
    REQUIRE(nodsynth::graphcheck::expectedDiagnostic("cycle") ==
            nodsynth::compiler::DiagnosticCode::cycleDetected);
}
