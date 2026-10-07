#include <nodsynth/nodes/BuiltinNodes.h>
#include <nodsynth/effects/Effect.h>

#include <stdexcept>
#include <utility>

namespace nodsynth::nodes {
namespace {
using namespace nodsynth::model;

PortSchema port(
    std::string id, std::string name, PortDirection direction, PortKind kind, std::uint32_t channels, PortDomain domain) {
    return {PortId{std::move(id)}, std::move(name), direction, kind, channels, domain};
}

ParameterSchema parameter(
    std::string id,
    std::string name,
    std::string unit,
    double minimum,
    double maximum,
    double defaultValue,
    ParameterScale scale,
    bool modulatable) {
    return {ParameterId{std::move(id)}, std::move(name), std::move(unit), minimum, maximum, defaultValue, scale, modulatable};
}

NodeSchema make(
    std::string type,
    std::string name,
    std::string category,
    NodeScope scope,
    std::vector<PortSchema> ports,
    std::vector<ParameterSchema> parameters,
    bool breaksCycle = false) {
    return {
        NodeTypeId{std::move(type)},
        1,
        std::move(name),
        std::move(category),
        "nodsynth",
        scope,
        std::move(ports),
        std::move(parameters),
        breaksCycle,
    };
}

void must(SchemaRegistry& registry, NodeSchema schema) {
    if (!registry.registerSchema(std::move(schema))) {
        throw std::logic_error("builtin node schema was rejected");
    }
}
} // namespace

model::SchemaRegistry builtinRegistry() {
    using model::NodeScope;
    using model::ParameterScale;
    using model::PortDirection;
    using model::PortDomain;
    using model::PortKind;
    model::SchemaRegistry registry;
    must(registry, make(
                       "nod.midi-input", "MIDI Input", "Input", NodeScope::perVoice,
                       {
                           port("note", "Note", PortDirection::output, PortKind::note, 1, PortDomain::sameAsNode),
                           port("gate", "Gate", PortDirection::output, PortKind::gate, 1, PortDomain::sameAsNode),
                       },
                       {}));
    must(registry, make(
                       "nod.note-to-frequency", "Note to Frequency", "Control", NodeScope::perVoice,
                       {
                           port("note", "Note", PortDirection::input, PortKind::note, 1, PortDomain::sameAsNode),
                           port("frequency", "Frequency", PortDirection::output, PortKind::control, 1, PortDomain::sameAsNode),
                       },
                       {}));
    must(registry, make(
                       "nod.oscillator", "Oscillator", "Source", NodeScope::perVoice,
                       {
                           port("frequency", "Frequency", PortDirection::input, PortKind::control, 1, PortDomain::sameAsNode),
                           port("audio", "Audio", PortDirection::output, PortKind::audio, 1, PortDomain::sameAsNode),
                       },
                       {
                           parameter("waveform", "Waveform", "", 0, 3, 0, ParameterScale::linear, false),
                           parameter("level", "Level", "", 0, 1, 0.2, ParameterScale::linear, true),
                           parameter("frequency", "Frequency", "Hz", 0, 20000, 440, ParameterScale::linear, true),
                       }));
    must(registry, make(
                       "nod.adsr", "ADSR", "Control", NodeScope::perVoice,
                       {
                           port("gate", "Gate", PortDirection::input, PortKind::gate, 1, PortDomain::sameAsNode),
                           port("envelope", "Envelope", PortDirection::output, PortKind::control, 1, PortDomain::sameAsNode),
                       },
                       {
                           parameter("attack", "Attack", "s", 0, 10, 0.01, ParameterScale::linear, true),
                           parameter("decay", "Decay", "s", 0, 10, 0.1, ParameterScale::linear, true),
                           parameter("sustain", "Sustain", "", 0, 1, 0.7, ParameterScale::linear, true),
                           parameter("release", "Release", "s", 0, 10, 0.2, ParameterScale::linear, true),
                       }));
    must(registry, make(
                       "nod.gain", "Gain", "Audio", NodeScope::perVoice,
                       {
                           port("audio-in", "Audio In", PortDirection::input, PortKind::audio, 1, PortDomain::sameAsNode),
                           port("gain", "Gain", PortDirection::input, PortKind::control, 1, PortDomain::sameAsNode),
                           port("audio-out", "Audio Out", PortDirection::output, PortKind::audio, 1, PortDomain::sameAsNode),
                       },
                       {parameter("gain", "Gain", "", 0, 4, 1, ParameterScale::linear, true)}));
    must(registry, make(
                       "nod.noise", "Noise", "Source", NodeScope::perVoice,
                       {port("audio", "Audio", PortDirection::output, PortKind::audio, 1, PortDomain::sameAsNode)},
                       {
                           parameter("level", "Level", "", 0, 1, 0.2, ParameterScale::linear, true),
                           parameter("seed", "Seed", "", 0, 2147483647, 1, ParameterScale::linear, false),
                       }));
    must(registry, make(
                       "nod.lowpass", "Lowpass", "Audio", NodeScope::perVoice,
                       {
                           port("audio-in", "Audio In", PortDirection::input, PortKind::audio, 1, PortDomain::sameAsNode),
                           port("cutoff", "Cutoff", PortDirection::input, PortKind::control, 1, PortDomain::sameAsNode),
                           port("resonance", "Resonance", PortDirection::input, PortKind::control, 1, PortDomain::sameAsNode),
                           port("audio-out", "Audio Out", PortDirection::output, PortKind::audio, 1, PortDomain::sameAsNode),
                       },
                       {
                           parameter("cutoff", "Cutoff", "Hz", 20, 20000, 1000, ParameterScale::logarithmic, true),
                           parameter("resonance", "Resonance", "", 0, 1, 0.1, ParameterScale::linear, true),
                       }));
    must(registry, make(
                       "nod.highpass", "Highpass", "Audio", NodeScope::perVoice,
                       {
                           port("audio-in", "Audio In", PortDirection::input, PortKind::audio, 1, PortDomain::sameAsNode),
                           port("cutoff", "Cutoff", PortDirection::input, PortKind::control, 1, PortDomain::sameAsNode),
                           port("resonance", "Resonance", PortDirection::input, PortKind::control, 1, PortDomain::sameAsNode),
                           port("audio-out", "Audio Out", PortDirection::output, PortKind::audio, 1, PortDomain::sameAsNode),
                       },
                       {
                           parameter("cutoff", "Cutoff", "Hz", 20, 20000, 4000, ParameterScale::logarithmic, true),
                           parameter("resonance", "Resonance", "", 0, 1, 0.1, ParameterScale::linear, true),
                       }));
    must(registry, make(
                       "nod.feedback-delay", "Feedback Delay", "Audio", NodeScope::perVoice,
                       {
                           port("audio-in", "Audio In", PortDirection::input, PortKind::audio, 1, PortDomain::sameAsNode),
                           port("feedback", "Feedback", PortDirection::input, PortKind::control, 1, PortDomain::sameAsNode),
                           port("audio-out", "Audio Out", PortDirection::output, PortKind::audio, 1, PortDomain::sameAsNode),
                       },
                       {
                           parameter("time", "Time", "s", 0, 2, 0.25, ParameterScale::linear, true),
                           parameter("feedback", "Feedback", "", 0, 0.95, 0.35, ParameterScale::linear, true),
                       },
                       true));
    must(registry, make(
                       "nod.add", "Add", "Control", NodeScope::perVoice,
                       {
                           port("a", "A", PortDirection::input, PortKind::control, 1, PortDomain::sameAsNode),
                           port("b", "B", PortDirection::input, PortKind::control, 1, PortDomain::sameAsNode),
                           port("sum", "Sum", PortDirection::output, PortKind::control, 1, PortDomain::sameAsNode),
                       },
                       {}));
    must(registry, make(
                       "nod.multiply", "Multiply", "Control", NodeScope::perVoice,
                       {
                           port("a", "A", PortDirection::input, PortKind::control, 1, PortDomain::sameAsNode),
                           port("b", "B", PortDirection::input, PortKind::control, 1, PortDomain::sameAsNode),
                           port("product", "Product", PortDirection::output, PortKind::control, 1, PortDomain::sameAsNode),
                       },
                       {}));
    must(registry, make(
                       "nod.scale-bias", "Scale Bias", "Control", NodeScope::perVoice,
                       {
                           port("input", "Input", PortDirection::input, PortKind::control, 1, PortDomain::sameAsNode),
                           port("output", "Output", PortDirection::output, PortKind::control, 1, PortDomain::sameAsNode),
                       },
                       {
                           parameter("scale", "Scale", "", -10000, 10000, 1, ParameterScale::linear, true),
                           parameter("bias", "Bias", "", -10000, 10000, 0, ParameterScale::linear, true),
                       }));
    must(registry, make(
                       "nod.mix", "Mix", "Audio", NodeScope::perVoice,
                       {
                           port("a", "A", PortDirection::input, PortKind::audio, 1, PortDomain::sameAsNode),
                           port("b", "B", PortDirection::input, PortKind::audio, 1, PortDomain::sameAsNode),
                           port("mix", "Mix", PortDirection::input, PortKind::control, 1, PortDomain::sameAsNode),
                           port("audio", "Audio", PortDirection::output, PortKind::audio, 1, PortDomain::sameAsNode),
                       },
                       {parameter("mix", "Mix", "", 0, 1, 0.5, ParameterScale::linear, true)}));
    must(registry, make(
                       "nod.voice-mix", "Voice Mix", "Audio", NodeScope::global,
                       {
                           port("voices", "Voices", PortDirection::input, PortKind::audio, 1, PortDomain::perVoice),
                           port("audio", "Audio", PortDirection::output, PortKind::audio, 2, PortDomain::sameAsNode),
                       },
                       {parameter("level", "Level", "", 0, 1, 1, ParameterScale::linear, true)}));
    must(registry, make(
                       "nod.audio-output", "Audio Output", "Output", NodeScope::global,
                       {port("audio", "Audio", PortDirection::input, PortKind::audio, 2, PortDomain::sameAsNode)}, {}));
    // New type IDs make migration explicit; v1 patch ports and DSP remain intact.
    for (const auto* type : {"nod.lowpass", "nod.highpass", "nod.gain", "nod.mix", "nod.voice-mix"}) {
        auto schema = *registry.find(NodeTypeId{type});
        schema.typeId.value += "-v2";
        schema.schemaVersion = 2;
        schema.displayName += " Stereo v2";
        for (auto& p : schema.ports) if (p.kind == PortKind::audio) p.channels = 2;
        if (std::string(type) == "nod.lowpass" || std::string(type) == "nod.highpass") {
            for (auto& p : schema.ports) if (p.id.value == "cutoff") p.id.value = "cutoff-mod";
            schema.parameters[0].modulationPort = "cutoff-mod";
            schema.parameters[0].modulationMode = ModulationMode::octave;
            schema.parameters[0].depthParameter = "depth";
            schema.parameters.push_back(parameter("depth", "Cutoff Depth", "octaves", -8, 8, 0, ParameterScale::linear, true));
        } else if (std::string(type) == "nod.gain") {
            schema.parameters[0].modulationPort = "gain";
            schema.parameters[0].modulationMode = ModulationMode::multiply;
        }
        must(registry, std::move(schema));
    }
    auto pan = make("nod.pan-v2", "Mono Pan v2", "Audio", NodeScope::perVoice,
        {port("audio-in", "Mono In", PortDirection::input, PortKind::audio, 1, PortDomain::sameAsNode),
         port("audio-out", "Stereo Out", PortDirection::output, PortKind::audio, 2, PortDomain::sameAsNode)},
        {parameter("pan", "Pan", "equal-power", -1, 1, 0, ParameterScale::linear, true)});
    pan.schemaVersion = 2;
    must(registry, std::move(pan));
    auto unison = make("nod.unison-v2", "Unison Saw v2", "Source", NodeScope::perVoice,
        {port("frequency", "Frequency", PortDirection::input, PortKind::control, 1, PortDomain::sameAsNode),
         port("audio", "Stereo Audio", PortDirection::output, PortKind::audio, 2, PortDomain::sameAsNode)},
        {parameter("voices", "Unison Voices", "oscillators/note", 1, 8, 7, ParameterScale::linear, false),
         parameter("detune", "Detune", "cents", 0, 100, 18, ParameterScale::linear, true),
         parameter("spread", "Spread", "ratio", 0, 1, .8, ParameterScale::linear, true),
         parameter("phase", "Phase (0 retrigger, 1 random)", "", 0, 1, 1, ParameterScale::linear, false),
         parameter("blend", "Blend", "ratio", 0, 1, .6, ParameterScale::linear, true),
         parameter("level", "Level", "linear-amplitude", 0, 1, .2, ParameterScale::linear, true),
         parameter("seed", "Seed", "", 0, 2147483647, 1, ParameterScale::linear, false),
         parameter("frequency", "Frequency", "Hz", 0, 20000, 440, ParameterScale::linear, true)});
    unison.schemaVersion = 2;
    must(registry, std::move(unison));
    for(const auto& effect:effects::schemas()) {
        if(effect.type!="delay" && effect.type!="reverb") continue;
        std::vector<ParameterSchema> parameters;
        for(const auto& p:effect.parameters) parameters.push_back(parameter(p.id.c_str(),p.id.c_str(),p.unit.c_str(),p.minimum,p.maximum,p.defaultValue,ParameterScale::linear,p.automatable));
        if(effect.type=="delay") parameters.push_back(parameter("tempoBpm","Tempo","BPM",20,400,120,ParameterScale::linear,true));
        must(registry,make(effect.type=="delay"?"nod.music-delay":"nod.reverb",effect.type=="delay"?"Music Delay":"Stereo Reverb","Effects",NodeScope::global,
            {port("audio-in","Stereo In",PortDirection::input,PortKind::audio,2,PortDomain::sameAsNode),
             port("audio-out","Stereo Out",PortDirection::output,PortKind::audio,2,PortDomain::sameAsNode)},std::move(parameters)));
    }
    return registry;
}
} // namespace nodsynth::nodes
