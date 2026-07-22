#pragma once

#include <optional>
#include <string>

#include <nodsynth/model/Identifiers.h>

namespace nodsynth::compiler {
enum class Severity { warning, error };

enum class DiagnosticCode {
    duplicateNodeId,
    missingNodeType,
    unsupportedSchemaVersion,
    missingEndpointNode,
    missingPort,
    wrongPortDirection,
    portKindMismatch,
    channelCountMismatch,
    domainMismatch,
    duplicateInputConnection,
    unknownParameter,
    parameterOutOfRange,
    cycleDetected,
    resourceLimitExceeded
};

struct Diagnostic {
    DiagnosticCode code;
    Severity severity;
    std::optional<model::NodeId> nodeId;
    std::optional<model::PortId> portId;
    std::string message;
};
} // namespace nodsynth::compiler
