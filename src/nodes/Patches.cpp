#include <nodsynth/nodes/BuiltinNodes.h>

namespace nodsynth::nodes {
namespace {
model::NodeRecord node(std::string id, std::string type, float x, float y) {
    model::NodeRecord record;
    record.id = model::NodeId{std::move(id)};
    record.typeId = model::NodeTypeId{std::move(type)};
    record.schemaVersion = 1;
    record.opaqueStateJson = "{}";
    record.extensionsJson = "{}";
    record.position = {x, y};
    return record;
}

model::Connection cable(std::string from, std::string fromPort, std::string to, std::string toPort) {
    return {{model::NodeId{std::move(from)}, model::PortId{std::move(fromPort)}},
            {model::NodeId{std::move(to)}, model::PortId{std::move(toPort)}}};
}

void set(model::NodeRecord& record, std::string id, double value) {
    record.parameters[model::ParameterId{std::move(id)}] = value;
}
} // namespace

model::GraphSnapshot sinePatch() {
    auto midi = node("midi", "nod.midi-input", 40.f, 120.f);
    auto note = node("note-to-frequency", "nod.note-to-frequency", 280.f, 40.f);
    auto osc = node("oscillator", "nod.oscillator", 540.f, 40.f);
    auto envelope = node("envelope", "nod.adsr", 540.f, 220.f);
    auto gain = node("gain", "nod.gain", 820.f, 120.f);
    auto mix = node("voice-mix", "nod.voice-mix", 1080.f, 120.f);
    auto output = node("audio-output", "nod.audio-output", 1320.f, 120.f);
    set(osc, "waveform", 0);
    set(osc, "level", 0.2);
    set(envelope, "attack", 0.01);
    set(envelope, "decay", 0.12);
    set(envelope, "sustain", 0.7);
    set(envelope, "release", 0.25);
    return {
        {midi, note, osc, envelope, gain, mix, output},
        {
            cable("midi", "note", "note-to-frequency", "note"),
            cable("note-to-frequency", "frequency", "oscillator", "frequency"),
            cable("midi", "gate", "envelope", "gate"),
            cable("oscillator", "audio", "gain", "audio-in"),
            cable("envelope", "envelope", "gain", "gain"),
            cable("gain", "audio-out", "voice-mix", "voices"),
            cable("voice-mix", "audio", "audio-output", "audio"),
        },
    };
}

model::GraphSnapshot filterPatch() {
    auto snapshot = sinePatch();
    snapshot.nodes.push_back(node("filter", "nod.lowpass", 820.f, 40.f));
    snapshot.nodes.push_back(node("cutoff", "nod.scale-bias", 820.f, 300.f));
    for (auto& record : snapshot.nodes) {
        if (record.id.value == "oscillator") set(record, "waveform", 2);
        if (record.id.value == "gain") record.position = {1080.f, 120.f};
        if (record.id.value == "voice-mix") record.position = {1320.f, 120.f};
        if (record.id.value == "audio-output") record.position = {1560.f, 120.f};
    }
    snapshot.nodes[4].position = {1080.f, 120.f};
    std::erase_if(snapshot.connections, [](const model::Connection& connection) {
        return connection.from.nodeId.value == "oscillator" && connection.from.portId.value == "audio";
    });
    snapshot.connections.push_back(cable("oscillator", "audio", "filter", "audio-in"));
    snapshot.connections.push_back(cable("envelope", "envelope", "cutoff", "input"));
    snapshot.connections.push_back(cable("cutoff", "output", "filter", "cutoff"));
    snapshot.connections.push_back(cable("filter", "audio-out", "gain", "audio-in"));
    for (auto& record : snapshot.nodes) {
        if (record.id.value == "cutoff") {
            set(record, "scale", 6000);
            set(record, "bias", 180);
        }
    }
    return snapshot;
}

model::GraphSnapshot delayPatch() {
    auto snapshot = sinePatch();
    snapshot.nodes.push_back(node("delay", "nod.feedback-delay", 1080.f, 40.f));
    snapshot.nodes.push_back(node("feedback-mix", "nod.mix", 1080.f, 220.f));
    for (auto& record : snapshot.nodes) {
        if (record.id.value == "oscillator") set(record, "waveform", 3);
        if (record.id.value == "delay") {
            set(record, "time", 0.18);
            set(record, "feedback", 0);
        }
        if (record.id.value == "feedback-mix") set(record, "mix", 0.35);
    }
    std::erase_if(snapshot.connections, [](const model::Connection& connection) {
        return connection.from.nodeId.value == "gain" && connection.from.portId.value == "audio-out";
    });
    snapshot.connections.push_back(cable("gain", "audio-out", "feedback-mix", "a"));
    snapshot.connections.push_back(cable("delay", "audio-out", "feedback-mix", "b"));
    snapshot.connections.push_back(cable("feedback-mix", "audio", "delay", "audio-in"));
    snapshot.connections.push_back(cable("feedback-mix", "audio", "voice-mix", "voices"));
    return snapshot;
}
} // namespace nodsynth::nodes
