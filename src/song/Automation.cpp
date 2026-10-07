#include <nodsynth/song/Automation.h>
#include <nodsynth/nodes/BuiltinNodes.h>

#include <algorithm>
#include <cmath>
#include <set>
#include <numbers>

namespace nodsynth::song {
bool validateMacros(const model::GraphSnapshot& graph,std::string& error) {
    const auto targets=parameterTargets(graph);std::set<std::string> names;
    if(graph.macros.size()>64) {error="patch-macro: at most 64 macros";return false;}
    for(const auto& macro:graph.macros) {
        if(macro.id.empty() || macro.id.size()>64 || macro.id=="brightness" || macro.id=="level" ||
            macro.id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_")!=std::string::npos || !names.insert(macro.id).second ||
            macro.mappings.empty() || macro.mappings.size()>64) {error="patch-macro: invalid, reserved or duplicate identity";return false;}
        std::set<std::string> mapped;
        for(const auto& mapping:macro.mappings) {
            const auto address=mapping.node.value+"/"+mapping.parameter.value;
            const auto target=std::find_if(targets.begin(),targets.end(),[&](const auto& item){return item.id==address;});
            if(target==targets.end() || !mapped.insert(address).second || !std::isfinite(mapping.minimum) || !std::isfinite(mapping.maximum) ||
                mapping.minimum<target->minimum || mapping.minimum>target->maximum || mapping.maximum<target->minimum || mapping.maximum>target->maximum ||
                (mapping.curve!="linear" && mapping.curve!="log") || (mapping.curve=="log" && (mapping.minimum<=0 || mapping.maximum<=0))) {
                error="patch-macro: invalid target, range or curve: "+address;return false;
            }
        }
    }return true;
}

namespace {
const model::GraphSnapshot::Macro::Mapping* macroMapping(const model::GraphSnapshot& graph,const std::string& id,const std::string& address) {
    for(const auto& macro:graph.macros) if(id=="macro:"+macro.id)
        for(const auto& mapping:macro.mappings) if(address==mapping.node.value+"/"+mapping.parameter.value) return &mapping;
    return nullptr;
}
double mappedMacro(const model::GraphSnapshot& graph,const std::string& id,const ParameterTarget& target,double value) {
    const auto* mapping=macroMapping(graph,id,target.id);
    if(!mapping) return target.minimum+value*(target.maximum-target.minimum);
    return mapping->curve=="log"?mapping->minimum*std::pow(mapping->maximum/mapping->minimum,value):mapping->minimum+value*(mapping->maximum-mapping->minimum);
}
}
std::vector<ParameterTarget> parameterTargets(const model::GraphSnapshot& graph) {
    const auto registry = nodes::builtinRegistry();
    std::vector<ParameterTarget> targets;
    for (const auto& node : graph.nodes) {
        const auto* schema = registry.find(node.typeId);
        if (!schema) continue;
        for (const auto& parameter : schema->parameters) {
            const auto value = node.parameters.find(parameter.id);
            ParameterTarget target{node.id.value + "/" + parameter.id.value, node.id, parameter.id,
                parameter.id.value == "level" ? "linear-amplitude" : parameter.unit, parameter.minimum, parameter.maximum,
                value == node.parameters.end() ? parameter.defaultValue : value->second,
                parameter.id.value == "level" && (node.typeId.value == "nod.oscillator" || node.typeId.value == "nod.noise" || node.typeId.value == "nod.unison-v2")};
            target.automatable = parameter.modulatable;
            if (!target.automatable) target.reason = "unsupported-target";
            if (node.schemaVersion != schema->schemaVersion) { target.automatable = false; target.reason = "unsupported-target"; }
            const auto controlPort = parameter.modulationPort.empty() ? parameter.id.value : parameter.modulationPort;
            if (parameter.modulationMode == model::ModulationMode::replace)
                for (const auto& wire : graph.connections)
                    if (wire.to.nodeId == node.id && wire.to.portId.value == controlPort)
                        target.blockingConnections.push_back(wire);
            if (!target.blockingConnections.empty()) { target.automatable = false; target.reason = "control-override"; }
            targets.push_back(std::move(target));
        }
    }
    return targets;
}

persist::Json parametersJson(const model::GraphSnapshot& graph) {
    using persist::Json;
    auto items = Json::array();
    const auto targets = parameterTargets(graph);
    const auto registry = nodes::builtinRegistry();
    for (const auto& target : targets) {
        auto item = Json::object(); item.set("id", Json::string(target.id));
        item.set("unit", Json::string(target.unit)); item.set("valueDomain", Json::string("physical"));
        item.set("minimum", Json::number(target.minimum)); item.set("maximum", Json::number(target.maximum));
        item.set("base", Json::number(target.base)); item.set("automatable", Json::boolean(target.automatable));
        for (const auto& node : graph.nodes) if (node.id == target.node) {
            const auto* schema = registry.find(node.typeId);
            for (const auto& parameter : schema->parameters) if (parameter.id == target.parameter) {
                item.set("schemaVersion", Json::number(schema->schemaVersion));
                if (node.typeId.value == "nod.unison-v2" && parameter.id.value == "voices") {
                    item.set("notePolyphonyBudget", Json::number(runtime::kMaxVoices));
                    item.set("oscillatorsPerNote", Json::number(std::lround(target.base)));
                    item.set("subOscillatorBudget", Json::number(runtime::kMaxVoices * std::lround(target.base)));
                    item.set("switching", Json::string("preallocated 8 oscillators/note; live integer knob changes; no lane"));
                }
                item.set("modulationMode", Json::string(parameter.modulationMode == model::ModulationMode::octave ? "octave" :
                    parameter.modulationMode == model::ModulationMode::multiply ? "multiply" : "replace"));
                item.set("modulationPort", Json::string(parameter.modulationPort.empty() ? parameter.id.value : parameter.modulationPort));
                if (parameter.modulationMode == model::ModulationMode::octave) {
                    item.set("depthParameter", Json::string(node.id.value + "/" + parameter.depthParameter));
                    item.set("polarity", Json::string("bipolar -1..1, clamped"));
                    item.set("mapping", Json::string("clamp(baseHz * 2^(depthOctaves * modulation), 20, min(20000, 0.45 * sampleRate))"));
                    item.set("audioRateModulation", Json::boolean(true));
                }
            }
        }
        if (!target.automatable) {
            item.set("reason", Json::string(target.reason));
            item.set("repair", Json::string(target.reason == "control-override" ?
                "Disconnect the replace cable, automate its source, or explicitly migrate to a base+modulation node." :
                "Edit the patch value before prepare; this target does not support a lane."));
            auto wires = Json::array();
            for (const auto& wire : target.blockingConnections) {
                auto cable = Json::object();
                cable.set("from", Json::string(wire.from.nodeId.value + "/" + wire.from.portId.value));
                cable.set("to", Json::string(wire.to.nodeId.value + "/" + wire.to.portId.value));
                wires.push(std::move(cable));
            }
            item.set("blockingConnections", std::move(wires));
        }
        items.push(std::move(item));
    }
    for (const auto* macro : {"brightness", "level"}) {
        auto mapping = Json::array();
        for (const auto& target : targets) if (target.automatable && (std::string(macro) == "brightness" ? target.parameter.value == "cutoff" : target.sourceLevel)) {
            auto item = Json::object(); item.set("target", Json::string(target.id));
            item.set("minimum", Json::number(target.minimum)); item.set("maximum", Json::number(target.maximum)); mapping.push(std::move(item));
        }
        if (mapping.asArray().empty()) continue;
        auto item = Json::object(); item.set("id", Json::string(std::string("macro:") + macro));
        item.set("unit", Json::string("normalized")); item.set("valueDomain", Json::string("normalized"));
        item.set("minimum", Json::number(0)); item.set("maximum", Json::number(1));
        item.set("automatable", Json::boolean(true)); item.set("mapping", std::move(mapping)); items.push(std::move(item));
    }
    for(const auto& macro:graph.macros) {
        auto item=Json::object(),mappings=Json::array();bool available=true;
        item.set("id",Json::string("macro:"+macro.id));item.set("unit",Json::string("normalized"));item.set("valueDomain",Json::string("normalized"));
        item.set("minimum",Json::number(0));item.set("maximum",Json::number(1));
        for(const auto& mapping:macro.mappings) {
            auto entry=Json::object();const auto address=mapping.node.value+"/"+mapping.parameter.value;
            entry.set("target",Json::string(address));entry.set("minimum",Json::number(mapping.minimum));entry.set("maximum",Json::number(mapping.maximum));
            entry.set("depth",Json::number(mapping.maximum-mapping.minimum));entry.set("curve",Json::string(mapping.curve));
            const auto target=std::find_if(targets.begin(),targets.end(),[&](const auto& t){return t.id==address;});
            if(target==targets.end() || !target->automatable) {available=false;entry.set("reason",Json::string(target==targets.end()?"unknown-address":target->reason));}
            mappings.push(std::move(entry));
        }
        item.set("automatable",Json::boolean(available));item.set("mapping",std::move(mappings));items.push(std::move(item));
    }
    return items;
}

bool prepareAutomation(const SongDocument& song, const Track& track, const model::GraphSnapshot& graph,
    const runtime::RuntimePlan* plan, std::uint32_t rate, std::vector<PreparedLane>& lanes, std::string& error) {
    const auto targets = parameterTargets(graph);
    if(!validateMacros(graph,error)) return false;
    const auto matching = [&](const std::string& id) {
        std::vector<ParameterTarget> selected;
        for (const auto& target : targets)
            if (target.id == id || macroMapping(graph,id,target.id) || (target.automatable && ((id == "macro:brightness" && target.parameter.value == "cutoff") ||
                (id == "macro:level" && target.sourceLevel)))) selected.push_back(target);
        return selected;
    };
    std::map<std::string, double> baseValues;
    for (const auto& target : targets) if (target.automatable) baseValues[target.id] = target.base;
    const auto check = [&](const std::string& id, const std::vector<ParameterTarget>& selected) {
        if (selected.empty()) { error = "unknown-address: " + id; return false; }
        for (const auto& target : selected) if (!target.automatable) {
            error = target.reason + ": " + id;
            for (const auto& wire : target.blockingConnections)
                error += " occupied by " + wire.from.nodeId.value + "/" + wire.from.portId.value;
            return false;
        }
        return true;
    };
    std::set<std::string> assigned;
    for (const auto& [id, value] : track.parameterValues) {
        const auto selected = matching(id);
        if (!check(id, selected)) return false;
        for (const auto& target : selected) {
            if (!assigned.insert(target.id).second) { error = "multiple base values address the same parameter: " + target.id; return false; }
            const bool macro = id.starts_with("macro:");
            if (!std::isfinite(value) || value < (macro ? 0 : target.minimum) || value > (macro ? 1 : target.maximum)) {
                error = "base parameter value is out of range: " + id; return false;
            }
            baseValues[target.id] = macro ? mappedMacro(graph,id,target,value) : value;
        }
    }
    std::set<std::string> automated;
    for (const auto& lane : track.parameterAutomation) {
        const auto selected = matching(lane.id);
        if (!check(lane.id, selected)) return false;
        const bool macro = lane.id.starts_with("macro:");
        if (lane.valueDomain != (macro ? "normalized" : "physical") ||
            (lane.interpolation != "linear" && lane.interpolation != "step")) {
            error = "automation unit or interpolation does not match address: " + lane.id; return false;
        }
        for (const auto& target : selected) {
            if (!automated.insert(target.id).second) { error = "multiple lanes address the same parameter: " + target.id; return false; }
            PreparedLane output; output.base = baseValues[target.id]; output.linear = lane.interpolation == "linear";
            const auto* mapping=macroMapping(graph,lane.id,target.id);output.logarithmic=mapping && mapping->curve=="log";
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
                output.points.push_back({*time, macro ? mappedMacro(graph,lane.id,target,point.value) : point.value});
            }
            lanes.push_back(std::move(output));
        }
    }
    for (const auto& [id, value] : baseValues) {
        if (automated.contains(id)) continue;
        const auto target = matching(id).front();
        PreparedLane output; output.base = value;
        if (plan) {
            const auto index = plan->findParameter(target.node, target.parameter);
            if (!index) { error = "parameter is absent from prepared patch"; return false; }
            output.index = *index;
        }
        // Only materialize overrides; unchanged parameters remain in their patch state.
        if (value != target.base) lanes.push_back(std::move(output));
    }
    for(const auto& node:graph.nodes) if(node.typeId.value=="nod.music-delay") {
        const auto id=node.id.value+"/tempoBpm";
        const auto syncId=node.id.value+"/syncBeats";
        double maxSync=baseValues[syncId],minTempo=baseValues[id];
        for(const auto& lane:track.parameterAutomation) for(const auto& target:matching(lane.id)) for(const auto& point:lane.points) {
            const double value=lane.id.starts_with("macro:")?mappedMacro(graph,lane.id,target,point.value):point.value;
            if(target.id==syncId) maxSync=std::max(maxSync,value);
            if(target.id==id) minTempo=std::min(minTempo,value);
        }
        if(!assigned.contains(id) && !automated.contains(id)) {
            minTempo=120;for(const auto& point:song.tempo) minTempo=std::min(minTempo,60000000.0/point.microsecondsPerQuarter);
        }
        if(maxSync*60/minTempo>8) {error="delay-time: graph tempo-sync exceeds 8-second buffer";return false;}
        if(assigned.contains(id) || automated.contains(id)) continue;
        PreparedLane hostTempo;hostTempo.base=120;hostTempo.linear=false;
        if(plan) {
            const auto index=plan->findParameter(node.id,model::ParameterId{"tempoBpm"});
            if(!index) {error="host tempo parameter is absent from prepared delay";return false;}hostTempo.index=*index;
        }
        for(const auto& point:song.tempo) {
            const double bpm=60000000.0/point.microsecondsPerQuarter;const auto sample=sampleAtTick(song,point.tick,rate);
            if(!sample || bpm<20 || bpm>400 || (!hostTempo.points.empty() && *sample<=hostTempo.points.back().sample)) {error="music delay host tempo must be 20..400 BPM with distinct sample times";return false;}
            hostTempo.points.push_back({*sample,bpm});
        }
        lanes.push_back(std::move(hostTempo));
    }
    return true;
}

double parameterAt(const PreparedLane& lane, std::int64_t sample) {
    if (lane.points.empty() || sample < lane.points.front().sample) return lane.base;
    const auto upper = std::upper_bound(lane.points.begin(), lane.points.end(), sample,
        [](auto time, const auto& point) { return time < point.sample; });
    const auto& lower = *(upper - 1);
    if (!lane.linear || upper == lane.points.end()) return lower.value;
    if(lane.logarithmic) return lower.value*std::pow(upper->value/lower.value,static_cast<double>(sample-lower.sample)/static_cast<double>(upper->sample-lower.sample));
    return lower.value + (upper->value - lower.value) * static_cast<double>(sample - lower.sample) / static_cast<double>(upper->sample - lower.sample);
}

std::string automationErrorCode(const std::string& error) {
    for (const auto* code : {"unknown-address", "unsupported-target", "control-override"})
        if (error.starts_with(std::string(code) + ":")) return code;
    return "unsupported-automation";
}

persist::Json productionMigrationPreview(const model::GraphSnapshot& graph) {
    using persist::Json;
    auto result = Json::object(), changes = Json::array(), requirements = Json::array();
    const auto registry = nodes::builtinRegistry();
    for (const auto& node : graph.nodes) {
        const auto* replacement = registry.find(model::NodeTypeId{node.typeId.value + "-v2"});
        if (!replacement) continue;
        auto item = Json::object();
        item.set("node", Json::string(node.id.value));
        item.set("fromType", Json::string(node.typeId.value));
        item.set("toType", Json::string(replacement->typeId.value));
        item.set("schemaVersion", Json::number(replacement->schemaVersion));
        item.set("audioChannels", Json::number(2));
        if (node.typeId.value == "nod.gain") item.set("gainSemantics", Json::string("base multiplied by cable; review existing base value"));
        changes.push(std::move(item));
        for (const auto& cable : graph.connections) if (cable.to.nodeId == node.id && cable.to.portId.value == "cutoff") {
            auto requirement = Json::object();
            requirement.set("code", Json::string("explicit-modulation-mapping-required"));
            requirement.set("from", Json::string(cable.from.nodeId.value + "/" + cable.from.portId.value));
            requirement.set("to", Json::string(node.id.value + "/cutoff-mod"));
            requirement.set("action", Json::string("Choose action normalized with sourceIsNormalized true, baseHz and depthOctaves, or action disconnect with baseHz. No automatic Hz normalization."));
            requirements.push(std::move(requirement));
        }
        for(const auto& cable:graph.connections) if(node.typeId.value=="nod.gain" && cable.to.nodeId==node.id && cable.to.portId.value=="gain") {
            auto requirement=Json::object();requirement.set("code",Json::string("explicit-gain-base-required"));requirement.set("node",Json::string(node.id.value));
            requirement.set("action",Json::string("Choose multiplier base explicitly; base 1 preserves the connected v1 gain signal."));requirements.push(std::move(requirement));
        }
    }
    for (const auto& cable : graph.connections) {
        const auto source = std::find_if(graph.nodes.begin(), graph.nodes.end(), [&](const auto& node) { return node.id == cable.from.nodeId; });
        const auto target = std::find_if(graph.nodes.begin(), graph.nodes.end(), [&](const auto& node) { return node.id == cable.to.nodeId; });
        if (source == graph.nodes.end() || target == graph.nodes.end()) continue;
        const auto* schema = registry.find(source->typeId);
        if (!schema || registry.find(model::NodeTypeId{source->typeId.value + "-v2"}) ||
            !registry.find(model::NodeTypeId{target->typeId.value + "-v2"})) continue;
        for (const auto& port : schema->ports) if (port.id == cable.from.portId && port.kind == model::PortKind::audio && port.channels == 1) {
            auto requirement = Json::object();
            requirement.set("code", Json::string("mono-to-stereo-required"));
            requirement.set("from", Json::string(source->id.value + "/" + port.id.value));
            requirement.set("to", Json::string(target->id.value + "/" + cable.to.portId.value));
            requirement.set("action", Json::string("Choose mono bridge pan and level explicitly; center uses equal power (-3 dB per side)."));
            requirements.push(std::move(requirement));
        }
    }
    result.set("status", Json::string("preview-only"));
    result.set("changes", std::move(changes));
    result.set("requirements", std::move(requirements));
    result.set("applied", Json::boolean(false));
    return result;
}

namespace {
persist::Json intervalsJson(const std::vector<TickInterval>& intervals) {
    auto items = persist::Json::array();
    for (const auto& interval : intervals) {
        auto item = persist::Json::object();
        item.set("startTick", persist::Json::number(interval.startTick));
        item.set("endTick", persist::Json::number(interval.endTick));
        items.push(std::move(item));
    }
    return items;
}

double intervalGate(const std::vector<PreparedTrackMix::Interval>& intervals, std::int64_t sample, std::int64_t fade) {
    double gate = 1;
    for (const auto& interval : intervals) {
        double current = 1;
        if (sample >= interval.start && sample < interval.end) current = 0;
        else if (fade > 0 && sample >= interval.start - fade && sample < interval.start)
            current = static_cast<double>(interval.start - sample) / fade;
        else if (fade > 0 && sample >= interval.end && sample < interval.end + fade)
            current = static_cast<double>(sample - interval.end) / fade;
        gate = std::min(gate, current);
    }
    return gate;
}
}

persist::Json mixControlsJson(const Track& track) {
    using persist::Json;
    auto result = Json::object();
    result.set("gainMode", Json::string(track.gainMode));
    result.set("panMode", Json::string(track.panMode));
    result.set("muteIntervals", intervalsJson(track.mute));
    result.set("muteFadeMs", Json::number(track.muteFadeMs));
    auto pump = Json::null();
    if (track.pump) {
        pump = Json::object();
        pump.set("startTick", Json::number(track.pump->startTick));
        pump.set("endTick", Json::number(track.pump->endTick));
        pump.set("period", Json::number(track.pump->period));
        pump.set("recovery", Json::number(track.pump->recovery));
        pump.set("depth", Json::number(track.pump->depth));
        pump.set("fadeMs", Json::number(track.pump->fadeMs));
        pump.set("skipIntervals", intervalsJson(track.pump->skip));
    }
    result.set("pump", std::move(pump));
    return result;
}

double PreparedTrackMix::gainAt(std::int64_t sample) const {
    double user = 0;
    if (multiply) user = parameterAt(gain, sample);
    else {
        // Preserve the v1-v3 float interpolation and absolute-lane rounding exactly.
        if (gain.points.empty()) user = static_cast<float>(gain.base);
        else if (sample <= gain.points.front().sample) user = static_cast<float>(gain.points.front().value);
        else if (sample >= gain.points.back().sample) user = static_cast<float>(gain.points.back().value);
        else {
            const auto upper = std::upper_bound(gain.points.begin(), gain.points.end(), sample,
                [](auto time, const auto& point) { return time < point.sample; });
            const auto& lower = *(upper - 1);
            const float from = static_cast<float>(lower.value), to = static_cast<float>(upper->value);
            const double position = static_cast<double>(sample - lower.sample) / (upper->sample - lower.sample);
            user = static_cast<float>(from + position * (to - from));
        }
    }
    if (dips.empty() && skip.empty() && mute.empty()) return multiply ? user * fader : user;
    double envelope = 1;
    const auto upper = std::upper_bound(dips.begin(), dips.end(), sample,
        [](auto time, const auto& dip) { return time < dip.start; });
    if (upper != dips.begin()) {
        const auto& dip = *(upper - 1);
        if (sample < dip.end) {
            const auto attack = std::min(pumpFade, (dip.recovery - dip.start) / 2);
            if (attack > 0 && sample < dip.start + attack)
                envelope = 1 - depth * static_cast<double>(sample - dip.start) / attack;
            else if (sample < dip.recovery)
                envelope = 1 - depth * static_cast<double>(dip.recovery - sample) / std::max<std::int64_t>(1, dip.recovery - dip.start - attack);
        }
    }
    // Skip disables pumping with a neutral gain; mute is an independent final gate.
    envelope = 1 + (envelope - 1) * intervalGate(skip, sample, pumpFade);
    return user * (multiply ? fader : 1) * envelope * intervalGate(mute, sample, muteFade);
}

void PreparedTrackMix::gainsAt(std::int64_t sample, float& left, float& right) const {
    const double gainValue = gainAt(sample);
    left = static_cast<float>(gainValue * panLeft);
    right = static_cast<float>(gainValue * panRight);
}

bool prepareTrackMix(const SongDocument& song, const Track& track, std::uint32_t rate, PreparedTrackMix& output, std::string& error) {
    if (!rate || !std::isfinite(track.gain) || track.gain < 0 || !std::isfinite(track.pan) || std::fabs(track.pan) > 1 ||
        (track.gainMode != "legacy" && track.gainMode != "multiply") ||
        (track.panMode != "balance" && track.panMode != "equal-power") ||
        !std::isfinite(track.muteFadeMs) || track.muteFadeMs < 0 || track.muteFadeMs > 1000) {
        error = "invalid mix controls"; return false;
    }
    PreparedTrackMix mix;
    mix.fader = track.gain; mix.pan = track.pan;
    mix.multiply = track.gainMode == "multiply"; mix.balance = track.panMode == "balance";
    if (mix.balance) {
        mix.panLeft = mix.pan > 0 ? std::cos(mix.pan * std::numbers::pi / 2) : 1;
        mix.panRight = mix.pan < 0 ? std::cos(-mix.pan * std::numbers::pi / 2) : 1;
    } else {
        const double angle = (mix.pan + 1) * std::numbers::pi / 4;
        mix.panLeft = std::cos(angle); mix.panRight = std::sin(angle);
    }
    mix.gain.base = mix.multiply ? 1 : track.gain;
    const auto time = [&](std::uint32_t tick, std::int64_t& sample) {
        const auto value = sampleAtTick(song, tick, rate);
        if (!value) { error = "mix control time does not fit in a sample index"; return false; }
        sample = *value; return true;
    };
    for (const auto& point : track.gainAutomation) {
        if (!std::isfinite(point.gain) || point.gain < 0) { error = "invalid gain point"; return false; }
        std::int64_t sample = 0;
        if (!time(point.tick, sample)) return false;
        if (!mix.gain.points.empty() && sample < mix.gain.points.back().sample) { error = "gain points must be sorted"; return false; }
        mix.gain.points.push_back({sample, point.gain});
    }
    const auto intervals = [&](const auto& ticks, auto& samples) {
        for (const auto& interval : ticks) {
            if (interval.startTick >= interval.endTick) { error = "invalid mix interval"; return false; }
            PreparedTrackMix::Interval span{};
            if (!time(interval.startTick, span.start) || !time(interval.endTick, span.end)) return false;
            if (!samples.empty() && span.start <= samples.back().end) { error = "mix intervals must be canonical and disjoint"; return false; }
            samples.push_back(span);
        }
        return true;
    };
    mix.muteFade = static_cast<std::int64_t>(std::llround(track.muteFadeMs * rate / 1000));
    if (!intervals(track.mute, mix.mute)) return false;
    if (track.pump) {
        const auto& pump = *track.pump;
        if (pump.period == 0 || pump.recovery == 0 || pump.recovery >= pump.period ||
            pump.endTick <= pump.startTick || (pump.endTick - pump.startTick) / pump.period > 100000 ||
            !std::isfinite(pump.depth) || pump.depth < 0 || pump.depth > 1 ||
            !std::isfinite(pump.fadeMs) || pump.fadeMs < 0 || pump.fadeMs > 1000) {
            error = "invalid pump timing"; return false;
        }
        mix.depth = pump.depth;
        mix.pumpFade = static_cast<std::int64_t>(std::llround(pump.fadeMs * rate / 1000));
        for (std::uint64_t tick = pump.startTick; tick < pump.endTick; tick += pump.period) {
            PreparedTrackMix::Dip dip{};
            if (!time(static_cast<std::uint32_t>(tick), dip.start) ||
                !time(static_cast<std::uint32_t>(std::min<std::uint64_t>(tick + pump.recovery, pump.endTick)), dip.recovery) ||
                !time(static_cast<std::uint32_t>(std::min<std::uint64_t>(tick + pump.period, pump.endTick)), dip.end)) return false;
            mix.dips.push_back(dip);
        }
        if (!intervals(pump.skip, mix.skip)) return false;
    }
    output = std::move(mix);
    return true;
}
} // namespace nodsynth::song
