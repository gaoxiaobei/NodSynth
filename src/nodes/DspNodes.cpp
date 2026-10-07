#include <nodsynth/nodes/BuiltinNodes.h>
#include <nodsynth/nodes/GlobalEffect.h>
#include <nodsynth/nodes/Unison.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <string_view>
#include <vector>

namespace nodsynth::nodes {
namespace {
using runtime::BufferView;
using runtime::NodeBinding;

float load(const BufferView& view, std::uint32_t voice, std::uint32_t channel, std::uint32_t frame, float fallback) {
    if (view.data == nullptr || channel >= view.channels) return fallback;
    const float value = *view.at(voice, channel, frame);
    return std::isfinite(value) ? value : fallback;
}

void store(const BufferView& view, std::uint32_t voice, std::uint32_t channel, std::uint32_t frame, float value) {
    if (view.data == nullptr || channel >= view.channels) return;
    *view.at(voice, channel, frame) = std::isfinite(value) ? value : 0.f;
}

BufferView port(const std::vector<BufferView>& ports, std::string_view id) {
    for (const auto& candidate : ports) {
        if (candidate.port.value == id) return candidate;
    }
    return {};
}

float polyBlep(float phase, float delta) {
    if (!(delta > 0.f)) return 0.f;
    if (phase < delta) {
        phase /= delta;
        return phase + phase - phase * phase - 1.f;
    }
    if (phase > 1.f - delta) {
        phase = (phase - 1.f) / delta;
        return phase * phase + phase + phase + 1.f;
    }
    return 0.f;
}

class MidiInput final : public runtime::DspNode {
public:
    void bind(const NodeBinding& binding) override {
        noteOut_ = port(binding.outputs, "note");
        gateOut_ = port(binding.outputs, "gate");
        notes_ = binding.note;
        gates_ = binding.gate;
    }
    void reset() override {}
    void process(std::uint32_t voice, std::uint32_t frames) override {
        const float* note = notes_ == nullptr ? nullptr : notes_[voice];
        const float* gate = gates_ == nullptr ? nullptr : gates_[voice];
        for (std::uint32_t frame = 0; frame < frames; ++frame) {
            store(noteOut_, voice, 0, frame, note == nullptr ? 0.f : note[frame]);
            store(gateOut_, voice, 0, frame, gate == nullptr ? 0.f : gate[frame]);
        }
    }

private:
    BufferView noteOut_{};
    BufferView gateOut_{};
    const float* const* notes_{nullptr};
    const float* const* gates_{nullptr};
};

class NoteToFrequency final : public runtime::DspNode {
public:
    void bind(const NodeBinding& binding) override {
        note_ = port(binding.inputs, "note");
        frequency_ = port(binding.outputs, "frequency");
    }
    void reset() override {}
    void process(std::uint32_t voice, std::uint32_t frames) override {
        for (std::uint32_t frame = 0; frame < frames; ++frame) {
            const float midi = load(note_, voice, 0, frame, 69.f);
            store(frequency_, voice, 0, frame, 440.f * std::exp2((midi - 69.f) / 12.f));
        }
    }

private:
    BufferView note_{};
    BufferView frequency_{};
};

class Oscillator final : public runtime::DspNode {
public:
    void bind(const NodeBinding& binding) override {
        frequency_ = port(binding.inputs, "frequency");
        audio_ = port(binding.outputs, "audio");
        parameters_ = binding.parameters;
        sampleRate_ = binding.sampleRate;
        velocity_ = binding.velocity;
        triggers_ = binding.triggers;
        phases_.assign(binding.voiceCount, 0.0);
        integrators_.assign(binding.voiceCount, 0.f);
    }
    void reset() override {
        std::fill(phases_.begin(), phases_.end(), 0.0);
        std::fill(integrators_.begin(), integrators_.end(), 0.f);
    }
    void process(std::uint32_t voice, std::uint32_t frames) override {
        for (std::uint32_t frame = 0; frame < frames; ++frame) {
            if (triggers_ != nullptr && triggers_[voice] != nullptr && triggers_[voice][frame] > 0.5f) {
                phases_[voice] = 0.0;
                integrators_[voice] = 0.f;
            }
            const float fallback = parameters_.count > 2 ? parameters_.at(2, frame) : 440.f;
            float hz = std::clamp(load(frequency_, voice, 0, frame, fallback), 0.f, 20000.f);
            hz = std::min(hz, static_cast<float>(sampleRate_ * 0.45));
            const int waveform = parameters_.count > 0 ? std::clamp(static_cast<int>(std::lround(parameters_.at(0, frame))), 0, 3) : 0;
            const float level = parameters_.count > 1 ? parameters_.at(1, frame) : 0.2f;
            const float velocity = velocity_ != nullptr && velocity_[voice] != nullptr ? velocity_[voice][frame] : 1.f;
            const float delta = static_cast<float>(hz / sampleRate_);
            const float phase = static_cast<float>(phases_[voice]);
            float sample = 0.f;
            if (waveform == 0) sample = std::sin(phase * static_cast<float>(2.0 * std::numbers::pi));
            else if (waveform == 1) {
                const float saw = (2.f * phase - 1.f) - polyBlep(phase, delta);
                integrators_[voice] = std::clamp(integrators_[voice] * 0.995f + saw * delta, -1.f, 1.f);
                sample = integrators_[voice] * 2.f;
            } else if (waveform == 2) sample = (2.f * phase - 1.f) - polyBlep(phase, delta);
            else {
                sample = (phase < 0.5f ? 1.f : -1.f) + polyBlep(phase, delta);
                sample -= polyBlep(std::fmod(phase + 0.5f, 1.f), delta);
            }
            store(audio_, voice, 0, frame, sample * level * velocity);
            phases_[voice] += delta;
            if (phases_[voice] >= 1.0) phases_[voice] -= std::floor(phases_[voice]);
        }
    }

private:
    BufferView frequency_{};
    BufferView audio_{};
    runtime::ParamView parameters_{};
    const float* const* velocity_{nullptr};
    const float* const* triggers_{nullptr};
    double sampleRate_{48000.0};
    std::vector<double> phases_;
    std::vector<float> integrators_;
};

class Adsr final : public runtime::DspNode {
public:
    void bind(const NodeBinding& binding) override {
        gate_ = port(binding.inputs, "gate");
        envelope_ = port(binding.outputs, "envelope");
        parameters_ = binding.parameters;
        triggers_ = binding.triggers;
        sampleRate_ = binding.sampleRate;
        states_.assign(binding.voiceCount, {});
    }
    void reset() override { std::fill(states_.begin(), states_.end(), State{}); }
    void process(std::uint32_t voice, std::uint32_t frames) override {
        auto& state = states_[voice];
        for (std::uint32_t frame = 0; frame < frames; ++frame) {
            const float gate = load(gate_, voice, 0, frame, 0.f);
            const bool high = gate > 0.5f;
            const bool trigger = triggers_ != nullptr && triggers_[voice] != nullptr && triggers_[voice][frame] > 0.5f;
            if (trigger || (high && state.previous <= 0.5f)) state.stage = Stage::attack;
            else if (!high && state.previous > 0.5f) {
                state.stage = Stage::release;
                state.releaseLevel = state.level;
            }
            state.previous = high ? 1.f : 0.f;
            const float attack = std::max(0.f, parameters_.at(0, frame));
            const float decay = std::max(0.f, parameters_.at(1, frame));
            const float sustain = std::clamp(parameters_.at(2, frame), 0.f, 1.f);
            const float release = std::max(0.f, parameters_.at(3, frame));
            const float attackStep = attack * sampleRate_ <= 1.0 ? 1.f : static_cast<float>(1.0 / (attack * sampleRate_));
            const float decayStep = decay * sampleRate_ <= 1.0 ? 1.f : static_cast<float>((1.0 - sustain) / (decay * sampleRate_));
            const float releaseStep = release * sampleRate_ <= 1.0 ? state.releaseLevel
                                                                   : static_cast<float>(state.releaseLevel / (release * sampleRate_));
            switch (state.stage) {
                case Stage::attack:
                    state.level += attackStep;
                    if (state.level >= 1.f) {
                        state.level = 1.f;
                        state.stage = Stage::decay;
                    }
                    break;
                case Stage::decay:
                    state.level -= decayStep;
                    if (state.level <= sustain) {
                        state.level = sustain;
                        state.stage = Stage::sustain;
                    }
                    break;
                case Stage::sustain: state.level = sustain; break;
                case Stage::release:
                    state.level -= releaseStep;
                    if (state.level <= 0.f) {
                        state.level = 0.f;
                        state.stage = Stage::idle;
                    }
                    break;
                case Stage::idle: state.level = 0.f; break;
            }
            store(envelope_, voice, 0, frame, std::clamp(state.level, 0.f, 1.f));
        }
    }

private:
    enum class Stage : std::uint8_t { idle, attack, decay, sustain, release };
    struct State {
        float level{0.f};
        float releaseLevel{0.f};
        float previous{0.f};
        Stage stage{Stage::idle};
    };
    BufferView gate_{};
    BufferView envelope_{};
    runtime::ParamView parameters_{};
    const float* const* triggers_{nullptr};
    double sampleRate_{48000.0};
    std::vector<State> states_;
};

class Gain final : public runtime::DspNode {
public:
    void bind(const NodeBinding& binding) override {
        audio_ = port(binding.inputs, "audio-in");
        gain_ = port(binding.inputs, "gain");
        output_ = port(binding.outputs, "audio-out");
        parameters_ = binding.parameters;
    }
    void reset() override {}
    void process(std::uint32_t voice, std::uint32_t frames) override {
        for (std::uint32_t frame = 0; frame < frames; ++frame) {
            const float base = parameters_.at(0, frame);
            const float amount = std::clamp(output_.channels == 2 ? base * load(gain_, voice, 0, frame, 1.f) :
                load(gain_, voice, 0, frame, base), 0.f, 4.f);
            for (std::uint32_t channel = 0; channel < output_.channels; ++channel)
                store(output_, voice, channel, frame, load(audio_, voice, channel, frame, 0.f) * amount);
        }
    }

private:
    BufferView audio_{};
    BufferView gain_{};
    BufferView output_{};
    runtime::ParamView parameters_{};
};

class Noise final : public runtime::DspNode {
public:
    void bind(const NodeBinding& binding) override {
        audio_ = port(binding.outputs, "audio");
        parameters_ = binding.parameters;
        velocity_ = binding.velocity;
        triggers_ = binding.triggers;
        states_.assign(binding.voiceCount, 0u);
        primed_.assign(binding.voiceCount, 0);
    }
    void reset() override {
        std::fill(states_.begin(), states_.end(), 0u);
        std::fill(primed_.begin(), primed_.end(), 0);
    }
    void process(std::uint32_t voice, std::uint32_t frames) override {
        for (std::uint32_t frame = 0; frame < frames; ++frame) {
            const auto seed = static_cast<std::uint32_t>(
                std::llround(std::clamp(parameters_.count > 1 ? parameters_.at(1, frame) : 1.f, 0.f, 2147483647.f)));
            const bool trigger = triggers_ != nullptr && triggers_[voice] != nullptr && triggers_[voice][frame] > 0.5f;
            if (trigger || primed_[voice] == 0) {
                states_[voice] = seed;
                primed_[voice] = 1;
            }
            states_[voice] = states_[voice] * 1664525u + 1013904223u;
            const auto bits = static_cast<std::int32_t>(states_[voice]) >> 8;
            const float level = parameters_.count > 0 ? std::clamp(parameters_.at(0, frame), 0.f, 1.f) : 0.2f;
            const float velocity = velocity_ != nullptr && velocity_[voice] != nullptr ? velocity_[voice][frame] : 1.f;
            store(audio_, voice, 0, frame, static_cast<float>(bits) * (1.f / 8388608.f) * level * velocity);
        }
    }

private:
    BufferView audio_{};
    runtime::ParamView parameters_{};
    const float* const* velocity_{nullptr};
    const float* const* triggers_{nullptr};
    std::vector<std::uint32_t> states_;
    std::vector<std::uint8_t> primed_;
};

class SvfFilter {
public:
    void bind(const NodeBinding& binding) {
        audio_ = port(binding.inputs, "audio-in");
        cutoff_ = port(binding.inputs, "cutoff");
        modulation_ = port(binding.inputs, "cutoff-mod");
        resonance_ = port(binding.inputs, "resonance");
        output_ = port(binding.outputs, "audio-out");
        parameters_ = binding.parameters;
        sampleRate_ = binding.sampleRate;
        states_.assign(binding.voiceCount * output_.channels, {});
    }
    void reset() { std::fill(states_.begin(), states_.end(), State{}); }
    void process(std::uint32_t voice, std::uint32_t frames, bool highpass) {
        for (std::uint32_t frame = 0; frame < frames; ++frame) {
            float cutoffValue = load(cutoff_, voice, 0, frame, parameters_.at(0, frame));
            if (parameters_.count > 2) {
                const float mod = std::clamp(load(modulation_, voice, 0, frame, 0.f), -1.f, 1.f);
                cutoffValue = std::clamp(parameters_.at(0, frame) * std::exp2(parameters_.at(2, frame) * mod), 20.f, 20000.f);
            }
            const float cutoff = std::clamp(
                cutoffValue, 20.f, static_cast<float>(sampleRate_ * 0.45));
            const float resonance = std::clamp(load(resonance_, voice, 0, frame, parameters_.at(1, frame)), 0.f, 0.98f);
            const float g = std::tan(static_cast<float>(std::numbers::pi) * cutoff / static_cast<float>(sampleRate_));
            const float k = 2.f * (1.f - resonance);
            const float a1 = 1.f / (1.f + g * (g + k));
            const float a2 = g * a1;
            const float a3 = g * a2;
            for (std::uint32_t channel = 0; channel < output_.channels; ++channel) {
                auto& state = states_[voice * output_.channels + channel];
                const float input = load(audio_, voice, channel, frame, 0.f);
                const float v3 = input - state.ic2;
                const float v1 = a1 * state.ic1 + a2 * v3;
                const float v2 = state.ic2 + a2 * state.ic1 + a3 * v3;
                state.ic1 = 2.f * v1 - state.ic1;
                state.ic2 = 2.f * v2 - state.ic2;
                if (!std::isfinite(state.ic1) || !std::isfinite(state.ic2)) state = {};
                // v1 retains its historical state output; v2 uses the TPT lowpass output.
                store(output_, voice, channel, frame, highpass ? input - k * v1 - v2 : parameters_.count>2?v2:state.ic2);
            }
        }
    }

private:
    struct State {
        float ic1{0.f};
        float ic2{0.f};
    };
    BufferView audio_{};
    BufferView cutoff_{};
    BufferView modulation_{};
    BufferView resonance_{};
    BufferView output_{};
    runtime::ParamView parameters_{};
    double sampleRate_{48000.0};
    std::vector<State> states_;
};

class Lowpass final : public runtime::DspNode {
public:
    void bind(const NodeBinding& binding) override { filter_.bind(binding); }
    void reset() override { filter_.reset(); }
    void process(std::uint32_t voice, std::uint32_t frames) override { filter_.process(voice, frames, false); }

private:
    SvfFilter filter_;
};

class Highpass final : public runtime::DspNode {
public:
    void bind(const NodeBinding& binding) override { filter_.bind(binding); }
    void reset() override { filter_.reset(); }
    void process(std::uint32_t voice, std::uint32_t frames) override { filter_.process(voice, frames, true); }

private:
    SvfFilter filter_;
};

class FeedbackDelay final : public runtime::DspNode {
public:
    void bind(const NodeBinding& binding) override {
        input_ = port(binding.inputs, "audio-in");
        feedback_ = port(binding.inputs, "feedback");
        output_ = port(binding.outputs, "audio-out");
        parameters_ = binding.parameters;
        sampleRate_ = binding.sampleRate;
        capacity_ = runtime::delayLineCapacity(binding.sampleRate, binding.maxFrames);
        minimum_ = binding.maxFrames;
        line_.assign(static_cast<std::size_t>(binding.voiceCount) * capacity_, 0.f);
        history_.assign(static_cast<std::size_t>(binding.voiceCount) * binding.maxFrames, 0.f);
        write_.assign(binding.voiceCount, 0u);
        maxFrames_ = binding.maxFrames;
    }
    void reset() override {
        std::fill(line_.begin(), line_.end(), 0.f);
        std::fill(history_.begin(), history_.end(), 0.f);
        std::fill(write_.begin(), write_.end(), 0u);
    }
    bool needsCommit() const noexcept override { return true; }
    void process(std::uint32_t voice, std::uint32_t frames) override {
        for (std::uint32_t frame = 0; frame < frames; ++frame) {
            const auto delay = delaySamples(parameters_.at(0, frame));
            const auto read = wrap(static_cast<std::int64_t>(write_[voice]) - static_cast<std::int64_t>(delay));
            const float value = line_[static_cast<std::size_t>(voice) * capacity_ + read];
            history_[static_cast<std::size_t>(voice) * maxFrames_ + frame] = value;
            store(output_, voice, 0, frame, value);
        }
    }
    void commit(std::uint32_t voice, std::uint32_t frames) override {
        for (std::uint32_t frame = 0; frame < frames; ++frame) {
            const float feedback = std::clamp(load(feedback_, voice, 0, frame, parameters_.at(1, frame)), 0.f, 0.95f);
            const float dry = load(input_, voice, 0, frame, 0.f);
            const float wet = history_[static_cast<std::size_t>(voice) * maxFrames_ + frame];
            line_[static_cast<std::size_t>(voice) * capacity_ + write_[voice]] = dry + feedback * wet;
            write_[voice] = (write_[voice] + 1) % capacity_;
        }
    }

private:
    [[nodiscard]] std::uint32_t delaySamples(float seconds) const {
        const auto rounded = std::lround(static_cast<double>(seconds) * sampleRate_);
        const auto requested = rounded <= 0 ? 0u : static_cast<std::uint32_t>(rounded);
        return std::min(capacity_ - 1, std::max(minimum_, std::max(1u, requested)));
    }
    [[nodiscard]] std::uint32_t wrap(std::int64_t position) const {
        auto wrapped = position % static_cast<std::int64_t>(capacity_);
        if (wrapped < 0) wrapped += capacity_;
        return static_cast<std::uint32_t>(wrapped);
    }

    BufferView input_{};
    BufferView feedback_{};
    BufferView output_{};
    runtime::ParamView parameters_{};
    double sampleRate_{48000.0};
    std::uint32_t capacity_{1};
    std::uint32_t minimum_{1};
    std::uint32_t maxFrames_{1};
    std::vector<float> line_;
    std::vector<float> history_;
    std::vector<std::uint32_t> write_;
};

class Add final : public runtime::DspNode {
public:
    void bind(const NodeBinding& binding) override {
        a_ = port(binding.inputs, "a");
        b_ = port(binding.inputs, "b");
        sum_ = port(binding.outputs, "sum");
    }
    void reset() override {}
    void process(std::uint32_t voice, std::uint32_t frames) override {
        for (std::uint32_t frame = 0; frame < frames; ++frame) {
            store(sum_, voice, 0, frame, load(a_, voice, 0, frame, 0.f) + load(b_, voice, 0, frame, 0.f));
        }
    }

private:
    BufferView a_{};
    BufferView b_{};
    BufferView sum_{};
};

class Multiply final : public runtime::DspNode {
public:
    void bind(const NodeBinding& binding) override {
        a_ = port(binding.inputs, "a");
        b_ = port(binding.inputs, "b");
        product_ = port(binding.outputs, "product");
    }
    void reset() override {}
    void process(std::uint32_t voice, std::uint32_t frames) override {
        for (std::uint32_t frame = 0; frame < frames; ++frame) {
            store(product_, voice, 0, frame, load(a_, voice, 0, frame, 0.f) * load(b_, voice, 0, frame, 1.f));
        }
    }

private:
    BufferView a_{};
    BufferView b_{};
    BufferView product_{};
};

class ScaleBias final : public runtime::DspNode {
public:
    void bind(const NodeBinding& binding) override {
        input_ = port(binding.inputs, "input");
        output_ = port(binding.outputs, "output");
        parameters_ = binding.parameters;
    }
    void reset() override {}
    void process(std::uint32_t voice, std::uint32_t frames) override {
        for (std::uint32_t frame = 0; frame < frames; ++frame) {
            store(
                output_,
                voice,
                0,
                frame,
                load(input_, voice, 0, frame, 0.f) * parameters_.at(0, frame) + parameters_.at(1, frame));
        }
    }

private:
    BufferView input_{};
    BufferView output_{};
    runtime::ParamView parameters_{};
};

class Mix final : public runtime::DspNode {
public:
    void bind(const NodeBinding& binding) override {
        a_ = port(binding.inputs, "a");
        b_ = port(binding.inputs, "b");
        mix_ = port(binding.inputs, "mix");
        output_ = port(binding.outputs, "audio");
        parameters_ = binding.parameters;
    }
    void reset() override {}
    void process(std::uint32_t voice, std::uint32_t frames) override {
        for (std::uint32_t frame = 0; frame < frames; ++frame) {
            const float mix = std::clamp(load(mix_, voice, 0, frame, parameters_.at(0, frame)), 0.f, 1.f);
            for (std::uint32_t channel = 0; channel < output_.channels; ++channel) {
                const float mixed = load(a_, voice, channel, frame, 0.f) * (1.f - mix) + load(b_, voice, channel, frame, 0.f) * mix;
                store(output_, voice, channel, frame, mixed);
            }
        }
    }

private:
    BufferView a_{};
    BufferView b_{};
    BufferView mix_{};
    BufferView output_{};
    runtime::ParamView parameters_{};
};

class VoiceMix final : public runtime::DspNode {
public:
    void bind(const NodeBinding& binding) override {
        voices_ = port(binding.inputs, "voices");
        output_ = port(binding.outputs, "audio");
        parameters_ = binding.parameters;
        attenuation_ = binding.attenuation;
        voiceCount_ = binding.voiceCount;
    }
    void reset() override {}
    void process(std::uint32_t, std::uint32_t frames) override {
        for (std::uint32_t frame = 0; frame < frames; ++frame) {
            for (std::uint32_t channel = 0; channel < 2; ++channel) {
                float sum = 0.f;
                for (std::uint32_t voice = 0; voice < voiceCount_; ++voice) {
                    const float attenuation =
                        attenuation_ != nullptr && attenuation_[voice] != nullptr ? attenuation_[voice][frame] : 1.f;
                    sum += load(voices_, voice, voices_.channels == 2 ? channel : 0, frame, 0.f) * attenuation;
                }
                const float level = parameters_.count > 0 ? parameters_.at(0, frame) : 1.f;
                store(output_, 0, channel, frame, sum * level);
            }
        }
    }

private:
    BufferView voices_{};
    BufferView output_{};
    runtime::ParamView parameters_{};
    const float* const* attenuation_{nullptr};
    std::uint32_t voiceCount_{1};
};

class Pan final : public runtime::DspNode {
public:
    void bind(const NodeBinding& binding) override {
        input_ = port(binding.inputs, "audio-in");
        output_ = port(binding.outputs, "audio-out");
        parameters_ = binding.parameters;
    }
    void reset() override {}
    void process(std::uint32_t voice, std::uint32_t frames) override {
        for (std::uint32_t frame = 0; frame < frames; ++frame) {
            const float angle = (parameters_.at(0, frame) + 1.f) * static_cast<float>(std::numbers::pi / 4);
            const float sample = load(input_, voice, 0, frame, 0.f);
            store(output_, voice, 0, frame, sample * std::cos(angle));
            store(output_, voice, 1, frame, sample * std::sin(angle));
        }
    }
private:
    BufferView input_{}, output_{};
    runtime::ParamView parameters_{};
};

class AudioOutput final : public runtime::DspNode {
public:
    void bind(const NodeBinding& binding) override { audio_ = port(binding.inputs, "audio"); }
    void reset() override {}
    void setMix(float* left, float* right) override {
        left_ = left;
        right_ = right;
    }
    void process(std::uint32_t, std::uint32_t frames) override {
        for (std::uint32_t frame = 0; frame < frames; ++frame) {
            const float left = load(audio_, 0, 0, frame, 0.f);
            const float right = audio_.channels > 1 ? load(audio_, 0, 1, frame, left) : left;
            if (left_ != nullptr) left_[frame] += left;
            if (right_ != nullptr) right_[frame] += right;
        }
    }

private:
    BufferView audio_{};
    float* left_{nullptr};
    float* right_{nullptr};
};

std::uint64_t stateBytesFor(
    const model::NodeTypeId& typeId, double sampleRate, std::uint32_t maxFrames, std::uint32_t voices) {
    if (typeId.value == "nod.unison-v2") return unisonStateBytes(voices);
    if(typeId.value=="nod.music-delay" || typeId.value=="nod.reverb") return globalEffectStateBytes(typeId.value=="nod.music-delay"?"delay":"reverb",sampleRate,maxFrames);
    if (typeId.value == "nod.feedback-delay") {
        const auto capacity = static_cast<std::uint64_t>(runtime::delayLineCapacity(sampleRate, maxFrames));
        return voices * (capacity + static_cast<std::uint64_t>(maxFrames) * 2ull) * sizeof(float) + 256ull;
    }
    return static_cast<std::uint64_t>(voices) * 64ull + 128ull;
}

class BuiltinImplementations final : public runtime::ImplementationRegistry {
public:
    bool contains(const model::NodeTypeId& typeId) const override {
        const auto& id = typeId.value;
        return id == "nod.midi-input" || id == "nod.note-to-frequency" || id == "nod.oscillator" || id == "nod.adsr" ||
               id == "nod.gain" || id == "nod.noise" || id == "nod.lowpass" || id == "nod.highpass" ||
               id == "nod.feedback-delay" || id == "nod.add" || id == "nod.multiply" || id == "nod.scale-bias" ||
               id == "nod.mix" || id == "nod.voice-mix" || id == "nod.audio-output" ||
               id == "nod.lowpass-v2" || id == "nod.highpass-v2" || id == "nod.gain-v2" ||
               id == "nod.mix-v2" || id == "nod.voice-mix-v2" || id == "nod.pan-v2" || id == "nod.unison-v2" || id=="nod.music-delay" || id=="nod.reverb";
    }
    std::uint64_t stateBytes(
        const model::NodeTypeId& typeId, double sampleRate, std::uint32_t maxFrames, std::uint32_t voices) const override {
        return stateBytesFor(typeId, sampleRate, maxFrames, voices);
    }
    std::unique_ptr<runtime::DspNode> instantiate(const model::NodeTypeId& typeId) const override {
        const auto& id = typeId.value;
        if (id == "nod.unison-v2") return makeUnisonOscillator();
        if(id=="nod.music-delay" || id=="nod.reverb") return makeGlobalEffect(id=="nod.music-delay"?"delay":"reverb");
        if (id == "nod.midi-input") return std::make_unique<MidiInput>();
        if (id == "nod.note-to-frequency") return std::make_unique<NoteToFrequency>();
        if (id == "nod.oscillator") return std::make_unique<Oscillator>();
        if (id == "nod.adsr") return std::make_unique<Adsr>();
        if (id == "nod.gain" || id == "nod.gain-v2") return std::make_unique<Gain>();
        if (id == "nod.noise") return std::make_unique<Noise>();
        if (id == "nod.lowpass" || id == "nod.lowpass-v2") return std::make_unique<Lowpass>();
        if (id == "nod.highpass" || id == "nod.highpass-v2") return std::make_unique<Highpass>();
        if (id == "nod.feedback-delay") return std::make_unique<FeedbackDelay>();
        if (id == "nod.add") return std::make_unique<Add>();
        if (id == "nod.multiply") return std::make_unique<Multiply>();
        if (id == "nod.scale-bias") return std::make_unique<ScaleBias>();
        if (id == "nod.mix" || id == "nod.mix-v2") return std::make_unique<Mix>();
        if (id == "nod.voice-mix" || id == "nod.voice-mix-v2") return std::make_unique<VoiceMix>();
        if (id == "nod.pan-v2") return std::make_unique<Pan>();
        if (id == "nod.audio-output") return std::make_unique<AudioOutput>();
        return nullptr;
    }
};
} // namespace

const runtime::ImplementationRegistry& builtinImplementations() {
    static const BuiltinImplementations implementations;
    return implementations;
}
} // namespace nodsynth::nodes
