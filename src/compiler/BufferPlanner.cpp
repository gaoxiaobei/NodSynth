#include "BufferPlanner.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace nodsynth::compiler::detail {
namespace {
struct LiveOutput {
    model::Endpoint endpoint;
    std::uint32_t channels;
    std::size_t productionIndex;
    std::size_t finalConsumerIndex;
    bool dedicated;
};

struct ActiveSlot {
    std::uint32_t slot;
    std::uint32_t channels;
    std::size_t finalConsumerIndex;
    bool reusable;
};

struct NamespacePlan {
    std::vector<BufferAssignment> assignments;
    std::uint64_t physicalChannels{};
    bool physicalChannelCountOverflow{};
};

struct ChannelUsage {
    std::uint64_t channels{};
    bool overflow{};
};

void checkedAdd(std::uint64_t& total, std::uint64_t amount, bool& overflow) {
    if (amount > std::numeric_limits<std::uint64_t>::max() - total) {
        total = std::numeric_limits<std::uint64_t>::max();
        overflow = true;
        return;
    }
    total += amount;
}

NamespacePlan planNamespace(
    std::vector<LiveOutput> outputs,
    model::NodeScope scope,
    bool singleChannel) {
    std::ranges::sort(outputs, [](const LiveOutput& left, const LiveOutput& right) {
        if (left.productionIndex != right.productionIndex) {
            return left.productionIndex < right.productionIndex;
        }
        return left.endpoint < right.endpoint;
    });

    NamespacePlan result;
    std::vector<ActiveSlot> active;
    std::map<std::uint32_t, std::set<std::uint32_t>> freeSlotsByChannels;
    std::uint32_t nextSlot = 0;
    std::size_t currentProduction = std::numeric_limits<std::size_t>::max();

    for (const auto& output : outputs) {
        if (output.productionIndex != currentProduction) {
            currentProduction = output.productionIndex;
            auto activeSlot = active.begin();
            while (activeSlot != active.end()) {
                if (activeSlot->finalConsumerIndex < currentProduction) {
                    if (activeSlot->reusable) {
                        freeSlotsByChannels[activeSlot->channels].insert(activeSlot->slot);
                    }
                    activeSlot = active.erase(activeSlot);
                } else {
                    ++activeSlot;
                }
            }
        }

        const auto channels = singleChannel ? std::uint32_t{1} : output.channels;
        auto& freeSlots = freeSlotsByChannels[channels];
        std::uint32_t slot{};
        if (!output.dedicated && !freeSlots.empty()) {
            slot = *freeSlots.begin();
            freeSlots.erase(freeSlots.begin());
        } else {
            slot = nextSlot++;
            checkedAdd(
                result.physicalChannels,
                channels,
                result.physicalChannelCountOverflow);
        }

        result.assignments.push_back({output.endpoint, slot, channels, scope});
        active.push_back({slot, channels, output.finalConsumerIndex, !output.dedicated});
    }
    return result;
}

std::pair<std::vector<LiveOutput>, std::vector<LiveOutput>> collectLiveOutputs(
    const model::GraphSnapshot& graph,
    const model::SchemaRegistry& registry,
    const std::vector<model::NodeId>& order) {
    std::map<model::NodeId, const model::NodeRecord*> nodes;
    for (const auto& node : graph.nodes) nodes.emplace(node.id, &node);

    std::map<model::NodeId, std::size_t> scheduleIndex;
    for (std::size_t index = 0; index < order.size(); ++index) {
        scheduleIndex.emplace(order[index], index);
    }

    std::map<model::Endpoint, std::size_t> finalConsumers;
    std::set<model::Endpoint> dedicatedOutputs;
    for (const auto& connection : graph.connections) {
        if (!scheduleIndex.contains(connection.from.nodeId)) continue;

        const auto targetNode = nodes.find(connection.to.nodeId);
        const auto* targetSchema = targetNode == nodes.end() ? nullptr : registry.find(targetNode->second->typeId);
        const auto consumer = scheduleIndex.find(connection.to.nodeId);
        const auto consumerIndex = targetSchema != nullptr && targetSchema->breaksDependencyCycle
                                       ? order.size()
                                       : consumer == scheduleIndex.end() ? order.size() : consumer->second;
        if (targetSchema != nullptr && targetSchema->breaksDependencyCycle) {
            dedicatedOutputs.insert(connection.from);
        }
        const auto [found, inserted] = finalConsumers.emplace(connection.from, consumerIndex);
        if (!inserted) found->second = std::max(found->second, consumerIndex);
    }

    std::vector<LiveOutput> audio;
    std::vector<LiveOutput> control;
    for (std::size_t productionIndex = 0; productionIndex < order.size(); ++productionIndex) {
        const auto node = nodes.find(order[productionIndex]);
        if (node == nodes.end()) continue;
        const auto* schema = registry.find(node->second->typeId);
        if (schema == nullptr) continue;

        for (const auto& port : schema->ports) {
            if (port.direction != model::PortDirection::output) continue;
            if (port.kind != model::PortKind::audio && port.kind != model::PortKind::control) continue;

            const model::Endpoint endpoint{order[productionIndex], port.id};
            const auto finalConsumer = finalConsumers.find(endpoint);
            const auto finalConsumerIndex = finalConsumer == finalConsumers.end()
                                                ? productionIndex
                                                : std::max(productionIndex, finalConsumer->second);
            auto& outputs = port.kind == model::PortKind::audio ? audio : control;
            outputs.push_back({
                endpoint,
                port.channels,
                productionIndex,
                finalConsumerIndex,
                dedicatedOutputs.contains(endpoint),
            });
        }
    }
    return {std::move(audio), std::move(control)};
}

ChannelUsage planSchedule(
    PlannedBuffers& result,
    const model::GraphSnapshot& graph,
    const model::SchemaRegistry& registry,
    const std::vector<model::NodeId>& order,
    model::NodeScope scope) {
    auto [audioOutputs, controlOutputs] = collectLiveOutputs(graph, registry, order);
    auto audio = planNamespace(std::move(audioOutputs), scope, false);
    auto control = planNamespace(std::move(controlOutputs), scope, true);

    result.audio.insert(
        result.audio.end(),
        std::make_move_iterator(audio.assignments.begin()),
        std::make_move_iterator(audio.assignments.end()));
    result.control.insert(
        result.control.end(),
        std::make_move_iterator(control.assignments.begin()),
        std::make_move_iterator(control.assignments.end()));
    ChannelUsage usage{
        .channels = audio.physicalChannels,
        .overflow = audio.physicalChannelCountOverflow || control.physicalChannelCountOverflow,
    };
    checkedAdd(usage.channels, control.physicalChannels, usage.overflow);
    return usage;
}
} // namespace

PlannedBuffers planBuffers(
    const model::GraphSnapshot& graph,
    const model::SchemaRegistry& registry,
    const std::vector<model::NodeId>& perVoiceOrder,
    const std::vector<model::NodeId>& globalOrder) {
    PlannedBuffers result;
    const auto perVoice =
        planSchedule(result, graph, registry, perVoiceOrder, model::NodeScope::perVoice);
    const auto global =
        planSchedule(result, graph, registry, globalOrder, model::NodeScope::global);
    result.perVoicePhysicalChannels = perVoice.channels;
    result.globalPhysicalChannels = global.channels;
    result.physicalChannelCountOverflow = perVoice.overflow || global.overflow;
    std::ranges::sort(result.audio, {}, &BufferAssignment::output);
    std::ranges::sort(result.control, {}, &BufferAssignment::output);
    return result;
}
} // namespace nodsynth::compiler::detail
