#include "Scenarios.h"

#include <iostream>
#include <string_view>

int main(int argc, char** argv) {
    if (argc != 3 || std::string_view{argv[1]} != "--scenario") {
        std::cerr << "usage: nod_graphcheck --scenario valid|type-error|cycle\n";
        return 64;
    }
    const auto scenario = nodsynth::graphcheck::makeScenario(argv[2]);
    const auto result = nodsynth::compiler::GraphCompiler{}.compile(scenario.graph, scenario.registry);
    if (scenario.expectSuccess) {
        if (!result.graph) return 1;
        std::cout << "valid perVoice=" << result.graph->perVoiceOrder.size()
                  << " global=" << result.graph->globalOrder.size() << '\n';
        return 0;
    }
    if (result.graph || !nodsynth::graphcheck::containsExpectedDiagnostic(result, scenario)) return 1;
    std::cout << "rejected code=" << nodsynth::graphcheck::diagnosticName(scenario.expectedCode) << '\n';
    return 0;
}
