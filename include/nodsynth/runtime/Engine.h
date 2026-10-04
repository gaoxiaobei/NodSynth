#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <nodsynth/model/Identifiers.h>
#include <nodsynth/runtime/PrepareConfig.h>
#include <nodsynth/runtime/RuntimePlan.h>
#include <nodsynth/runtime/SpscQueue.h>
#include <nodsynth/runtime/VoiceAllocator.h>

namespace nodsynth::runtime {
struct Telemetry {
    std::uint32_t activeVoices{0};
    std::uint32_t xruns{0};
    float lastLoad{0.f};
    float loadP50{0.f};
    float loadP99{0.f};
};

class Engine {
public:
    explicit Engine(EngineConfig config = {});
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    void configure(const PrepareConfig& config);
    [[nodiscard]] const PrepareConfig& config() const noexcept { return config_; }
    [[nodiscard]] double parameterSmoothSeconds() const noexcept { return engineConfig_.parameterSmoothSeconds; }
    [[nodiscard]] std::uint64_t overheadBytes() const noexcept { return scratchBytes_; }
    void setPeakBudget(std::uint64_t bytes) noexcept { engineConfig_.maxPeakBytes = bytes; }
    [[nodiscard]] const VoiceAllocator& voices() const noexcept { return voices_; }
    [[nodiscard]] VoiceAllocator& voices() noexcept { return voices_; }

    [[nodiscard]] StageResult stage(std::unique_ptr<RuntimePlan> plan);
    void setReleaseHold(double seconds) noexcept;
    bool setParameter(const model::NodeId& node, const model::ParameterId& parameter, float value);
    bool postMidi(MidiEvent event);
    bool postDeviceMidi(MidiEvent event);
    void process(
        float* const* outputs,
        std::uint32_t channels,
        std::uint32_t frames,
        std::span<const MidiEvent> midi);
    void reclaim();
    [[nodiscard]] Telemetry telemetry() const;
    [[nodiscard]] const RuntimePlan* activePlan() const noexcept;

private:
    void retire(RuntimePlan* plan) noexcept;
    void installPending() noexcept;
    void renderPlan(RuntimePlan& plan, float* left, float* right, std::uint32_t frames);
    [[nodiscard]] std::uint64_t residentBytes() const noexcept;

    EngineConfig engineConfig_{};
    PrepareConfig config_{};
    VoiceAllocator voices_{};
    std::atomic<bool> halt_{false};
    std::atomic<std::uint32_t> entries_{0};
    std::atomic<std::uint32_t> exits_{0};
    std::atomic<RuntimePlan*> pending_{nullptr};
    std::atomic<RuntimePlan*> observed_{nullptr};
    std::atomic<RuntimePlan*> outgoingObserved_{nullptr};
    std::atomic<RuntimePlan*> reclaim_{nullptr};
    std::atomic<RuntimePlan*> reclaimExtra_{nullptr};
    RuntimePlan* active_{nullptr};
    RuntimePlan* outgoing_{nullptr};
    int fadeLeft_{0};
    int fadeTotal_{1};
    std::uint64_t scratchBytes_{0};
    std::vector<float> mixA_;
    std::vector<float> mixB_;
    std::vector<MidiEvent> blockMidi_;
    SpscQueue<MidiEvent, 1024> postedMidi_{};
    SpscQueue<MidiEvent, 1024> deviceMidi_{};
    std::atomic<bool> panic_{false};
    std::atomic<std::uint32_t> activeVoices_{0};
    std::atomic<std::uint32_t> xruns_{0};
    std::atomic<std::uint32_t> loadBits_{0};
    std::atomic<std::uint64_t> timings_[512]{};
    std::atomic<std::uint32_t> timingWrite_{0};
};
} // namespace nodsynth::runtime
