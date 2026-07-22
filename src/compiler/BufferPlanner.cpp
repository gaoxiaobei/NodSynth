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
};

struct ActiveSlot {
    std::uint32_t slot;
    std::uint32_t channels;
    std::size_t finalConsumerIndex;
};

struct NamespacePlan {
    std::vector<BufferAssignment> assignments;
    std::uint32_t physicalChannels{};
};

std::uint32_t saturatedAdd(std::uint32_t left, std::uint32_t right) {
    if (right > std::numeric_limits<std::uint32_t>::max() - left) {
        return std::numeric_limits<std::uint32_t>::max();
    }
    return left + right;
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
                    freeSlotsByChannels[activeSlot->channels].insert(activeSlot->slot);
                    activeSlot = active.erase(activeSlot);
                } else {
                    ++activeSlot;
                }
            }
        }

        const auto channels = singleChannel ? std::uint32_t{1} : output.channels;
        auto& freeSlots = freeSlotsByChannels[channels];
        std::uint32_t slot{};
        if (!freeSlots.empty()) {
            slot = *freeSlots.begin();
            freeSlots.erase(freeSlots.begin());
        } else {
            slot = nextSlot++;
            result.physicalChannels = saturatedAdd(result.physicalChannels, channels);
        }

        result.assignments.push_back({output.endpoint, slot, channels, scope});
        active.push_back({slot, channels, output.finalConsumerIndex});
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
    for (const auto& connection : graph.connections) {
        if (!scheduleIndex.contains(connection.from.nodeId)) continue;

        const auto consumer = scheduleIndex.find(connection.to.nodeId);
        const auto consumerIndex = consumer == scheduleIndex.end() ? order.size() : consumer->second;
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
            outputs.push_back({endpoint, port.channels, productionIndex, finalConsumerIndex});
        }
    }
    return {std::move(audio), std::move(control)};
}

std::uint32_t planSchedule(
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
    return saturatedAdd(audio.physicalChannels, control.physicalChannels);
}
} // namespace

PlannedBuffers planBuffers(
    const model::GraphSnapshot& graph,
    const model::SchemaRegistry& registry,
    const std::vector<model::NodeId>& perVoiceOrder,
    const std::vector<model::NodeId>& globalOrder) {
    PlannedBuffers result;
    result.perVoicePhysicalChannels =
        planSchedule(result, graph, registry, perVoiceOrder, model::NodeScope::perVoice);
    result.globalPhysicalChannels =
        planSchedule(result, graph, registry, globalOrder, model::NodeScope::global);
    std::ranges::sort(result.audio, {}, &BufferAssignment::output);
    std::ranges::sort(result.control, {}, &BufferAssignment::output);
    return result;
}
} // namespace nodsynth::compiler::detail
