#define INIT_CLASS_IID
#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/vsttypes.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {
class MemoryStream : public Steinberg::IBStream {
public:
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID, void**) override { return Steinberg::kNoInterface; }
    Steinberg::uint32 PLUGIN_API addRef() override { return 1; }
    Steinberg::uint32 PLUGIN_API release() override { return 1; }
    Steinberg::tresult PLUGIN_API read(void* buffer, Steinberg::int32 numBytes, Steinberg::int32* numRead) override
    {
        const auto available = static_cast<Steinberg::int32>(data.size() - static_cast<std::size_t>(position));
        const auto count = numBytes < available ? numBytes : available;
        if (count > 0) std::memcpy(buffer, data.data() + position, static_cast<std::size_t>(count));
        position += count;
        if (numRead != nullptr) *numRead = count;
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API write(void* buffer, Steinberg::int32 numBytes, Steinberg::int32* numWritten) override
    {
        const auto* bytes = static_cast<const char*>(buffer);
        data.insert(data.end(), bytes, bytes + numBytes);
        position += numBytes;
        if (numWritten != nullptr) *numWritten = numBytes;
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API seek(Steinberg::int64 pos, Steinberg::int32 mode, Steinberg::int64* result) override
    {
        if (mode == kIBSeekSet) position = pos;
        else if (mode == kIBSeekCur) position += pos;
        else position = static_cast<Steinberg::int64>(data.size()) + pos;
        if (result != nullptr) *result = position;
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API tell(Steinberg::int64* pos) override
    {
        if (pos != nullptr) *pos = position;
        return Steinberg::kResultOk;
    }
    void rewind() { position = 0; }

private:
    std::vector<char> data;
    Steinberg::int64 position{0};
};

class EventList : public Steinberg::Vst::IEventList {
public:
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID, void**) override { return Steinberg::kNoInterface; }
    Steinberg::uint32 PLUGIN_API addRef() override { return 1; }
    Steinberg::uint32 PLUGIN_API release() override { return 1; }
    Steinberg::int32 PLUGIN_API getEventCount() override { return static_cast<Steinberg::int32>(events.size()); }
    Steinberg::tresult PLUGIN_API getEvent(Steinberg::int32 index, Steinberg::Vst::Event& event) override
    {
        if (index < 0 || static_cast<std::size_t>(index) >= events.size()) return Steinberg::kResultFalse;
        event = events[static_cast<std::size_t>(index)];
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API addEvent(Steinberg::Vst::Event& event) override
    {
        events.push_back(event);
        return Steinberg::kResultOk;
    }
    void clear() { events.clear(); }

private:
    std::vector<Steinberg::Vst::Event> events;
};

bool fail(const char* message)
{
    std::cerr << message << '\n';
    return false;
}

float peakOf(const float* left, const float* right, int count)
{
    float peak = 0.f;
    for (int index = 0; index < count; ++index) peak = std::max(peak, std::max(std::fabs(left[index]), std::fabs(right[index])));
    return peak;
}
} // namespace

int main()
{
    const auto module = LoadLibraryA(NOD_VST3_PATH);
    if (module == nullptr) return fail("the VST3 module did not load") ? 1 : 1;
    using GetFactory = Steinberg::IPluginFactory* (*)();
    auto* getFactory = reinterpret_cast<GetFactory>(GetProcAddress(module, "GetPluginFactory"));
    if (getFactory == nullptr) return fail("GetPluginFactory is missing") ? 1 : 1;
    auto* factory = getFactory();
    if (factory == nullptr || factory->countClasses() < 1) return fail("the plugin factory is empty") ? 1 : 1;

    Steinberg::PClassInfo info{};
    if (factory->getClassInfo(0, &info) != Steinberg::kResultOk) return fail("class info is missing") ? 1 : 1;
    if (std::string(info.category) != "Audio Module Class" || std::string(info.name) != "NodSynth") {
        return fail("the factory class is not the NodSynth instrument") ? 1 : 1;
    }

    Steinberg::Vst::IComponent* component = nullptr;
    if (factory->createInstance(info.cid, Steinberg::Vst::IComponent::iid, reinterpret_cast<void**>(&component)) != Steinberg::kResultOk ||
        component == nullptr) {
        return fail("the component was not created") ? 1 : 1;
    }
    if (component->initialize(nullptr) != Steinberg::kResultOk) return fail("initialize failed") ? 1 : 1;
    Steinberg::Vst::IAudioProcessor* processor = nullptr;
    if (component->queryInterface(Steinberg::Vst::IAudioProcessor::iid, reinterpret_cast<void**>(&processor)) != Steinberg::kResultOk ||
        processor == nullptr) {
        return fail("the component has no audio processor") ? 1 : 1;
    }
    Steinberg::Vst::SpeakerArrangement output = Steinberg::Vst::SpeakerArr::kStereo;
    if (processor->setBusArrangements(nullptr, 0, &output, 1) != Steinberg::kResultOk) return fail("stereo output was rejected") ? 1 : 1;
    Steinberg::Vst::ProcessSetup setup{};
    setup.processMode = Steinberg::Vst::kRealtime;
    setup.symbolicSampleSize = Steinberg::Vst::kSample32;
    setup.maxSamplesPerBlock = 128;
    setup.sampleRate = 48000.0;
    if (processor->setupProcessing(setup) != Steinberg::kResultOk) return fail("setupProcessing failed") ? 1 : 1;
    component->activateBus(Steinberg::Vst::kAudio, Steinberg::Vst::kOutput, 0, 1);
    component->activateBus(Steinberg::Vst::kEvent, Steinberg::Vst::kInput, 0, 1);
    if (component->setActive(1) != Steinberg::kResultOk) return fail("setActive failed") ? 1 : 1;

    constexpr int kFrames = 128;
    float left[kFrames]{};
    float right[kFrames]{};
    float* buffers[2] = {left, right};
    Steinberg::Vst::AudioBusBuffers outputBus{};
    outputBus.numChannels = 2;
    outputBus.channelBuffers32 = buffers;
    EventList events;
    Steinberg::Vst::Event note{};
    note.type = Steinberg::Vst::Event::kNoteOnEvent;
    note.noteOn.channel = 0;
    note.noteOn.pitch = 69;
    note.noteOn.velocity = 0.8f;
    events.addEvent(note);
    Steinberg::Vst::ProcessData data{};
    data.numSamples = kFrames;
    data.symbolicSampleSize = Steinberg::Vst::kSample32;
    data.numOutputs = 1;
    data.outputs = &outputBus;
    data.inputEvents = &events;
    float peak = 0.f;
    for (int block = 0; block < 12; ++block) {
        if (processor->process(data) != Steinberg::kResultOk) return fail("process failed") ? 1 : 1;
        peak = std::max(peak, peakOf(left, right, kFrames));
        events.clear();
        data.inputEvents = nullptr;
    }
    if (!(peak > 0.01f)) return fail("a note produced silence") ? 1 : 1;

    MemoryStream state;
    if (component->getState(&state) != Steinberg::kResultOk) return fail("getState failed") ? 1 : 1;
    component->setActive(0);
    if (component->setActive(1) != Steinberg::kResultOk) return fail("reactivate failed") ? 1 : 1;
    state.rewind();
    if (component->setState(&state) != Steinberg::kResultOk) return fail("setState rejected the saved patch") ? 1 : 1;
    note.type = Steinberg::Vst::Event::kNoteOnEvent;
    events.addEvent(note);
    data.inputEvents = &events;
    peak = 0.f;
    for (int block = 0; block < 12; ++block) {
        if (processor->process(data) != Steinberg::kResultOk) return fail("process after setState failed") ? 1 : 1;
        peak = std::max(peak, peakOf(left, right, kFrames));
        events.clear();
        data.inputEvents = nullptr;
    }
    if (!(peak > 0.01f)) return fail("restored state produced silence") ? 1 : 1;
    component->setActive(0);
    component->terminate();
    component->release();
    processor->release();
    FreeLibrary(module);
    std::cout << "nodsynth vst3 loaded, played, and restored state\n";
    return 0;
}
