#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nodsynth/model/Identifiers.h>

namespace nodsynth::model {
enum class PortDirection { input, output };
enum class PortKind { audio, control, gate, note };
enum class NodeScope { perVoice, global };
enum class PortDomain { sameAsNode, perVoice, global };
enum class ParameterScale { linear, logarithmic };

struct PortSchema {
    PortId id;
    std::string displayName;
    PortDirection direction;
    PortKind kind;
    std::uint32_t channels;
    PortDomain domain;
};

struct ParameterSchema {
    ParameterId id;
    std::string displayName;
    std::string unit;
    double minimum;
    double maximum;
    double defaultValue;
    ParameterScale scale;
    bool modulatable;
};

struct NodeSchema {
    NodeTypeId typeId;
    std::uint32_t schemaVersion;
    std::string displayName;
    std::string category;
    std::string sourceId;
    NodeScope scope;
    std::vector<PortSchema> ports;
    std::vector<ParameterSchema> parameters;
    bool breaksDependencyCycle;
};
} // namespace nodsynth::model
