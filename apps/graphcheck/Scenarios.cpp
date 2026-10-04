#include "Scenarios.h"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>

#include <nodsynth/nodes/BuiltinNodes.h>

namespace nodsynth::graphcheck {
namespace {
using namespace nodsynth::compiler;
using namespace nodsynth::model;

NodeSchema midiInputSchema() {
    return {
        .typeId = NodeTypeId{"nod.midi-input"},
        .schemaVersion = 1,
        .displayName = "MIDI Input",
        .category = "Input",
        .sourceId = "nodsynth",
        .scope = NodeScope::perVoice,
        .ports = {
            {PortId{"note"}, "Note", PortDirection::output, PortKind::note, 1, PortDomain::sameAsNode},
            {PortId{"gate"}, "Gate", PortDirection::output, PortKind::gate, 1, PortDomain::sameAsNode},
        },
        .parameters = {},
        .breaksDependencyCycle = false,
    };
}

NodeSchema noteToFrequencySchema() {
    return {
        .typeId = NodeTypeId{"nod.note-to-frequency"},
        .schemaVersion = 1,
        .displayName = "Note to Frequency",
        .category = "Control",
        .sourceId = "nodsynth",
        .scope = NodeScope::perVoice,
        .ports = {
            {PortId{"note"}, "Note", PortDirection::input, PortKind::note, 1, PortDomain::sameAsNode},
            {PortId{"frequency"}, "Frequency", PortDirection::output, PortKind::control, 1,
             PortDomain::sameAsNode},
        },
        .parameters = {},
        .breaksDependencyCycle = false,
    };
}

NodeSchema oscillatorSchema() {
    return {
        .typeId = NodeTypeId{"nod.oscillator"},
        .schemaVersion = 1,
        .displayName = "Oscillator",
        .category = "Source",
        .sourceId = "nodsynth",
        .scope = NodeScope::perVoice,
        .ports = {
            {PortId{"frequency"}, "Frequency", PortDirection::input, PortKind::control, 1,
             PortDomain::sameAsNode},
            {PortId{"audio"}, "Audio", PortDirection::output, PortKind::audio, 1,
             PortDomain::sameAsNode},
        },
        .parameters = {},
        .breaksDependencyCycle = false,
    };
}

NodeSchema adsrSchema() {
    return {
        .typeId = NodeTypeId{"nod.adsr"},
        .schemaVersion = 1,
        .displayName = "ADSR",
        .category = "Control",
        .sourceId = "nodsynth",
        .scope = NodeScope::perVoice,
        .ports = {
            {PortId{"gate"}, "Gate", PortDirection::input, PortKind::gate, 1, PortDomain::sameAsNode},
            {PortId{"envelope"}, "Envelope", PortDirection::output, PortKind::control, 1,
             PortDomain::sameAsNode},
        },
        .parameters = {},
        .breaksDependencyCycle = false,
    };
}

NodeSchema gainSchema() {
    return {
        .typeId = NodeTypeId{"nod.gain"},
        .schemaVersion = 1,
        .displayName = "Gain",
        .category = "Audio",
        .sourceId = "nodsynth",
        .scope = NodeScope::perVoice,
        .ports = {
            {PortId{"audio-in"}, "Audio In", PortDirection::input, PortKind::audio, 1,
             PortDomain::sameAsNode},
            {PortId{"gain"}, "Gain", PortDirection::input, PortKind::control, 1,
             PortDomain::sameAsNode},
            {PortId{"audio-out"}, "Audio Out", PortDirection::output, PortKind::audio, 1,
             PortDomain::sameAsNode},
        },
        .parameters = {},
        .breaksDependencyCycle = false,
    };
}

NodeSchema voiceMixSchema() {
    return {
        .typeId = NodeTypeId{"nod.voice-mix"},
        .schemaVersion = 1,
        .displayName = "Voice Mix",
        .category = "Audio",
        .sourceId = "nodsynth",
        .scope = NodeScope::global,
        .ports = {
            {PortId{"voices"}, "Voices", PortDirection::input, PortKind::audio, 1,
             PortDomain::perVoice},
            {PortId{"audio"}, "Audio", PortDirection::output, PortKind::audio, 2,
             PortDomain::sameAsNode},
        },
        .parameters = {},
        .breaksDependencyCycle = false,
    };
}

NodeSchema audioOutputSchema() {
    return {
        .typeId = NodeTypeId{"nod.audio-output"},
        .schemaVersion = 1,
        .displayName = "Audio Output",
        .category = "Output",
        .sourceId = "nodsynth",
        .scope = NodeScope::global,
        .ports = {
            {PortId{"audio"}, "Audio", PortDirection::input, PortKind::audio, 2,
             PortDomain::sameAsNode},
        },
        .parameters = {},
        .breaksDependencyCycle = false,
    };
}

NodeRecord node(std::string id, std::string type) {
    return {NodeId{std::move(id)}, NodeTypeId{std::move(type)}, 1, {}, "{}", {}};
}

Connection connection(
    std::string fromNode,
    std::string fromPort,
    std::string toNode,
    std::string toPort) {
    return {
        {NodeId{std::move(fromNode)}, PortId{std::move(fromPort)}},
        {NodeId{std::move(toNode)}, PortId{std::move(toPort)}},
    };
}

SchemaRegistry makeRegistry() { return nodsynth::nodes::builtinRegistry(); }

GraphSnapshot makeValidGraph() {
    return {
        {
            node("midi", "nod.midi-input"),
            node("note-to-frequency", "nod.note-to-frequency"),
            node("oscillator", "nod.oscillator"),
            node("envelope", "nod.adsr"),
            node("gain", "nod.gain"),
            node("voice-mix", "nod.voice-mix"),
            node("audio-output", "nod.audio-output"),
        },
        {
            connection("midi", "note", "note-to-frequency", "note"),
            connection("note-to-frequency", "frequency", "oscillator", "frequency"),
            connection("midi", "gate", "envelope", "gate"),
            connection("oscillator", "audio", "gain", "audio-in"),
            connection("envelope", "envelope", "gain", "gain"),
            connection("gain", "audio-out", "voice-mix", "voices"),
            connection("voice-mix", "audio", "audio-output", "audio"),
        },
    };
}

Scenario validScenario() {
    return {
        .graph = makeValidGraph(),
        .registry = makeRegistry(),
        .expectSuccess = true,
        .expectedCode = DiagnosticCode::cycleDetected,
        .expectedNodeId = std::nullopt,
        .expectedPortId = std::nullopt,
    };
}
} // namespace

Scenario makeScenario(std::string_view name) {
    auto scenario = validScenario();
    if (name == "valid") return scenario;

    scenario.expectSuccess = false;
    if (name == "type-error") {
        scenario.graph.connections[2] = connection("midi", "note", "envelope", "gate");
        scenario.expectedCode = DiagnosticCode::portKindMismatch;
        scenario.expectedNodeId = NodeId{"envelope"};
        scenario.expectedPortId = PortId{"gate"};
        return scenario;
    }
    if (name == "cycle") {
        scenario.graph.connections[3] = connection("gain", "audio-out", "gain", "audio-in");
        scenario.expectedCode = DiagnosticCode::cycleDetected;
        scenario.expectedNodeId = std::nullopt;
        scenario.expectedPortId = std::nullopt;
        return scenario;
    }
    throw std::invalid_argument{"unknown graphcheck scenario"};
}

DiagnosticCode expectedDiagnostic(std::string_view name) {
    if (name == "type-error") return DiagnosticCode::portKindMismatch;
    if (name == "cycle") return DiagnosticCode::cycleDetected;
    throw std::invalid_argument{"scenario does not expect a diagnostic"};
}

bool containsExpectedDiagnostic(const CompileResult& result, const Scenario& scenario) {
    return std::ranges::any_of(result.diagnostics, [&](const Diagnostic& diagnostic) {
        return diagnostic.code == scenario.expectedCode && diagnostic.nodeId == scenario.expectedNodeId &&
               diagnostic.portId == scenario.expectedPortId;
    });
}

std::string_view diagnosticName(DiagnosticCode code) noexcept {
    switch (code) {
        case DiagnosticCode::duplicateNodeId: return "duplicate-node-id";
        case DiagnosticCode::missingNodeType: return "missing-node-type";
        case DiagnosticCode::unsupportedSchemaVersion: return "unsupported-schema-version";
        case DiagnosticCode::missingEndpointNode: return "missing-endpoint-node";
        case DiagnosticCode::missingPort: return "missing-port";
        case DiagnosticCode::wrongPortDirection: return "wrong-port-direction";
        case DiagnosticCode::portKindMismatch: return "port-kind-mismatch";
        case DiagnosticCode::channelCountMismatch: return "channel-count-mismatch";
        case DiagnosticCode::domainMismatch: return "domain-mismatch";
        case DiagnosticCode::duplicateInputConnection: return "duplicate-input-connection";
        case DiagnosticCode::unknownParameter: return "unknown-parameter";
        case DiagnosticCode::parameterOutOfRange: return "parameter-out-of-range";
        case DiagnosticCode::cycleDetected: return "cycle-detected";
        case DiagnosticCode::resourceLimitExceeded: return "resource-limit-exceeded";
    }
    return "unknown";
}
} // namespace nodsynth::graphcheck
