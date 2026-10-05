#include <nodsynth/song/Automation.h>
#include <nodsynth/nodes/BuiltinNodes.h>

#include <algorithm>
#include <cmath>
#include <set>

namespace nodsynth::song {
std::vector<ParameterTarget> parameterTargets(const model::GraphSnapshot& graph) {
    const auto registry = nodes::builtinRegistry();
    std::vector<ParameterTarget> targets;
    for (const auto& node : graph.nodes) {
        const auto* schema = registry.find(node.typeId);
        if (!schema) continue;
        for (const auto& parameter : schema->parameters) {
            if (parameter.id.value != "cutoff" && parameter.id.value != "level") continue;
            // An input control cable overrides the corresponding DSP parameter.
            if (std::any_of(graph.connections.begin(), graph.connections.end(), [&](const auto& wire) {
                return wire.to.nodeId == node.id && wire.to.portId.value == parameter.id.value;
            })) continue;
            const auto value = node.parameters.find(parameter.id);
            targets.push_back({node.id.value + "/" + parameter.id.value, node.id, parameter.id,
                parameter.id.value == "level" ? "linear-amplitude" : parameter.unit, parameter.minimum, parameter.maximum,
                value == node.parameters.end() ? parameter.defaultValue : value->second,
                parameter.id.value == "level" && (node.typeId.value == "nod.oscillator" || node.typeId.value == "nod.noise")});
        }
    }
    return targets;
}

persist::Json parametersJson(const model::GraphSnapshot& graph) {
    using persist::Json;
    auto items = Json::array();
    const auto targets = parameterTargets(graph);
    for (const auto& target : targets) {
        auto item = Json::object(); item.set("id", Json::string(target.id));
        item.set("unit", Json::string(target.unit)); item.set("valueDomain", Json::string("physical"));
        item.set("minimum", Json::number(target.minimum)); item.set("maximum", Json::number(target.maximum));
        item.set("base", Json::number(target.base)); item.set("automatable", Json::boolean(true)); items.push(std::move(item));
    }
    for (const auto* macro : {"brightness", "level"}) {
        auto mapping = Json::array();
        for (const auto& target : targets) if (std::string(macro) == "brightness" ? target.parameter.value == "cutoff" : target.sourceLevel) {
            auto item = Json::object(); item.set("target", Json::string(target.id));
            item.set("minimum", Json::number(target.minimum)); item.set("maximum", Json::number(target.maximum)); mapping.push(std::move(item));
        }
        if (mapping.asArray().empty()) continue;
        auto item = Json::object(); item.set("id", Json::string(std::string("macro:") + macro));
        item.set("unit", Json::string("normalized")); item.set("valueDomain", Json::string("normalized"));
        item.set("minimum", Json::number(0)); item.set("maximum", Json::number(1));
        item.set("automatable", Json::boolean(true)); item.set("mapping", std::move(mapping)); items.push(std::move(item));
    }
    return items;
}

bool prepareAutomation(const SongDocument& song, const Track& track, const model::GraphSnapshot& graph,
    const runtime::RuntimePlan* plan, std::uint32_t rate, std::vector<PreparedLane>& lanes, std::string& error) {
    const auto targets = parameterTargets(graph);
    const auto matching = [&](const std::string& id) {
        std::vector<ParameterTarget> selected;
        for (const auto& target : targets)
            if (target.id == id || (id == "macro:brightness" && target.parameter.value == "cutoff") ||
                (id == "macro:level" && target.sourceLevel)) selected.push_back(target);
        return selected;
    };
    std::map<std::string, double> baseValues;
    for (const auto& target : targets) baseValues[target.id] = target.base;
    for (const auto& [id, value] : track.parameterValues) {
        const auto selected = matching(id);
        if (selected.empty()) { error = "invalid parameter address: " + id; return false; }
        for (const auto& target : selected) {
            const bool macro = id.starts_with("macro:");
            if (!std::isfinite(value) || value < (macro ? 0 : target.minimum) || value > (macro ? 1 : target.maximum)) {
                error = "base parameter value is out of range: " + id; return false;
            }
            baseValues[target.id] = macro ? target.minimum + value * (target.maximum - target.minimum) : value;
        }
    }
    std::set<std::string> automated;
    for (const auto& lane : track.parameterAutomation) {
        const auto selected = matching(lane.id);
        if (selected.empty()) { error = "invalid automation address: " + lane.id; return false; }
        const bool macro = lane.id.starts_with("macro:");
        if (lane.valueDomain != (macro ? "normalized" : "physical") ||
            (lane.interpolation != "linear" && lane.interpolation != "step")) {
            error = "automation unit or interpolation does not match address: " + lane.id; return false;
        }
        for (const auto& target : selected) {
            if (!automated.insert(target.id).second) { error = "multiple lanes address the same parameter: " + target.id; return false; }
            PreparedLane output; output.base = baseValues[target.id]; output.linear = lane.interpolation == "linear";
            if (plan) {
                const auto index = plan->findParameter(target.node, target.parameter);
                if (!index) { error = "parameter is absent from prepared patch"; return false; }
                output.index = *index;
            }
            for (const auto& point : lane.points) {
                const auto time = sampleAtTick(song, point.tick, rate);
                if (!time || !std::isfinite(point.value) || point.value < (macro ? 0 : target.minimum) || point.value > (macro ? 1 : target.maximum) ||
                    (!output.points.empty() && *time <= output.points.back().sample)) {
                    error = "automation points must be in range and have strictly increasing sample times"; return false;
                }
                output.points.push_back({*time, macro ? target.minimum + point.value * (target.maximum - target.minimum) : point.value});
            }
            lanes.push_back(std::move(output));
        }
    }
    for (const auto& [id, value] : baseValues) {
        if (automated.contains(id)) continue;
        const auto target = matching(id).front();
        PreparedLane output; output.base = value;
        if (plan) output.index = *plan->findParameter(target.node, target.parameter);
        // Only materialize overrides; unchanged parameters remain in their patch state.
        if (value != target.base) lanes.push_back(std::move(output));
    }
    return true;
}

double parameterAt(const PreparedLane& lane, std::int64_t sample) {
    if (lane.points.empty() || sample < lane.points.front().sample) return lane.base;
    const auto upper = std::upper_bound(lane.points.begin(), lane.points.end(), sample,
        [](auto time, const auto& point) { return time < point.sample; });
    const auto& lower = *(upper - 1);
    if (!lane.linear || upper == lane.points.end()) return lower.value;
    return lower.value + (upper->value - lower.value) * static_cast<double>(sample - lower.sample) / static_cast<double>(upper->sample - lower.sample);
}
} // namespace nodsynth::song
