#include <catch2/catch_test_macros.hpp>
#include <nodsynth/compiler/GraphCompiler.h>

#include <algorithm>
#include <tuple>
#include <vector>

#include "TestGraphBuilders.h"

using namespace nodsynth::model;
using namespace nodsynth::compiler;
using namespace nodsynth::compiler::test;

namespace {
bool hasDiagnostic(
    const CompileResult& result,
    DiagnosticCode code,
    const NodeId& nodeId,
    const std::optional<PortId>& portId = std::nullopt) {
    return std::ranges::any_of(result.diagnostics, [&](const Diagnostic& diagnostic) {
        return diagnostic.code == code && diagnostic.nodeId == nodeId && diagnostic.portId == portId;
    });
}

using DiagnosticLocation = std::tuple<DiagnosticCode, std::optional<NodeId>, std::optional<PortId>>;

std::vector<DiagnosticLocation> diagnosticLocations(const CompileResult& result) {
    std::vector<DiagnosticLocation> locations;
    for (const auto& diagnostic : result.diagnostics) {
        locations.emplace_back(diagnostic.code, diagnostic.nodeId, diagnostic.portId);
    }
    return locations;
}
} // namespace

TEST_CASE("compiler locates a missing node schema") {
    SchemaRegistry registry;
    GraphSnapshot graph{{{NodeId{"missing-1"}, NodeTypeId{"third.party.absent"}, 1, {}, "{}", {}}}, {}};

    const auto result = GraphCompiler{}.compile(graph, registry);

    REQUIRE_FALSE(result.graph.has_value());
    REQUIRE(result.diagnostics.front().code == DiagnosticCode::missingNodeType);
    REQUIRE(result.diagnostics.front().nodeId == NodeId{"missing-1"});
}

TEST_CASE("compiler rejects audio to control") {
    auto [registry, graph] = testGraphWithAudioSourceAndControlSink();

    const auto result = GraphCompiler{}.compile(graph, registry);

    REQUIRE_FALSE(result.graph.has_value());
    REQUIRE(result.diagnostics.front().code == DiagnosticCode::portKindMismatch);
    REQUIRE(result.diagnostics.front().nodeId == NodeId{"sink"});
    REQUIRE(result.diagnostics.front().portId == PortId{"frequency"});
}

TEST_CASE("compiler locates a connection endpoint whose node is missing") {
    SchemaRegistry registry;
    REQUIRE(registry.registerSchema(audioSourceSchema()));
    const GraphSnapshot graph{
        {{NodeId{"source"}, NodeTypeId{"test.audio-source"}, 1, {}, "{}", {}}},
        {{{NodeId{"source"}, PortId{"audio"}}, {NodeId{"absent"}, PortId{"audio"}}}},
    };

    const auto result = GraphCompiler{}.compile(graph, registry);

    REQUIRE_FALSE(result.graph.has_value());
    REQUIRE(hasDiagnostic(result, DiagnosticCode::missingEndpointNode, NodeId{"absent"}, PortId{"audio"}));
}

TEST_CASE("compiler locates a missing port") {
    SchemaRegistry registry;
    REQUIRE(registry.registerSchema(audioSourceSchema()));
    REQUIRE(registry.registerSchema(audioSinkSchema()));
    const GraphSnapshot graph{
        {
            {NodeId{"source"}, NodeTypeId{"test.audio-source"}, 1, {}, "{}", {}},
            {NodeId{"sink"}, NodeTypeId{"test.audio-sink"}, 1, {}, "{}", {}},
        },
        {{{NodeId{"source"}, PortId{"absent"}}, {NodeId{"sink"}, PortId{"audio"}}}},
    };

    const auto result = GraphCompiler{}.compile(graph, registry);

    REQUIRE_FALSE(result.graph.has_value());
    REQUIRE(hasDiagnostic(result, DiagnosticCode::missingPort, NodeId{"source"}, PortId{"absent"}));
}

TEST_CASE("compiler locates both reversed port directions") {
    SchemaRegistry registry;
    REQUIRE(registry.registerSchema(audioSourceSchema()));
    REQUIRE(registry.registerSchema(audioSinkSchema()));
    const GraphSnapshot graph{
        {
            {NodeId{"source"}, NodeTypeId{"test.audio-source"}, 1, {}, "{}", {}},
            {NodeId{"sink"}, NodeTypeId{"test.audio-sink"}, 1, {}, "{}", {}},
        },
        {{{NodeId{"sink"}, PortId{"audio"}}, {NodeId{"source"}, PortId{"audio"}}}},
    };

    const auto result = GraphCompiler{}.compile(graph, registry);

    REQUIRE_FALSE(result.graph.has_value());
    REQUIRE(hasDiagnostic(result, DiagnosticCode::wrongPortDirection, NodeId{"sink"}, PortId{"audio"}));
    REQUIRE(hasDiagnostic(result, DiagnosticCode::wrongPortDirection, NodeId{"source"}, PortId{"audio"}));
}

TEST_CASE("compiler locates an audio channel mismatch") {
    SchemaRegistry registry;
    REQUIRE(registry.registerSchema(audioSourceSchema()));
    REQUIRE(registry.registerSchema(stereoAudioSinkSchema()));
    const GraphSnapshot graph{
        {
            {NodeId{"source"}, NodeTypeId{"test.audio-source"}, 1, {}, "{}", {}},
            {NodeId{"sink"}, NodeTypeId{"test.stereo-audio-sink"}, 1, {}, "{}", {}},
        },
        {{{NodeId{"source"}, PortId{"audio"}}, {NodeId{"sink"}, PortId{"audio"}}}},
    };

    const auto result = GraphCompiler{}.compile(graph, registry);

    REQUIRE_FALSE(result.graph.has_value());
    REQUIRE(hasDiagnostic(result, DiagnosticCode::channelCountMismatch, NodeId{"sink"}, PortId{"audio"}));
}

TEST_CASE("compiler locates a second writer to an input") {
    SchemaRegistry registry;
    REQUIRE(registry.registerSchema(audioSourceSchema()));
    REQUIRE(registry.registerSchema(audioSinkSchema()));
    const GraphSnapshot graph{
        {
            {NodeId{"source-b"}, NodeTypeId{"test.audio-source"}, 1, {}, "{}", {}},
            {NodeId{"sink"}, NodeTypeId{"test.audio-sink"}, 1, {}, "{}", {}},
            {NodeId{"source-a"}, NodeTypeId{"test.audio-source"}, 1, {}, "{}", {}},
        },
        {
            {{NodeId{"source-b"}, PortId{"audio"}}, {NodeId{"sink"}, PortId{"audio"}}},
            {{NodeId{"source-a"}, PortId{"audio"}}, {NodeId{"sink"}, PortId{"audio"}}},
        },
    };

    const auto result = GraphCompiler{}.compile(graph, registry);

    REQUIRE_FALSE(result.graph.has_value());
    REQUIRE(hasDiagnostic(result, DiagnosticCode::duplicateInputConnection, NodeId{"sink"}, PortId{"audio"}));
}

TEST_CASE("compiler locates an unsupported schema version") {
    SchemaRegistry registry;
    REQUIRE(registry.registerSchema(parameterNodeSchema()));
    const GraphSnapshot graph{
        {{NodeId{"parameter"}, NodeTypeId{"test.parameter-node"}, 2, {}, "{}", {}}}, {}};

    const auto result = GraphCompiler{}.compile(graph, registry);

    REQUIRE_FALSE(result.graph.has_value());
    REQUIRE(hasDiagnostic(result, DiagnosticCode::unsupportedSchemaVersion, NodeId{"parameter"}));
}

TEST_CASE("compiler locates an unknown stored parameter") {
    SchemaRegistry registry;
    REQUIRE(registry.registerSchema(parameterNodeSchema()));
    const GraphSnapshot graph{
        {{NodeId{"parameter"}, NodeTypeId{"test.parameter-node"}, 1,
          {{ParameterId{"unknown"}, 0.5}}, "{}", {}}},
        {}};

    const auto result = GraphCompiler{}.compile(graph, registry);

    REQUIRE_FALSE(result.graph.has_value());
    REQUIRE(hasDiagnostic(result, DiagnosticCode::unknownParameter, NodeId{"parameter"}));
}

TEST_CASE("compiler locates a stored parameter outside its inclusive range") {
    SchemaRegistry registry;
    REQUIRE(registry.registerSchema(parameterNodeSchema()));
    const GraphSnapshot graph{
        {{NodeId{"parameter"}, NodeTypeId{"test.parameter-node"}, 1, {{ParameterId{"gain"}, 1.1}}, "{}", {}}},
        {}};

    const auto result = GraphCompiler{}.compile(graph, registry);

    REQUIRE_FALSE(result.graph.has_value());
    REQUIRE(hasDiagnostic(result, DiagnosticCode::parameterOutOfRange, NodeId{"parameter"}));
}

TEST_CASE("compiler locates a duplicate node ID") {
    SchemaRegistry registry;
    REQUIRE(registry.registerSchema(parameterNodeSchema()));
    const GraphSnapshot graph{
        {
            {NodeId{"duplicate"}, NodeTypeId{"test.parameter-node"}, 1, {}, "{}", {}},
            {NodeId{"duplicate"}, NodeTypeId{"test.parameter-node"}, 1, {}, "{}", {}},
        },
        {},
    };

    const auto result = GraphCompiler{}.compile(graph, registry);

    REQUIRE_FALSE(result.graph.has_value());
    REQUIRE(hasDiagnostic(result, DiagnosticCode::duplicateNodeId, NodeId{"duplicate"}));
}

TEST_CASE("compiler returns all independent diagnostics in stable code node port order") {
    SchemaRegistry registry;
    REQUIRE(registry.registerSchema(parameterNodeSchema()));
    GraphSnapshot graph{
        {
            {NodeId{"z-missing-type"}, NodeTypeId{"test.absent"}, 1, {}, "{}", {}},
            {NodeId{"b-parameter"}, NodeTypeId{"test.parameter-node"}, 1,
             {{ParameterId{"unknown"}, 0.2}}, "{}", {}},
            {NodeId{"a-parameter"}, NodeTypeId{"test.parameter-node"}, 2,
             {{ParameterId{"gain"}, 2.0}}, "{}", {}},
        },
        {
            {{NodeId{"a-parameter"}, PortId{"missing-z"}}, {NodeId{"absent"}, PortId{"input"}}},
            {{NodeId{"a-parameter"}, PortId{"missing-a"}}, {NodeId{"b-parameter"}, PortId{"missing-b"}}},
        },
    };
    auto permuted = graph;
    std::ranges::reverse(permuted.nodes);
    std::ranges::reverse(permuted.connections);

    const auto first = GraphCompiler{}.compile(graph, registry);
    const auto second = GraphCompiler{}.compile(permuted, registry);

    REQUIRE_FALSE(first.graph.has_value());
    REQUIRE(diagnosticLocations(first) == diagnosticLocations(second));
    REQUIRE(std::ranges::is_sorted(diagnosticLocations(first)));
    REQUIRE(hasDiagnostic(first, DiagnosticCode::missingNodeType, NodeId{"z-missing-type"}));
    REQUIRE(hasDiagnostic(first, DiagnosticCode::unsupportedSchemaVersion, NodeId{"a-parameter"}));
    REQUIRE(hasDiagnostic(first, DiagnosticCode::missingEndpointNode, NodeId{"absent"}, PortId{"input"}));
    REQUIRE(hasDiagnostic(first, DiagnosticCode::unknownParameter, NodeId{"b-parameter"}));
    REQUIRE(hasDiagnostic(first, DiagnosticCode::parameterOutOfRange, NodeId{"a-parameter"}));
}
