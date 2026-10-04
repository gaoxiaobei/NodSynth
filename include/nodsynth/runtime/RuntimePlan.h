#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nodsynth/compiler/CompiledGraph.h>
#include <nodsynth/model/SchemaRegistry.h>
#include <nodsynth/runtime/DspNode.h>
#include <nodsynth/runtime/PrepareConfig.h>
#include <nodsynth/runtime/SpscQueue.h>
#include <nodsynth/runtime/VoiceAllocator.h>

namespace nodsynth::runtime {
struct PlanResult {
    std::unique_ptr<class RuntimePlan> plan;
    PrepareError error{PrepareError::none};
    std::string message;
};

[[nodiscard]] PlanResult preparePlan(
    const compiler::CompiledGraph& graph,
    const model::SchemaRegistry& registry,
    const ImplementationRegistry& implementations,
    const PrepareConfig& config,
    const VoiceAllocator& voices,
    double parameterSmoothSeconds);

[[nodiscard]] std::atomic<int>& runtimePlanLiveCount();

class RuntimePlan {
public:
    RuntimePlan();
    ~RuntimePlan();
    RuntimePlan(const RuntimePlan&) = delete;
    RuntimePlan& operator=(const RuntimePlan&) = delete;

    void process(const VoiceAllocator& voices, float* mixLeft, float* mixRight, std::uint32_t frames);
    bool enqueueParameter(std::uint32_t index, float value, std::uint32_t sampleOffset);
    [[nodiscard]] std::optional<std::uint32_t> findParameter(
        const model::NodeId& node,
        const model::ParameterId& parameter) const;
    [[nodiscard]] const float* portSamples(
        const model::NodeId& node,
        const model::PortId& port,
        std::uint32_t voice,
        std::uint32_t channel) const noexcept;
    [[nodiscard]] std::uint64_t committedBytes() const noexcept { return bytes_; }
    [[nodiscard]] double sampleRate() const noexcept { return sampleRate_; }
    [[nodiscard]] std::uint32_t voiceCount() const noexcept { return voiceCount_; }

private:
    friend PlanResult preparePlan(
        const compiler::CompiledGraph&,
        const model::SchemaRegistry&,
        const ImplementationRegistry&,
        const PrepareConfig&,
        const VoiceAllocator&,
        double);

    struct Mailbox {
        std::atomic<std::uint32_t> bits{0};
        std::atomic<std::uint32_t> stamp{0};
    };

    struct ParamEvent {
        std::uint32_t sampleOffset{0};
        std::uint32_t index{0};
        float value{0.f};
    };

    struct ParamState {
        float current{0.f};
        float target{0.f};
        float minimum{0.f};
        float maximum{0.f};
        bool smooth{true};
        std::uint32_t appliedStamp{0};
        model::NodeId node{};
        model::ParameterId id{};
    };

    struct Program {
        model::NodeId id{};
        std::unique_ptr<DspNode> node;
        bool perVoice{true};
        std::vector<BufferView> outputs;
        ParamView parameters{};
    };

    void renderParameters(std::uint32_t frames);
    void fillParameter(std::uint32_t index, float value);

    double sampleRate_{48000.0};
    std::uint32_t maxFrames_{0};
    std::uint32_t voiceCount_{0};
    std::uint64_t bytes_{0};
    float smoothCoeff_{1.f};
    std::vector<float> storage_;
    std::vector<float> tracks_;
    std::vector<ParamState> parameters_;
    std::unique_ptr<Mailbox[]> mailboxes_;
    std::vector<const float*> notePtrs_;
    std::vector<const float*> gatePtrs_;
    std::vector<const float*> velocityPtrs_;
    std::vector<const float*> triggerPtrs_;
    std::vector<const float*> attenuationPtrs_;
    std::vector<Program> perVoice_;
    std::vector<Program> global_;
    SpscQueue<ParamEvent, 256> queue_{};
    std::atomic<bool> overflow_{false};
    std::vector<ParamEvent> drained_;
};
} // namespace nodsynth::runtime
