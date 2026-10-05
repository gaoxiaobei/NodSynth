#include "public.sdk/source/main/pluginfactory.h"
#include "public.sdk/source/vst/vstsinglecomponenteffect.h"

#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"

#include <nodsynth/compiler/GraphCompiler.h>
#include <nodsynth/nodes/BuiltinNodes.h>
#include <nodsynth/persist/ProjectFile.h>
#include <nodsynth/runtime/Engine.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace Steinberg::Vst {
class NodSynthProcessor : public SingleComponentEffect {
public:
    NodSynthProcessor() = default;

    static FUnknown* createInstance(void*) { return static_cast<IAudioProcessor*>(new NodSynthProcessor); }

    tresult PLUGIN_API initialize(FUnknown* context) SMTG_OVERRIDE
    {
        const auto result = SingleComponentEffect::initialize(context);
        if (result != kResultOk) return result;
        addAudioInput(STR16("Stereo In"), SpeakerArr::kStereo);
        addAudioOutput(STR16("Stereo Out"), SpeakerArr::kStereo);
        addEventInput(STR16("Event In"), 1);
        parameters.addParameter(STR16("Gain"), nullptr, 0, 0.8, ParameterInfo::kCanAutomate, kGain);
        parameters.addParameter(STR16("Bypass"), nullptr, 1, 0, ParameterInfo::kCanAutomate | ParameterInfo::kIsBypass, kBypass);
        graph = nodsynth::nodes::sinePatch();
        stateText = patchText(graph);
        return kResultOk;
    }

    tresult PLUGIN_API setBusArrangements(SpeakerArrangement* inputs, int32 numIns, SpeakerArrangement* outputs, int32 numOuts) SMTG_OVERRIDE
    {
        if (numOuts == 1 && outputs != nullptr && outputs[0] == SpeakerArr::kStereo && numIns <= 1 &&
            (numIns == 0 || (inputs != nullptr && inputs[0] == SpeakerArr::kStereo))) {
            return SingleComponentEffect::setBusArrangements(inputs, numIns, outputs, numOuts);
        }
        return kResultFalse;
    }

    tresult PLUGIN_API canProcessSampleSize(int32 symbolicSampleSize) SMTG_OVERRIDE
    {
        return symbolicSampleSize == kSample32 || symbolicSampleSize == kSample64 ? kResultOk : kResultFalse;
    }

    tresult PLUGIN_API setupProcessing(ProcessSetup& newSetup) SMTG_OVERRIDE
    {
        const auto result = SingleComponentEffect::setupProcessing(newSetup);
        if (result != kResultOk) return result;
        sampleRate = newSetup.sampleRate;
        maxFrames = std::max<int32>(newSetup.maxSamplesPerBlock, 64);
        prepared = false;
        return kResultOk;
    }

    tresult PLUGIN_API setParamNormalized(ParamID id, ParamValue value) SMTG_OVERRIDE
    {
        if (id == kGain) gain = static_cast<float>(value);
        if (id == kBypass) bypassed = value >= 0.5;
        return SingleComponentEffect::setParamNormalized(id, value);
    }

    tresult PLUGIN_API setActive(TBool state) SMTG_OVERRIDE
    {
        if (state) {
            if (!prepare()) return kResultFalse;
        } else {
            engine.reset();
            prepared = false;
        }
        return kResultOk;
    }

    tresult PLUGIN_API process(ProcessData& data) SMTG_OVERRIDE
    {
        if (data.numSamples <= 0) return kResultOk;
        readParameters(data.inputParameterChanges);
        const bool wide = data.symbolicSampleSize == kSample64;
        if (data.outputs == nullptr || data.outputs[0].numChannels < 2) return kResultOk;
        if (!wide && data.outputs[0].channelBuffers32 == nullptr) return kResultOk;
        if (wide && data.outputs[0].channelBuffers64 == nullptr) return kResultOk;
        std::vector<float> wideLeft;
        std::vector<float> wideRight;
        float* left = nullptr;
        float* right = nullptr;
        if (wide) {
            wideLeft.assign(static_cast<std::size_t>(data.numSamples), 0.f);
            wideRight.assign(static_cast<std::size_t>(data.numSamples), 0.f);
            left = wideLeft.data();
            right = wideRight.data();
        } else {
            left = data.outputs[0].channelBuffers32[0];
            right = data.outputs[0].channelBuffers32[1];
        }
        const auto publish = [&]() {
            if (!wide) return;
            for (int32 index = 0; index < data.numSamples; ++index) {
                data.outputs[0].channelBuffers64[0][index] = left[index];
                data.outputs[0].channelBuffers64[1][index] = right[index];
            }
        };
        if (bypassed || !prepared || !engine) {
            std::memset(left, 0, static_cast<std::size_t>(data.numSamples) * sizeof(float));
            std::memset(right, 0, static_cast<std::size_t>(data.numSamples) * sizeof(float));
            publish();
            data.outputs[0].silenceFlags = data.outputs[0].numChannels >= 64 ? ~std::uint64_t{0} : ((std::uint64_t{1} << data.outputs[0].numChannels) - 1);
            return kResultOk;
        }
        std::vector<nodsynth::runtime::MidiEvent> midi;
        if (data.inputEvents != nullptr) {
            const auto count = data.inputEvents->getEventCount();
            for (int32 index = 0; index < count; ++index) {
                Event event{};
                if (data.inputEvents->getEvent(index, event) != kResultOk) continue;
                nodsynth::runtime::MidiEvent message{};
                message.sampleOffset = static_cast<std::uint32_t>(std::max<int32>(event.sampleOffset, 0));
                if (event.type == Event::kNoteOnEvent) {
                    message.type = nodsynth::runtime::MidiType::noteOn;
                    message.channel = static_cast<std::uint8_t>(event.noteOn.channel & 0x0f);
                    message.data1 = static_cast<std::uint8_t>(std::clamp<int32>(event.noteOn.pitch, 0, 127));
                    message.data2 = static_cast<std::uint8_t>(std::clamp(event.noteOn.velocity, 0.f, 1.f) * 127.f);
                } else if (event.type == Event::kNoteOffEvent) {
                    message.type = nodsynth::runtime::MidiType::noteOff;
                    message.channel = static_cast<std::uint8_t>(event.noteOff.channel & 0x0f);
                    message.data1 = static_cast<std::uint8_t>(std::clamp<int32>(event.noteOff.pitch, 0, 127));
                } else {
                    continue;
                }
                midi.push_back(message);
            }
        }
        int32 rendered = 0;
        while (rendered < data.numSamples) {
            const auto chunk = std::min<int32>(maxFrames, data.numSamples - rendered);
            std::vector<nodsynth::runtime::MidiEvent> block;
            for (const auto& event : midi) {
                if (event.sampleOffset < static_cast<std::uint32_t>(rendered) ||
                    event.sampleOffset >= static_cast<std::uint32_t>(rendered + chunk)) {
                    continue;
                }
                auto copy = event;
                copy.sampleOffset -= static_cast<std::uint32_t>(rendered);
                block.push_back(copy);
            }
            float* outputs[] = {left + rendered, right + rendered};
            engine->process(outputs, 2, static_cast<std::uint32_t>(chunk), block);
            engine->reclaim();
            rendered += chunk;
        }
        std::vector<std::pair<int32, float>> gainPoints;
        if (data.inputParameterChanges != nullptr) {
            const auto count = data.inputParameterChanges->getParameterCount();
            for (int32 index = 0; index < count; ++index) {
                auto* queue = data.inputParameterChanges->getParameterData(index);
                if (queue == nullptr || queue->getParameterId() != kGain) continue;
                for (int32 point = 0; point < queue->getPointCount(); ++point) {
                    int32 offset = 0;
                    ParamValue value = 0;
                    if (queue->getPoint(point, offset, value) != kResultTrue) continue;
                    gainPoints.emplace_back(std::clamp(offset, 0, data.numSamples), static_cast<float>(value));
                }
            }
        }
        std::stable_sort(gainPoints.begin(), gainPoints.end(), [](const auto& leftPoint, const auto& rightPoint) {
            return leftPoint.first < rightPoint.first;
        });
        std::vector<std::pair<int32, float>> breaks;
        breaks.emplace_back(0, gain);
        for (const auto& point : gainPoints) {
            if (point.first == breaks.back().first) breaks.back().second = point.second;
            else breaks.push_back(point);
        }
        for (int32 index = 0; index < data.numSamples; ++index) {
            std::size_t segment = 0;
            while (segment + 1 < breaks.size() && breaks[segment + 1].first <= index) ++segment;
            float value = breaks[segment].second;
            if (segment + 1 < breaks.size() && breaks[segment + 1].first > breaks[segment].first) {
                const auto span = breaks[segment + 1].first - breaks[segment].first;
                const auto local = index - breaks[segment].first;
                const auto mix = static_cast<float>(local) / static_cast<float>(span);
                value = breaks[segment].second + (breaks[segment + 1].second - breaks[segment].second) * mix;
            }
            left[index] *= value;
            right[index] *= value;
            if (index == data.numSamples - 1) gain = value;
        }
        publish();
        data.outputs[0].silenceFlags = 0;
        return kResultOk;
    }

    tresult PLUGIN_API getState(IBStream* state) SMTG_OVERRIDE
    {
        if (state == nullptr) return kInvalidArgument;
        std::string blob = "NOD1";
        const float storedGain = gain;
        blob.append(reinterpret_cast<const char*>(&storedGain), sizeof(float));
        blob.push_back(bypassed ? '\1' : '\0');
        blob.append(stateText);
        int32 written = 0;
        return state->write(blob.data(), static_cast<int32>(blob.size()), &written);
    }

    tresult PLUGIN_API setState(IBStream* state) SMTG_OVERRIDE
    {
        if (state == nullptr) return kInvalidArgument;
        std::string text;
        char buffer[1024];
        while (true) {
            int32 read = 0;
            const auto result = state->read(buffer, static_cast<int32>(sizeof(buffer)), &read);
            if (read > 0) text.append(buffer, static_cast<std::size_t>(read));
            if (result != kResultOk || read == 0) break;
        }
        if (text.size() >= 9 && text.compare(0, 4, "NOD1") == 0) {
            float storedGain = 0.8f;
            std::memcpy(&storedGain, text.data() + 4, sizeof(float));
            bypassed = text[8] != 0;
            gain = storedGain;
            text.erase(0, 9);
            SingleComponentEffect::setParamNormalized(kGain, gain);
            SingleComponentEffect::setParamNormalized(kBypass, bypassed ? 1.0 : 0.0);
        }
        if (text.empty()) return kResultOk;
        const auto path = std::filesystem::temp_directory_path() / "nodsynth-vst3-state.json";
        {
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            if (!output) return kResultFalse;
            output.write(text.data(), static_cast<std::streamsize>(text.size()));
        }
        std::string error;
        auto document = nodsynth::persist::loadProject(path, error);
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        if (!document) return kResultFalse;
        graph = std::move(document->graph);
        stateText = patchText(graph);
        prepared = false;
        if (engine && !prepare()) return kResultFalse;
        return kResultOk;
    }

    IPlugView* PLUGIN_API createView(FIDString) SMTG_OVERRIDE { return nullptr; }

private:
    enum ParameterIds { kGain = 0, kBypass = 1 };

    static std::string patchText(const nodsynth::model::GraphSnapshot& snapshot)
    {
        auto document = nodsynth::persist::projectFromGraph(snapshot);
        return document.root.dump();
    }

    bool prepare()
    {
        nodsynth::runtime::EngineConfig config;
        config.audio.sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
        config.audio.maxFrames = static_cast<std::uint32_t>(std::max<int32>(maxFrames, 64));
        config.audio.voiceCount = 16;
        if (!engine) engine = std::make_unique<nodsynth::runtime::Engine>(config);
        else engine->configure(config.audio);
        const auto registry = nodsynth::nodes::builtinRegistry();
        const auto compiled = nodsynth::compiler::GraphCompiler{}.compile(graph, registry);
        if (!compiled.graph) return false;
        auto plan = nodsynth::runtime::preparePlan(
            *compiled.graph, registry, nodsynth::nodes::builtinImplementations(), engine->config(), engine->voices(),
            engine->parameterSmoothSeconds());
        if (!plan.plan || !engine->stage(std::move(plan.plan)).accepted) return false;
        prepared = true;
        return true;
    }

    void readParameters(IParameterChanges* changes)
    {
        if (changes == nullptr) return;
        const auto count = changes->getParameterCount();
        for (int32 index = 0; index < count; ++index) {
            auto* queue = changes->getParameterData(index);
            if (queue == nullptr || queue->getPointCount() <= 0) continue;
            int32 offset = 0;
            ParamValue value = 0;
            if (queue->getPoint(queue->getPointCount() - 1, offset, value) != kResultTrue) continue;
            if (queue->getParameterId() == kBypass) bypassed = value >= 0.5;
        }
    }

    nodsynth::model::GraphSnapshot graph{};
    std::string stateText;
    std::unique_ptr<nodsynth::runtime::Engine> engine;
    double sampleRate{48000.0};
    int32 maxFrames{512};
    float gain{0.8f};
    bool bypassed{false};
    bool prepared{false};
};
} // namespace Steinberg::Vst

BEGIN_FACTORY_DEF("NodSynth", "https://nodsynth.local", "mailto:dev@nodsynth.local")
DEF_CLASS2(
    INLINE_UID(0x4E6F6453, 0x796E7468, 0x56335433, 0x496E7374), PClassInfo::kManyInstances, kVstAudioEffectClass, "NodSynth", 0,
    Steinberg::Vst::PlugType::kInstrumentSynth, "0.1.0", kVstVersionString, Steinberg::Vst::NodSynthProcessor::createInstance)
END_FACTORY
