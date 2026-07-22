#pragma once

#include <optional>
#include <string_view>

#include <nodsynth/compiler/GraphCompiler.h>
#include <nodsynth/model/GraphDocument.h>
#include <nodsynth/model/SchemaRegistry.h>

namespace nodsynth::graphcheck {
struct Scenario {
    model::GraphSnapshot graph;
    model::SchemaRegistry registry;
    bool expectSuccess;
    compiler::DiagnosticCode expectedCode;
    std::optional<model::NodeId> expectedNodeId;
    std::optional<model::PortId> expectedPortId;
};

[[nodiscard]] Scenario makeScenario(std::string_view name);
[[nodiscard]] compiler::DiagnosticCode expectedDiagnostic(std::string_view name);
[[nodiscard]] bool containsExpectedDiagnostic(
    const compiler::CompileResult& result,
    const Scenario& scenario);
[[nodiscard]] std::string_view diagnosticName(compiler::DiagnosticCode code) noexcept;
} // namespace nodsynth::graphcheck
