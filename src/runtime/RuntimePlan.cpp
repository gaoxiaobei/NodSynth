#include <nodsynth/runtime/RuntimePlan.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <utility>

namespace nodsynth::runtime {
namespace {
std::atomic<int> gLivePlans{0};

void addCount(std::uint64_t& total, std::uint64_t amount, bool& overflow) {
    if (amount > std::numeric_limits<std::uint64_t>::max() - total) {
        overflow = true;
        return;
    }
    total += amount;
}

template <typename Event>
void sortEvents(std::vector<Event>& events) {
    for (std::size_t index = 1; index < events.size(); ++index) {
        const auto key = events[index];
        std::size_t cursor = index;
        while (cursor > 0 && events[cursor - 1].sampleOffset > key.sampleOffset) {
            events[cursor] = events[cursor - 1];
            --cursor;
        }
        events[cursor] = key;
    }
}

const model::NodeSchema* schemaFor(
    const compiler::CompiledGraph& graph, const model::SchemaRegistry& registry, const model::NodeId& id) {
    for (const auto& node : graph.nodes) {
        if (node.record.id == id) return registry.find(node.record.typeId);
    }
    return nullptr;
}

const compiler::CompiledGraph::Node* compiledNode(const compiler::CompiledGraph& graph, const model::NodeId& id) {
    for (const auto& node : graph.nodes) {
        if (node.record.id == id) return &node;
    }
    return nullptr;
}

float bitsToFloat(std::uint32_t bits) {
    float value = 0.f;
    static_assert(sizeof(float) == sizeof(std::uint32_t));
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

std::uint32_t floatToBits(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}
} // namespace

std::atomic<int>& runtimePlanLiveCount() { return gLivePlans; }

RuntimePlan::RuntimePlan() { gLivePlans.fetch_add(1, std::memory_order_relaxed); }
RuntimePlan::~RuntimePlan() { gLivePlans.fetch_sub(1, std::memory_order_relaxed); }

PlanResult preparePlan(
    const compiler::CompiledGraph& graph,
    const model::SchemaRegistry& registry,
    const ImplementationRegistry& implementations,
    const PrepareConfig& config,
    const VoiceAllocator& voices,
    double parameterSmoothSeconds) {
    PlanResult result;
    if (!validSampleRate(config.sampleRate)) {
        result.error = PrepareError::invalidSampleRate;
        result.message = prepareErrorText(result.error);
        return result;
    }
    if (!validFrameCount(config.maxFrames)) {
        result.error = PrepareError::invalidBlockSize;
        result.message = prepareErrorText(result.error);
        return result;
    }
    if (config.voiceCount < 1 || config.voiceCount > kMaxVoices) {
        result.error = PrepareError::invalidVoiceCount;
        result.message = prepareErrorText(result.error);
        return result;
    }
    if (config.voiceCount != graph.voiceBudget || voices.voiceCount() != config.voiceCount ||
        voices.maxFrames() < config.maxFrames) {
        result.error = config.voiceCount != graph.voiceBudget ? PrepareError::voiceBudgetMismatch
                                                              : PrepareError::invalidVoiceCount;
        result.message = prepareErrorText(result.error);
        return result;
    }

    std::vector<const compiler::CompiledGraph::Node*> ordered;
    ordered.reserve(graph.perVoiceOrder.size() + graph.globalOrder.size());
    for (const auto& id : graph.perVoiceOrder) ordered.push_back(compiledNode(graph, id));
    for (const auto& id : graph.globalOrder) ordered.push_back(compiledNode(graph, id));
    for (const auto* node : ordered) {
        if (node == nullptr) {
            result.error = PrepareError::unknownImplementation;
            result.message = prepareErrorText(result.error);
            return result;
        }
        const auto* schema = registry.find(node->record.typeId);
        if (schema == nullptr || !implementations.contains(node->record.typeId)) {
            result.error = PrepareError::unknownImplementation;
            result.message = std::string(prepareErrorText(result.error)) + ": " + node->record.typeId.value;
            return result;
        }
        if (schema->parameters.size() > kMaxParametersPerNode) {
            result.error = PrepareError::tooManyParameters;
            result.message = prepareErrorText(result.error);
            return result;
        }
    }

    bool overflow = false;
    std::uint64_t floatCount = 0;
    struct Placement {
        model::Endpoint endpoint;
        std::uint64_t offset{0};
        std::uint32_t channels{1};
        std::uint32_t voiceStride{0};
    };
    std::vector<Placement> placements;
    auto place = [&](model::Endpoint endpoint, std::uint32_t channels, std::uint32_t voiceSlots) {
        Placement placement{std::move(endpoint), floatCount, channels, channels * config.maxFrames};
        const auto samples = static_cast<std::uint64_t>(voiceSlots) * channels * config.maxFrames;
        addCount(floatCount, samples, overflow);
        placements.push_back(std::move(placement));
    };
    for (const auto& buffer : graph.audioBuffers) {
        place(buffer.output, buffer.channels, buffer.domain == model::NodeScope::perVoice ? config.voiceCount : 1);
    }
    for (const auto& buffer : graph.controlBuffers) {
        place(buffer.output, 1, buffer.domain == model::NodeScope::perVoice ? config.voiceCount : 1);
    }
    for (const auto* node : ordered) {
        const auto* schema = registry.find(node->record.typeId);
        const auto slots = node->scope == model::NodeScope::perVoice ? config.voiceCount : 1u;
        for (const auto& port : schema->ports) {
            if (port.direction != model::PortDirection::output) continue;
            if (port.kind != model::PortKind::note && port.kind != model::PortKind::gate) continue;
            place(model::Endpoint{node->record.id, port.id}, 1, slots);
        }
    }

    std::uint64_t parameterCount = 0;
    std::uint64_t stateBytes = 0;
    for (const auto* node : ordered) {
        const auto* schema = registry.find(node->record.typeId);
        addCount(parameterCount, schema->parameters.size(), overflow);
        addCount(
            stateBytes,
            implementations.stateBytes(node->record.typeId, config.sampleRate, config.maxFrames, config.voiceCount) + 256,
            overflow);
    }
    const auto bufferFloats = floatCount;
    addCount(floatCount, parameterCount * config.maxFrames, overflow);
    std::uint64_t bytes = stateBytes;
    addCount(bytes, floatCount * sizeof(float), overflow);
    if (overflow || bytes > config.maxBytes) {
        result.error = PrepareError::budgetExceeded;
        result.message = prepareErrorText(result.error);
        return result;
    }

    auto plan = std::make_unique<RuntimePlan>();
    plan->sampleRate_ = config.sampleRate;
    plan->maxFrames_ = config.maxFrames;
    plan->voiceCount_ = config.voiceCount;
    plan->bytes_ = bytes;
    if (parameterSmoothSeconds <= 0.0) plan->smoothCoeff_ = 1.f;
    else {
        const double samples = parameterSmoothSeconds * config.sampleRate;
        plan->smoothCoeff_ = samples <= 1.0 ? 1.f : static_cast<float>(1.0 - std::exp(-1.0 / samples));
    }
    plan->storage_.assign(static_cast<std::size_t>(bufferFloats), 0.f);
    plan->tracks_.assign(static_cast<std::size_t>(parameterCount) * config.maxFrames, 0.f);
    plan->parameters_.reserve(parameterCount);
    if (parameterCount > 0) plan->mailboxes_ = std::make_unique<RuntimePlan::Mailbox[]>(parameterCount);
    plan->drained_.reserve(256);

    auto capture = [&](auto getter) {
        std::vector<const float*> pointers(config.voiceCount);
        for (std::uint32_t voice = 0; voice < config.voiceCount; ++voice) pointers[voice] = getter(voice);
        return pointers;
    };
    plan->notePtrs_ = capture([&](std::uint32_t voice) { return voices.note(voice); });
    plan->gatePtrs_ = capture([&](std::uint32_t voice) { return voices.gate(voice); });
    plan->velocityPtrs_ = capture([&](std::uint32_t voice) { return voices.velocity(voice); });
    plan->triggerPtrs_ = capture([&](std::uint32_t voice) { return voices.triggers(voice); });
    plan->attenuationPtrs_ = capture([&](std::uint32_t voice) { return voices.attenuation(voice); });

    std::map<model::Endpoint, BufferView> views;
    for (const auto& placement : placements) {
        BufferView view;
        view.port = placement.endpoint.portId;
        view.data = plan->storage_.data() + static_cast<std::size_t>(placement.offset);
        view.channels = placement.channels;
        view.capacity = config.maxFrames;
        view.channelStride = config.maxFrames;
        view.voiceStride = placement.voiceStride;
        views.emplace(placement.endpoint, std::move(view));
    }

    std::uint32_t parameterBase = 0;
    for (const auto* node : ordered) {
        const auto* schema = registry.find(node->record.typeId);
        RuntimePlan::Program program;
        program.id = node->record.id;
        program.perVoice = node->scope == model::NodeScope::perVoice;
        program.node = implementations.instantiate(node->record.typeId);
        if (!program.node) {
            result.error = PrepareError::unknownImplementation;
            result.message = prepareErrorText(result.error);
            return result;
        }
        for (const auto& parameter : schema->parameters) {
            const auto stored = node->record.parameters.find(parameter.id);
            const float value = static_cast<float>(stored == node->record.parameters.end() ? parameter.defaultValue : stored->second);
            plan->parameters_.push_back({
                value,
                value,
                static_cast<float>(parameter.minimum),
                static_cast<float>(parameter.maximum),
                parameter.modulatable,
                0,
                node->record.id,
                parameter.id,
            });
        }
        if (!schema->parameters.empty()) {
            program.parameters.data = plan->tracks_.data() + static_cast<std::size_t>(parameterBase) * config.maxFrames;
            program.parameters.count = static_cast<std::uint32_t>(schema->parameters.size());
            program.parameters.capacity = config.maxFrames;
        }
        NodeBinding binding;
        binding.sampleRate = config.sampleRate;
        binding.maxFrames = config.maxFrames;
        binding.voiceCount = config.voiceCount;
        binding.parameters = program.parameters;
        binding.note = plan->notePtrs_.data();
        binding.gate = plan->gatePtrs_.data();
        binding.velocity = plan->velocityPtrs_.data();
        binding.triggers = plan->triggerPtrs_.data();
        binding.attenuation = plan->attenuationPtrs_.data();
        for (const auto& port : schema->ports) {
            BufferView view;
            view.port = port.id;
            view.channels = port.channels;
            view.capacity = config.maxFrames;
            if (port.direction == model::PortDirection::output) {
                const auto found = views.find(model::Endpoint{node->record.id, port.id});
                if (found != views.end()) view = found->second;
            } else {
                for (const auto& connection : graph.connections) {
                    if (connection.record.to.nodeId != node->record.id || connection.record.to.portId != port.id) continue;
                    const auto found = views.find(connection.record.from);
                    if (found != views.end()) view = found->second;
                    view.port = port.id;
                    break;
                }
            }
            if (port.direction == model::PortDirection::output) {
                program.outputs.push_back(view);
                binding.outputs.push_back(view);
            } else {
                binding.inputs.push_back(std::move(view));
            }
        }
        program.node->bind(binding);
        program.node->reset();
        parameterBase += static_cast<std::uint32_t>(schema->parameters.size());
        if (program.perVoice) plan->perVoice_.push_back(std::move(program));
        else plan->global_.push_back(std::move(program));
    }

    for (std::uint32_t index = 0; index < plan->parameters_.size(); ++index) {
        for (std::uint32_t frame = 0; frame < config.maxFrames; ++frame) {
            plan->tracks_[static_cast<std::size_t>(index) * config.maxFrames + frame] = plan->parameters_[index].current;
        }
    }
    result.plan = std::move(plan);
    return result;
}

void RuntimePlan::fillParameter(std::uint32_t index, float value) {
    auto& parameter = parameters_[index];
    if (!std::isfinite(value)) return;
    parameter.target = std::clamp(value, parameter.minimum, parameter.maximum);
    if (!parameter.smooth) parameter.current = parameter.target;
}

void RuntimePlan::renderParameters(std::uint32_t frames, std::span<const ParameterEvent> direct, std::uint32_t origin) {
    if (overflow_.exchange(false, std::memory_order_acq_rel)) queue_.discardAll();
    drained_.clear();
    ParamEvent event;
    while (queue_.pop(event)) {
        if (drained_.size() == drained_.capacity()) {
            overflow_.store(true, std::memory_order_relaxed);
            break;
        }
        drained_.push_back(event);
    }
    if (overflow_.exchange(false, std::memory_order_acq_rel)) {
        queue_.discardAll();
        drained_.clear();
    }
    sortEvents(drained_);
    std::size_t cursor = 0;
    std::size_t directCursor = 0;
    while (directCursor < direct.size() && direct[directCursor].sampleOffset < origin) ++directCursor;
    for (std::uint32_t frame = 0; frame < frames; ++frame) {
        while (cursor < drained_.size() && drained_[cursor].sampleOffset <= frame) {
            fillParameter(drained_[cursor].index, drained_[cursor].value);
            ++cursor;
        }
        while (directCursor < direct.size() && direct[directCursor].sampleOffset <= origin + frame) {
            if (direct[directCursor].index < parameters_.size())
                fillParameter(direct[directCursor].index, direct[directCursor].value);
            ++directCursor;
        }
        for (std::uint32_t index = 0; index < parameters_.size(); ++index) {
            auto& parameter = parameters_[index];
            if (parameter.smooth) parameter.current += (parameter.target - parameter.current) * smoothCoeff_;
            else parameter.current = parameter.target;
            tracks_[static_cast<std::size_t>(index) * maxFrames_ + frame] = parameter.current;
        }
    }
    for (std::uint32_t index = 0; index < parameters_.size(); ++index) {
        auto& box = mailboxes_[index];
        const auto first = box.stamp.load(std::memory_order_acquire);
        const auto bits = box.bits.load(std::memory_order_acquire);
        const auto second = box.stamp.load(std::memory_order_acquire);
        if (first != second || first == parameters_[index].appliedStamp) continue;
        fillParameter(index, bitsToFloat(bits));
        parameters_[index].appliedStamp = first;
    }
}

void RuntimePlan::process(const VoiceAllocator& voices, float* mixLeft, float* mixRight, std::uint32_t frames,
                          std::span<const ParameterEvent> parameters, std::uint32_t origin) {
    frames = std::min(frames, maxFrames_);
    renderParameters(frames, parameters, origin);
    for (std::uint32_t voice = 0; voice < voiceCount_; ++voice) {
        for (auto& program : perVoice_) program.node->process(voice, frames);
        for (auto& program : perVoice_) {
            if (program.node->needsCommit()) program.node->commit(voice, frames);
        }
    }
    for (auto& program : global_) {
        program.node->setMix(mixLeft, mixRight);
        program.node->process(0, frames);
    }
    (void)voices;
}

bool RuntimePlan::enqueueParameter(std::uint32_t index, float value, std::uint32_t sampleOffset) {
    if (index >= parameters_.size() || !std::isfinite(value)) return false;
    value = std::clamp(value, parameters_[index].minimum, parameters_[index].maximum);
    mailboxes_[index].bits.store(floatToBits(value), std::memory_order_relaxed);
    mailboxes_[index].stamp.fetch_add(1, std::memory_order_release);
    if (!queue_.push(ParamEvent{sampleOffset, index, value})) {
        overflow_.store(true, std::memory_order_release);
        return false;
    }
    return true;
}

std::optional<std::uint32_t> RuntimePlan::findParameter(const model::NodeId& node, const model::ParameterId& parameter) const {
    for (std::uint32_t index = 0; index < parameters_.size(); ++index) {
        if (parameters_[index].node == node && parameters_[index].id == parameter) return index;
    }
    return std::nullopt;
}

const float* RuntimePlan::portSamples(
    const model::NodeId& node, const model::PortId& port, std::uint32_t voice, std::uint32_t channel) const noexcept {
    const auto search = [&](const std::vector<Program>& programs) -> const float* {
        for (const auto& program : programs) {
            if (program.id != node) continue;
            for (const auto& output : program.outputs) {
                if (output.port != port || output.data == nullptr || channel >= output.channels) continue;
                return output.at(voice, channel, 0);
            }
        }
        return nullptr;
    };
    if (const auto* found = search(perVoice_)) return found;
    return search(global_);
}
} // namespace nodsynth::runtime
