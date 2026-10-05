#include "public.sdk/source/main/pluginfactory.h"
#include "public.sdk/source/vst/vstsinglecomponenteffect.h"

#include "pluginterfaces/vst/ivstevents.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace Steinberg::Vst {
class LatencyProbeProcessor : public SingleComponentEffect {
public:
    static FUnknown* createInstance(void*) { return static_cast<IAudioProcessor*>(new LatencyProbeProcessor); }

    tresult PLUGIN_API initialize(FUnknown* context) SMTG_OVERRIDE
    {
        const auto result = SingleComponentEffect::initialize(context);
        if (result != kResultOk) return result;
        addAudioOutput(STR16("Stereo Out"), SpeakerArr::kStereo);
        addEventInput(STR16("Event In"), 1);
        delay_.assign(static_cast<std::size_t>(kLatency), 0.f);
        return kResultOk;
    }

    uint32 PLUGIN_API getLatencySamples() SMTG_OVERRIDE { return static_cast<uint32>(kLatency); }

    tresult PLUGIN_API canProcessSampleSize(int32 symbolicSampleSize) SMTG_OVERRIDE
    {
        return symbolicSampleSize == kSample32 ? kResultOk : kResultFalse;
    }

    tresult PLUGIN_API setupProcessing(ProcessSetup& newSetup) SMTG_OVERRIDE
    {
        const auto result = SingleComponentEffect::setupProcessing(newSetup);
        if (result != kResultOk) return result;
        inject_.assign(static_cast<std::size_t>(std::max<int32>(newSetup.maxSamplesPerBlock, 1)), 0.f);
        return kResultOk;
    }

    tresult PLUGIN_API process(ProcessData& data) SMTG_OVERRIDE
    {
        if (data.numSamples <= 0 || data.outputs == nullptr || data.outputs[0].numChannels < 2 || data.outputs[0].channelBuffers32 == nullptr) {
            return kResultOk;
        }
        if (static_cast<int32>(inject_.size()) < data.numSamples) inject_.assign(static_cast<std::size_t>(data.numSamples), 0.f);
        std::fill(inject_.begin(), inject_.begin() + data.numSamples, 0.f);
        if (data.inputEvents != nullptr) {
            const auto count = data.inputEvents->getEventCount();
            for (int32 index = 0; index < count; ++index) {
                Event event{};
                if (data.inputEvents->getEvent(index, event) != kResultOk || event.type != Event::kNoteOnEvent) continue;
                const auto offset = std::clamp(event.sampleOffset, 0, data.numSamples - 1);
                inject_[static_cast<std::size_t>(offset)] = 1.f;
            }
        }
        float* left = data.outputs[0].channelBuffers32[0];
        float* right = data.outputs[0].channelBuffers32[1];
        if (delay_.size() != static_cast<std::size_t>(kLatency)) delay_.assign(static_cast<std::size_t>(kLatency), 0.f);
        for (int32 index = 0; index < data.numSamples; ++index) {
            const float delayed = delay_[static_cast<std::size_t>(cursor_)];
            left[index] = delayed;
            right[index] = delayed;
            delay_[static_cast<std::size_t>(cursor_)] = inject_[static_cast<std::size_t>(index)];
            cursor_ = (cursor_ + 1) % kLatency;
        }
        data.outputs[0].silenceFlags = 0;
        return kResultOk;
    }

    IPlugView* PLUGIN_API createView(FIDString) SMTG_OVERRIDE { return nullptr; }

private:
    static constexpr int32 kLatency = 128;
    std::vector<float> delay_;
    std::vector<float> inject_;
    int32 cursor_{0};
};
} // namespace Steinberg::Vst

BEGIN_FACTORY_DEF("NodSynth", "https://nodsynth.local", "mailto:dev@nodsynth.local")
DEF_CLASS2(
    INLINE_UID(0x4C617450, 0x726F6265, 0x56335433, 0x31323800), PClassInfo::kManyInstances, kVstAudioEffectClass, "LatencyProbe", 0,
    Steinberg::Vst::PlugType::kInstrumentSynth, "0.1.0", kVstVersionString, Steinberg::Vst::LatencyProbeProcessor::createInstance)
END_FACTORY
