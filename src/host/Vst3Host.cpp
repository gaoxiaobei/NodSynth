#include <nodsynth/host/Vst3Host.h>

#include "public.sdk/source/vst/hosting/eventlist.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/vst/hosting/processdata.h"

#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstmessage.h"

#include <algorithm>
#include <cstring>
#include <vector>
#include <cmath>
#include <limits>

namespace nodsynth::host {
namespace {
class MemoryStream final : public Steinberg::IBStream {
public:
    explicit MemoryStream(std::vector<char>* storage) : storage_(storage) {}
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID, void**) override { return Steinberg::kNoInterface; }
    Steinberg::uint32 PLUGIN_API addRef() override { return 1; }
    Steinberg::uint32 PLUGIN_API release() override { return 1; }
    Steinberg::tresult PLUGIN_API read(void* buffer, Steinberg::int32 numBytes, Steinberg::int32* numRead) override
    {
        const auto size = static_cast<Steinberg::int64>(storage_->size());
        const auto available = position_ >= size ? 0 : static_cast<Steinberg::int32>(size - position_);
        const auto count = std::max<Steinberg::int32>(0, std::min(numBytes, available));
        if (count > 0) std::memcpy(buffer, storage_->data() + position_, static_cast<std::size_t>(count));
        position_ += count;
        if (numRead != nullptr) *numRead = count;
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API write(void* buffer, Steinberg::int32 numBytes, Steinberg::int32* numWritten) override
    {
        if (position_ < 0) position_ = 0;
        const auto end = position_ + numBytes;
        if (static_cast<std::size_t>(end) > storage_->size()) storage_->resize(static_cast<std::size_t>(end));
        if (numBytes > 0) std::memcpy(storage_->data() + position_, buffer, static_cast<std::size_t>(numBytes));
        position_ = end;
        if (numWritten != nullptr) *numWritten = numBytes;
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API seek(Steinberg::int64 pos, Steinberg::int32 mode, Steinberg::int64* result) override
    {
        if (mode == kIBSeekSet) position_ = pos;
        else if (mode == kIBSeekCur) position_ += pos;
        else position_ = static_cast<Steinberg::int64>(storage_->size()) + pos;
        if (position_ < 0) position_ = 0;
        if (result != nullptr) *result = position_;
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API tell(Steinberg::int64* pos) override
    {
        if (pos != nullptr) *pos = position_;
        return Steinberg::kResultOk;
    }
    void rewind() { position_ = 0; }

private:
    std::vector<char>* storage_;
    Steinberg::int64 position_{0};
};

bool transferState(Steinberg::IBStream& stream, bool save, Steinberg::Vst::IComponent* component, Steinberg::Vst::IEditController* controller)
{
    if (component == nullptr) return false;
    if ((save ? component->getState(&stream) : component->setState(&stream)) != Steinberg::kResultOk) return false;
    if (controller == nullptr) return true;
    Steinberg::int32 read = 0;
    char marker = 0;
    if (save) {
        stream.write(&marker, 0, &read);
    }
    return (save ? controller->getState(&stream) : controller->setState(&stream)) == Steinberg::kResultOk || controller == nullptr;
}
} // namespace

struct Vst3Plugin::Instance {
    VST3::Hosting::Module::Ptr module;
    Steinberg::IPtr<Steinberg::Vst::HostApplication> host;
    Steinberg::IPtr<Steinberg::Vst::IComponent> component;
    Steinberg::IPtr<Steinberg::Vst::IAudioProcessor> processor;
    Steinberg::IPtr<Steinberg::Vst::IEditController> controller;
    Steinberg::IPtr<Steinberg::Vst::IConnectionPoint> componentConnection,controllerConnection;
    Steinberg::Vst::HostProcessData process;
    Steinberg::Vst::EventList events{64};
    Steinberg::Vst::ParameterChanges parameterChanges{16};
    Steinberg::Vst::ProcessContext context{};
    bool processing{false};
    std::uint32_t capacity{0};
    bool audioInput{false};
    std::vector<float> silence;
    double musicalPosition{0};
};

Vst3Plugin::Vst3Plugin() = default;

Vst3Plugin::~Vst3Plugin() { close(); }

bool Vst3Plugin::open(const std::filesystem::path& path, std::string& error, const std::string& className)
{
    close();
    instance_ = new Instance;
    instance_->module = VST3::Hosting::Module::create(path.string(), error);
    if (!instance_->module) {
        close();
        if (error.empty()) error = "the VST3 module did not load";
        return false;
    }
    instance_->host = new Steinberg::Vst::HostApplication;
    instance_->module->getFactory().setHostContext(instance_->host);
    const auto classes = instance_->module->getFactory().classInfos();
    const VST3::Hosting::ClassInfo* effect = nullptr;
    for (const auto& info : classes) {
        if (info.category() != "Audio Module Class") continue;
        if (!className.empty() && info.name() != className) continue;
        effect = &info;
        break;
    }
    if (effect == nullptr) {
        error = "the module has no audio effect class";
        close();
        return false;
    }
    info_.name = effect->name();
    info_.vendor = effect->vendor();
    info_.version = effect->version();
    info_.subCategories = effect->subCategoriesString();
    instance_->component = instance_->module->getFactory().createInstance<Steinberg::Vst::IComponent>(effect->ID());
    if (!instance_->component || instance_->component->initialize(instance_->host) != Steinberg::kResultOk) {
        error = "the plugin component did not initialize";
        close();
        return false;
    }
    instance_->processor = Steinberg::FUnknownPtr<Steinberg::Vst::IAudioProcessor>(instance_->component);
    if (!instance_->processor) {
        error = "the plugin has no audio processor";
        close();
        return false;
    }
    instance_->controller = Steinberg::FUnknownPtr<Steinberg::Vst::IEditController>(instance_->component);
    if (!instance_->controller) {
        Steinberg::TUID controllerId{};
        if(instance_->component->getControllerClassId(controllerId)==Steinberg::kResultOk) {
            instance_->controller=instance_->module->getFactory().createInstance<Steinberg::Vst::IEditController>(VST3::UID(controllerId));
            if(!instance_->controller || instance_->controller->initialize(instance_->host)!=Steinberg::kResultOk) {
                error="the selected plugin controller did not initialize";close();return false;
            }
            instance_->componentConnection=Steinberg::FUnknownPtr<Steinberg::Vst::IConnectionPoint>(instance_->component);
            instance_->controllerConnection=Steinberg::FUnknownPtr<Steinberg::Vst::IConnectionPoint>(instance_->controller);
            if(instance_->componentConnection && instance_->controllerConnection) {
                instance_->componentConnection->connect(instance_->controllerConnection);instance_->controllerConnection->connect(instance_->componentConnection);
            }
        }
    }
    return true;
}

std::vector<std::string> Vst3Plugin::classes() const {
    std::vector<std::string> result;
    if(instance_ && instance_->module) for(const auto& info:instance_->module->getFactory().classInfos())
        if(info.category()=="Audio Module Class") result.push_back(info.name());
    return result;
}
std::vector<Vst3Parameter> Vst3Plugin::parameters() const
{
    std::vector<Vst3Parameter> result;
    if (instance_ == nullptr || instance_->controller == nullptr) return result;
    const auto count = instance_->controller->getParameterCount();
    result.reserve(static_cast<std::size_t>(std::max<Steinberg::int32>(count, 0)));
    for (Steinberg::int32 index = 0; index < count; ++index) {
        Steinberg::Vst::ParameterInfo info{};
        if (instance_->controller->getParameterInfo(index, info) != Steinberg::kResultOk) continue;
        Vst3Parameter parameter;
        parameter.id = info.id;
        for (const auto character : info.title) {
            if (character == 0) break;
            if (character < 128) parameter.title.push_back(static_cast<char>(character));
        }
        parameter.canAutomate = (info.flags & Steinberg::Vst::ParameterInfo::kCanAutomate) != 0;
        parameter.value = instance_->controller->getParamNormalized(info.id);
        result.push_back(std::move(parameter));
    }
    return result;
}

bool Vst3Plugin::activate(double sampleRate, std::uint32_t blockSize, std::string& error)
{
    if (instance_ == nullptr || instance_->processor == nullptr) {
        error = "the plugin is not open";
        return false;
    }
    if(!std::isfinite(sampleRate) || sampleRate<8000 || sampleRate>192000 || !blockSize || blockSize>8192 || instance_->processing) {
        error="invalid plugin rate, block or activation state";return false;
    }
    Steinberg::Vst::SpeakerArrangement stereo = Steinberg::Vst::SpeakerArr::kStereo;
    const auto inputs = instance_->component->getBusCount(Steinberg::Vst::kAudio, Steinberg::Vst::kInput);
    const auto outputs = instance_->component->getBusCount(Steinberg::Vst::kAudio, Steinberg::Vst::kOutput);
    if (outputs > 0) {
        std::vector<Steinberg::Vst::SpeakerArrangement> outputArr(static_cast<std::size_t>(outputs), stereo);
        std::vector<Steinberg::Vst::SpeakerArrangement> inputArr(static_cast<std::size_t>(std::max<Steinberg::int32>(inputs, 0)), stereo);
        instance_->processor->setBusArrangements(inputArr.empty() ? nullptr : inputArr.data(), inputs, outputArr.data(), outputs);
    }
    for (Steinberg::int32 bus = 0; bus < outputs; ++bus) {
        instance_->component->activateBus(Steinberg::Vst::kAudio, Steinberg::Vst::kOutput, bus, bus == 0);
    }
    for (Steinberg::int32 bus = 0; bus < inputs; ++bus) {
        instance_->component->activateBus(Steinberg::Vst::kAudio, Steinberg::Vst::kInput, bus, bus == 0);
    }
    const auto eventIns = instance_->component->getBusCount(Steinberg::Vst::kEvent, Steinberg::Vst::kInput);
    for (Steinberg::int32 bus = 0; bus < eventIns; ++bus) {
        instance_->component->activateBus(Steinberg::Vst::kEvent, Steinberg::Vst::kInput, bus, 1);
    }
    Steinberg::Vst::ProcessSetup setup{};
    setup.processMode = Steinberg::Vst::kOffline;
    setup.symbolicSampleSize = Steinberg::Vst::kSample32;
    setup.maxSamplesPerBlock = static_cast<Steinberg::int32>(std::max<std::uint32_t>(blockSize, 32));
    setup.sampleRate = sampleRate;
    if (instance_->processor->setupProcessing(setup) != Steinberg::kResultOk) {
        error = "setupProcessing failed";
        return false;
    }
    if (instance_->component->setActive(1) != Steinberg::kResultOk) {
        error = "setActive failed";
        return false;
    }
    if (!instance_->process.prepare(*instance_->component, 0, Steinberg::Vst::kSample32)) {
        error = "the host could not prepare plugin buffers";
        instance_->component->setActive(0);
        return false;
    }
    instance_->capacity=blockSize;instance_->silence.assign(static_cast<std::size_t>(blockSize)*2,0);
    instance_->audioInput=instance_->process.numInputs>0 && instance_->process.inputs[0].numChannels==2;
    instance_->processor->setProcessing(1);
    instance_->processing = true;
    info_.latencySamples = instance_->processor->getLatencySamples();
    timelineSamples_ = 0;
    instance_->musicalPosition=0;
    instance_->context = {};
    instance_->context.sampleRate = sampleRate;
    instance_->context.tempo = 120.0;
    instance_->context.state = Steinberg::Vst::ProcessContext::kPlaying | Steinberg::Vst::ProcessContext::kProjectTimeMusicValid |
                               Steinberg::Vst::ProcessContext::kContTimeValid | Steinberg::Vst::ProcessContext::kTempoValid;
    instance_->process.processContext = &instance_->context;
    return true;
}

bool Vst3Plugin::process(
    float* left,
    float* right,
    std::uint32_t frames,
    const std::vector<Vst3Midi>& midi,
    std::string& error,
    const float* inputLeft,
    const float* inputRight,
    const std::vector<Vst3ParamPoint>* parameters)
{
    if (instance_ == nullptr || !instance_->processing || left == nullptr || right == nullptr || frames>instance_->capacity) {
        error = "the plugin is not active";
        return false;
    }
    instance_->events.clear();
    for (const auto& message : midi) {
        Steinberg::Vst::Event event{};
        event.sampleOffset = static_cast<Steinberg::int32>(message.sampleOffset);
        event.type = message.noteOn ? Steinberg::Vst::Event::kNoteOnEvent : Steinberg::Vst::Event::kNoteOffEvent;
        event.noteOn.channel = message.channel;
        event.noteOn.pitch = message.pitch;
        event.noteOn.velocity = message.noteOn ? static_cast<float>(message.velocity) / 127.f : 0.f;
        if (!message.noteOn) {
            event.noteOff.channel = message.channel;
            event.noteOff.pitch = message.pitch;
            event.noteOff.velocity = 0.f;
        }
        instance_->events.addEvent(event);
    }
    float* outputs[] = {left, right};
    if (!instance_->process.setChannelBuffers(Steinberg::Vst::kOutput, 0, outputs, 2)) {
        error = "the plugin has no stereo output";
        return false;
    }
    if (instance_->process.numInputs > 0) instance_->process.inputs[0].silenceFlags = 0;
    if (instance_->process.numOutputs > 0) instance_->process.outputs[0].silenceFlags = 0;
    const auto inputs = instance_->component->getBusCount(Steinberg::Vst::kAudio, Steinberg::Vst::kInput);
    if (inputs > 0) {
        const float* sourceLeft = inputLeft != nullptr ? inputLeft : nullptr;
        const float* sourceRight = inputRight != nullptr ? inputRight : inputLeft;
        if (sourceLeft == nullptr) {
            sourceLeft = instance_->silence.data();
            sourceRight = instance_->silence.data() + instance_->capacity;
        }
        float* inputBuffers[] = {const_cast<float*>(sourceLeft), const_cast<float*>(sourceRight)};
        if (!instance_->process.setChannelBuffers(Steinberg::Vst::kInput, 0, inputBuffers, 2)) {
            error = "input buffers were rejected";
            return false;
        }
    }
    instance_->context.projectTimeSamples = timelineSamples_;
    instance_->context.continousTimeSamples = timelineSamples_;
    instance_->context.projectTimeMusic=instance_->musicalPosition;
    instance_->parameterChanges.clearQueue();
    if (parameters != nullptr) {
        for (const auto& point : *parameters) {
            Steinberg::int32 queueIndex = 0;
            auto* queue = instance_->parameterChanges.addParameterData(point.id, queueIndex);
            if (queue == nullptr) continue;
            Steinberg::int32 pointIndex = 0;
            queue->addPoint(static_cast<Steinberg::int32>(point.sampleOffset), point.value, pointIndex);
        }
    }
    instance_->process.numSamples = static_cast<Steinberg::int32>(frames);
    instance_->process.inputEvents = &instance_->events;
    instance_->process.inputParameterChanges = parameters != nullptr && !parameters->empty() ? &instance_->parameterChanges : nullptr;
    if (instance_->processor->process(instance_->process) != Steinberg::kResultOk) {
        error = "process failed";
        return false;
    }
    timelineSamples_ += frames;
    instance_->musicalPosition+=frames/instance_->context.sampleRate*instance_->context.tempo/60;
    return true;
}

bool Vst3Plugin::saveState(std::vector<char>& bytes, std::string& error)
{
    if (instance_ == nullptr || instance_->component == nullptr) {
        error = "the plugin is not open";
        return false;
    }
    bytes.clear();
    MemoryStream stream(&bytes);
    if (instance_->component->getState(&stream) != Steinberg::kResultOk) {
        error = "getState failed";
        return false;
    }
    return true;
}

bool Vst3Plugin::restoreState(const std::vector<char>& bytes, std::string& error)
{
    if (instance_ == nullptr || instance_->component == nullptr) {
        error = "the plugin is not open";
        return false;
    }
    std::vector<char> copy = bytes;
    MemoryStream stream(&copy);
    if (instance_->component->setState(&stream) != Steinberg::kResultOk) {
        error = "setState failed";
        return false;
    }
    if(instance_->controller) {stream.rewind();instance_->controller->setComponentState(&stream);}
    return true;
}

bool Vst3Plugin::restart(const std::vector<char>& state,double rate,std::uint32_t frames,std::string& error) {
    if(!instance_ || !instance_->processor || !instance_->component) {error="plugin is not open";return false;}
    if(instance_->processing) instance_->processor->setProcessing(0);instance_->processing=false;
    instance_->component->setActive(0);instance_->process.unprepare();
    return restoreState(state,error) && activate(rate,frames,error);
}
bool Vst3Plugin::hasAudioInput() const noexcept {return instance_ && instance_->audioInput;}
std::int32_t Vst3Plugin::currentLatency() const noexcept {return instance_ && instance_->processor?static_cast<std::int32_t>(instance_->processor->getLatencySamples()):0;}
double Vst3Plugin::tailSeconds() const noexcept {
    if(!instance_ || !instance_->processor || instance_->context.sampleRate<=0) return 0;
    const auto tail=instance_->processor->getTailSamples();
    return tail==std::numeric_limits<Steinberg::uint32>::max()?std::numeric_limits<double>::infinity():tail/instance_->context.sampleRate;
}
void Vst3Plugin::setTempo(double bpm) noexcept {if(instance_ && std::isfinite(bpm) && bpm>0) instance_->context.tempo=bpm;}

void Vst3Plugin::close() noexcept
{
    if (instance_ == nullptr) return;
    if (instance_->processing && instance_->processor) instance_->processor->setProcessing(0);
    if (instance_->component) instance_->component->setActive(0);
    if(instance_->componentConnection && instance_->controllerConnection) {
        instance_->componentConnection->disconnect(instance_->controllerConnection);instance_->controllerConnection->disconnect(instance_->componentConnection);
    }
    instance_->process.unprepare();
    if (instance_->controller &&
        reinterpret_cast<void*>(instance_->controller.get()) != reinterpret_cast<void*>(instance_->component.get())) {
        instance_->controller->terminate();
    }
    if (instance_->component) instance_->component->terminate();
    delete instance_;
    instance_ = nullptr;
    info_ = {};
    timelineSamples_ = 0;
}

std::vector<float> delayForLatency(const std::vector<float>& interleaved, std::uint32_t channels, std::int32_t latency)
{
    if (channels == 0 || latency <= 0) return interleaved;
    const auto frames = interleaved.size() / channels;
    std::vector<float> delayed((frames + static_cast<std::size_t>(latency)) * channels, 0.f);
    std::copy(interleaved.begin(), interleaved.end(), delayed.begin() + static_cast<std::size_t>(latency) * channels);
    return delayed;
}
} // namespace nodsynth::host
