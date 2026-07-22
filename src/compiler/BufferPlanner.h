#pragma once

#include <cstdint>
#include <vector>

#include <nodsynth/compiler/CompiledGraph.h>
#include <nodsynth/model/SchemaRegistry.h>

namespace nodsynth::compiler::detail {
struct PlannedBuffers {
    std::vector<BufferAssignment> audio;
    std::vector<BufferAssignment> control;
    std::uint64_t perVoicePhysicalChannels{};
    std::uint64_t globalPhysicalChannels{};
    bool physicalChannelCountOverflow{};
};

PlannedBuffers planBuffers(
    const model::GraphSnapshot& graph,
    const model::SchemaRegistry& registry,
    const std::vector<model::NodeId>& perVoiceOrder,
    const std::vector<model::NodeId>& globalOrder);
} // namespace nodsynth::compiler::detail
