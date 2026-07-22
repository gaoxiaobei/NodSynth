#pragma once

#include <utility>

#include <nodsynth/model/GraphDocument.h>
#include <nodsynth/model/SchemaRegistry.h>

namespace nodsynth::compiler::test {
using namespace nodsynth::model;

inline NodeSchema audioSourceSchema() {
    return {
        .typeId = NodeTypeId{"test.audio-source"},
        .schemaVersion = 1,
        .displayName = "Audio Source",
        .category = "Test",
        .sourceId = "test",
        .scope = NodeScope::global,
        .ports = {
            {PortId{"audio"}, "Audio", PortDirection::output, PortKind::audio, 1, PortDomain::sameAsNode},
        },
        .parameters = {},
        .breaksDependencyCycle = false,
    };
}

inline NodeSchema controlSinkSchema() {
    return {
        .typeId = NodeTypeId{"test.control-sink"},
        .schemaVersion = 1,
        .displayName = "Control Sink",
        .category = "Test",
        .sourceId = "test",
        .scope = NodeScope::global,
        .ports = {
            {PortId{"frequency"}, "Frequency", PortDirection::input, PortKind::control, 1,
             PortDomain::sameAsNode},
        },
        .parameters = {},
        .breaksDependencyCycle = false,
    };
}

inline NodeSchema audioSinkSchema() {
    return {
        .typeId = NodeTypeId{"test.audio-sink"},
        .schemaVersion = 1,
        .displayName = "Audio Sink",
        .category = "Test",
        .sourceId = "test",
        .scope = NodeScope::global,
        .ports = {
            {PortId{"audio"}, "Audio", PortDirection::input, PortKind::audio, 1, PortDomain::sameAsNode},
        },
        .parameters = {},
        .breaksDependencyCycle = false,
    };
}

inline NodeSchema stereoAudioSinkSchema() {
    auto schema = audioSinkSchema();
    schema.typeId = NodeTypeId{"test.stereo-audio-sink"};
    schema.ports.front().channels = 2;
    return schema;
}

inline NodeSchema parameterNodeSchema() {
    return {
        .typeId = NodeTypeId{"test.parameter-node"},
        .schemaVersion = 1,
        .displayName = "Parameter Node",
        .category = "Test",
        .sourceId = "test",
        .scope = NodeScope::global,
        .ports = {},
        .parameters = {
            {ParameterId{"gain"}, "Gain", "", 0.0, 1.0, 0.5, ParameterScale::linear, false},
        },
        .breaksDependencyCycle = false,
    };
}

inline std::pair<SchemaRegistry, GraphSnapshot> testGraphWithAudioSourceAndControlSink() {
    SchemaRegistry registry;
    registry.registerSchema(audioSourceSchema());
    registry.registerSchema(controlSinkSchema());

    GraphSnapshot graph{
        {
            {NodeId{"source"}, NodeTypeId{"test.audio-source"}, 1, {}, "{}", {}},
            {NodeId{"sink"}, NodeTypeId{"test.control-sink"}, 1, {}, "{}", {}},
        },
        {
            {{NodeId{"source"}, PortId{"audio"}}, {NodeId{"sink"}, PortId{"frequency"}}},
        },
    };
    return {std::move(registry), std::move(graph)};
}

} // namespace nodsynth::compiler::test
