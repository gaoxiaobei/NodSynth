#include <nodsynth/runtime/Engine.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>
#include <utility>

namespace nodsynth::runtime {
namespace {
float floatFromBits(std::uint32_t bits) {
    float value = 0.f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

std::uint32_t bitsFromFloat(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

void sanitize(float* samples, std::uint32_t frames) {
    if (samples == nullptr) return;
    for (std::uint32_t frame = 0; frame < frames; ++frame) {
        const float sample = samples[frame];
        samples[frame] = std::isfinite(sample) ? std::clamp(sample, -1.f, 1.f) : 0.f;
    }
}
} // namespace

Engine::Engine(EngineConfig config) : engineConfig_(std::move(config)) {
    if (engineConfig_.crossfadeSeconds < 0.01 || engineConfig_.crossfadeSeconds > 0.02) engineConfig_.crossfadeSeconds = 0.015;
    configure(engineConfig_.audio);
}

Engine::~Engine() {
    halt_.store(true, std::memory_order_release);
    while (entries_.load(std::memory_order_acquire) != exits_.load(std::memory_order_acquire)) std::this_thread::yield();
    delete pending_.exchange(nullptr, std::memory_order_acq_rel);
    delete active_;
    delete outgoing_;
    delete reclaim_.exchange(nullptr, std::memory_order_acq_rel);
    delete reclaimExtra_.exchange(nullptr, std::memory_order_acq_rel);
    active_ = nullptr;
    outgoing_ = nullptr;
}

void Engine::configure(const PrepareConfig& config) {
    halt_.store(true, std::memory_order_release);
    while (entries_.load(std::memory_order_acquire) != exits_.load(std::memory_order_acquire)) std::this_thread::yield();
    config_ = config;
    if (!validSampleRate(config_.sampleRate)) config_.sampleRate = 48000.0;
    if (!validFrameCount(config_.maxFrames)) config_.maxFrames = 128;
    if (config_.voiceCount < 1 || config_.voiceCount > kMaxVoices) config_.voiceCount = 1;
    voices_.prepare(
        config_.voiceCount, config_.sampleRate, config_.maxFrames, engineConfig_.releaseHoldSeconds,
        engineConfig_.stealFadeSeconds);
    delete pending_.exchange(nullptr, std::memory_order_acq_rel);
    delete active_;
    delete outgoing_;
    delete reclaim_.exchange(nullptr, std::memory_order_acq_rel);
    delete reclaimExtra_.exchange(nullptr, std::memory_order_acq_rel);
    active_ = nullptr;
    outgoing_ = nullptr;
    observed_.store(nullptr, std::memory_order_release);
    outgoingObserved_.store(nullptr, std::memory_order_release);
    fadeLeft_ = 0;
    mixA_.assign(static_cast<std::size_t>(config_.maxFrames) * 2, 0.f);
    mixB_.assign(mixA_.size(), 0.f);
    blockMidi_.reserve(2048);
    scratchBytes_ = mixA_.size() * sizeof(float) * 2;
    halt_.store(false, std::memory_order_release);
}

std::uint64_t Engine::residentBytes() const noexcept {
    std::uint64_t total = scratchBytes_;
    if (const auto* plan = observed_.load(std::memory_order_acquire)) total += plan->committedBytes();
    if (const auto* plan = pending_.load(std::memory_order_acquire)) total += plan->committedBytes();
    if (const auto* plan = outgoingObserved_.load(std::memory_order_acquire)) total += plan->committedBytes();
    return total;
}

StageResult Engine::stage(std::unique_ptr<RuntimePlan> plan) {
    if (!plan) return {false, "missing plan"};
    if (plan->sampleRate() != config_.sampleRate || plan->voiceCount() != config_.voiceCount) {
        return {false, "plan configuration does not match the audio device"};
    }
    if (residentBytes() + plan->committedBytes() > engineConfig_.maxPeakBytes) return {false, prepareErrorText(PrepareError::budgetExceeded)};
    auto* published = pending_.exchange(plan.release(), std::memory_order_acq_rel);
    if (published != nullptr) delete published;
    return {true, ""};
}

void Engine::setReleaseHold(double seconds) noexcept { voices_.setReleaseHold(seconds); }

bool Engine::setParameter(const model::NodeId& node, const model::ParameterId& parameter, float value) {
    bool matched = false;
    const auto push = [&](RuntimePlan* plan) {
        if (plan == nullptr) return;
        if (const auto index = plan->findParameter(node, parameter)) {
            plan->enqueueParameter(*index, value, 0);
            matched = true;
        }
    };
    push(observed_.load(std::memory_order_acquire));
    push(pending_.load(std::memory_order_acquire));
    push(outgoingObserved_.load(std::memory_order_acquire));
    return matched;
}

bool Engine::postMidi(MidiEvent event) {
    if (!postedMidi_.push(event)) {
        panic_.store(true, std::memory_order_release);
        return false;
    }
    return true;
}

bool Engine::postDeviceMidi(MidiEvent event) {
    if (!deviceMidi_.push(event)) {
        panic_.store(true, std::memory_order_release);
        return false;
    }
    return true;
}

void Engine::installPending() noexcept {
    auto* next = pending_.exchange(nullptr, std::memory_order_acq_rel);
    if (next == nullptr) return;
    if (active_ == nullptr || next->sampleRate() != active_->sampleRate()) {
        retire(outgoing_);
        retire(active_);
        outgoing_ = nullptr;
        active_ = next;
        fadeLeft_ = 0;
        return;
    }
    retire(outgoing_);
    outgoing_ = active_;
    active_ = next;
    fadeTotal_ = std::max(1, static_cast<int>(std::lround(engineConfig_.crossfadeSeconds * config_.sampleRate)));
    fadeLeft_ = fadeTotal_;
}

void Engine::retire(RuntimePlan* plan) noexcept {
    if (plan == nullptr) return;
    RuntimePlan* expected = nullptr;
    if (reclaim_.compare_exchange_strong(expected, plan, std::memory_order_release, std::memory_order_relaxed)) return;
    expected = nullptr;
    reclaimExtra_.compare_exchange_strong(expected, plan, std::memory_order_release, std::memory_order_relaxed);
}

void Engine::renderPlan(RuntimePlan& plan, float* left, float* right, std::uint32_t frames) {
    std::fill_n(left, frames, 0.f);
    std::fill_n(right, frames, 0.f);
    plan.process(voices_, left, right, frames);
}

void Engine::process(float* const* outputs, std::uint32_t channels, std::uint32_t frames, std::span<const MidiEvent> midi) {
    entries_.fetch_add(1, std::memory_order_acq_rel);
    if (halt_.load(std::memory_order_acquire)) {
        exits_.fetch_add(1, std::memory_order_acq_rel);
        if (outputs != nullptr) {
            for (std::uint32_t channel = 0; channel < channels; ++channel) {
                if (outputs[channel] != nullptr) std::fill_n(outputs[channel], frames, 0.f);
            }
        }
        return;
    }
    const auto started = std::chrono::steady_clock::now();
    std::uint32_t rendered = 0;
    bool overrun = false;
    while (rendered < frames) {
        const auto chunk = std::min(config_.maxFrames, frames - rendered);
        blockMidi_.clear();
        if (panic_.exchange(false, std::memory_order_acq_rel)) voices_.panic();
        MidiEvent posted;
        const auto take = [&](auto& queue) {
            while (queue.pop(posted)) {
                if (blockMidi_.size() == blockMidi_.capacity()) {
                    panic_.store(true, std::memory_order_release);
                    break;
                }
                posted.sampleOffset = std::min(posted.sampleOffset, chunk == 0 ? 0 : chunk - 1);
                blockMidi_.push_back(posted);
            }
        };
        take(postedMidi_);
        take(deviceMidi_);
        for (const auto& event : midi) {
            if (event.sampleOffset < rendered || event.sampleOffset >= rendered + chunk) continue;
            if (blockMidi_.size() == blockMidi_.capacity()) {
                panic_.store(true, std::memory_order_release);
                break;
            }
            auto local = event;
            local.sampleOffset -= rendered;
            blockMidi_.push_back(local);
        }
        if (panic_.exchange(false, std::memory_order_acq_rel)) {
            blockMidi_.clear();
            voices_.panic();
        }
        voices_.renderBlock(blockMidi_, chunk);
        installPending();
        observed_.store(active_, std::memory_order_release);
        outgoingObserved_.store(outgoing_, std::memory_order_release);
        float* left = mixA_.data();
        float* right = mixA_.data() + config_.maxFrames;
        float* oldLeft = mixB_.data();
        float* oldRight = mixB_.data() + config_.maxFrames;
        if (active_ == nullptr) {
            std::fill_n(left, chunk, 0.f);
            std::fill_n(right, chunk, 0.f);
        } else {
            renderPlan(*active_, left, right, chunk);
        }
        if (outgoing_ != nullptr && fadeLeft_ > 0) {
            renderPlan(*outgoing_, oldLeft, oldRight, chunk);
            for (std::uint32_t frame = 0; frame < chunk; ++frame) {
                const float oldGain = fadeTotal_ == 0 ? 0.f : static_cast<float>(fadeLeft_) / static_cast<float>(fadeTotal_);
                left[frame] = left[frame] * (1.f - oldGain) + oldLeft[frame] * oldGain;
                right[frame] = right[frame] * (1.f - oldGain) + oldRight[frame] * oldGain;
                if (fadeLeft_ > 0) --fadeLeft_;
            }
            if (fadeLeft_ <= 0) {
                retire(outgoing_);
                outgoing_ = nullptr;
                outgoingObserved_.store(nullptr, std::memory_order_release);
            }
        }
        sanitize(left, chunk);
        sanitize(right, chunk);
        if (outputs != nullptr) {
            for (std::uint32_t frame = 0; frame < chunk; ++frame) {
                const float mono = 0.5f * (left[frame] + right[frame]);
                if (channels > 0 && outputs[0] != nullptr) outputs[0][rendered + frame] = channels == 1 ? mono : left[frame];
                if (channels > 1 && outputs[1] != nullptr) outputs[1][rendered + frame] = right[frame];
                for (std::uint32_t channel = 2; channel < channels; ++channel) {
                    if (outputs[channel] != nullptr) outputs[channel][rendered + frame] = 0.f;
                }
            }
        }
        activeVoices_.store(voices_.activeCount(), std::memory_order_relaxed);
        rendered += chunk;
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    const double seconds = std::chrono::duration<double>(elapsed).count();
    const double budget = frames == 0 ? 1.0 : static_cast<double>(frames) / config_.sampleRate;
    const float load = static_cast<float>(seconds / budget);
    if (load > 1.f) overrun = true;
    loadBits_.store(bitsFromFloat(load), std::memory_order_relaxed);
    const auto slot = timingWrite_.fetch_add(1, std::memory_order_relaxed) & 511u;
    timings_[slot].store(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()),
                         std::memory_order_relaxed);
    if (overrun) xruns_.fetch_add(1, std::memory_order_relaxed);
    exits_.fetch_add(1, std::memory_order_acq_rel);
}

void Engine::reclaim() {
    delete reclaim_.exchange(nullptr, std::memory_order_acq_rel);
    delete reclaimExtra_.exchange(nullptr, std::memory_order_acq_rel);
}

Telemetry Engine::telemetry() const {
    Telemetry result;
    result.activeVoices = activeVoices_.load(std::memory_order_relaxed);
    result.xruns = xruns_.load(std::memory_order_relaxed);
    result.lastLoad = floatFromBits(loadBits_.load(std::memory_order_relaxed));
    float loads[512];
    std::uint32_t count = 0;
    const auto write = timingWrite_.load(std::memory_order_relaxed);
    const auto available = std::min<std::uint32_t>(write, 512);
    for (std::uint32_t index = 0; index < available; ++index) {
        const auto nanos = timings_[index].load(std::memory_order_relaxed);
        const double seconds = static_cast<double>(nanos) * 1.0e-9;
        const double budget = config_.maxFrames == 0 ? 1.0 : static_cast<double>(config_.maxFrames) / config_.sampleRate;
        loads[count++] = static_cast<float>(seconds / budget);
    }
    if (count == 0) return result;
    std::sort(loads, loads + count);
    result.loadP50 = loads[count / 2];
    result.loadP99 = loads[std::min(count - 1, (count * 99) / 100)];
    return result;
}

const RuntimePlan* Engine::activePlan() const noexcept { return observed_.load(std::memory_order_acquire); }
} // namespace nodsynth::runtime
