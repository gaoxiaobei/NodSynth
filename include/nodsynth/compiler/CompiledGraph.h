#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include <nodsynth/compiler/Diagnostic.h>
#include <nodsynth/model/GraphDocument.h>
#include <nodsynth/model/NodeSchema.h>

namespace nodsynth::compiler {
struct BufferAssignment {
    model::Endpoint output;
    std::uint32_t slot;
    std::uint32_t channels;
    model::NodeScope domain;
};

struct CompiledGraph {
    struct Node {
        model::NodeRecord record;
        model::NodeScope scope;
    };

    struct Connection {
        model::Connection record;
        model::PortKind kind;
        std::uint32_t bufferSlot;
    };

    std::vector<Node> nodes;
    std::vector<Connection> connections;
    std::vector<model::NodeId> perVoiceOrder;
    std::vector<model::NodeId> globalOrder;
    std::vector<BufferAssignment> audioBuffers;
    std::vector<BufferAssignment> controlBuffers;
    std::uint32_t voiceBudget{0};
};

struct CompileResult {
    std::optional<CompiledGraph> graph;
    std::vector<Diagnostic> diagnostics;
};
} // namespace nodsynth::compiler
