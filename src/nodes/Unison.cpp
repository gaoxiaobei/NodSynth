#include <nodsynth/nodes/Unison.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <numbers>
#include <vector>

namespace nodsynth::nodes {
namespace {
using Table = std::array<float, kSawTableSize + 1>;
using Tables = std::array<Table, kSawTableLevels>;

const Tables& sawTables() {
    // Fourier saw with power-of-two harmonic limits (Smith, bandlimited table synthesis).
    // Initialization happens during bind, never on the audio thread.
    static const Tables tables = [] {
        Tables result{};
        for (std::uint32_t sample = 0; sample < kSawTableSize; ++sample) {
            const double phase = 2 * std::numbers::pi * sample / kSawTableSize;
            double sum = 0;
            std::uint32_t level = 0;
            for (std::uint32_t harmonic = 1; harmonic <= (1u << (kSawTableLevels - 1)); ++harmonic) {
                sum -= 2 / std::numbers::pi * std::sin(phase * harmonic) / harmonic;
                if (harmonic == (1u << level)) result[level++][sample] = static_cast<float>(sum);
            }
        }
        for (auto& table : result) table[kSawTableSize] = table[0];
        return result;
    }();
    return tables;
}

runtime::BufferView port(const std::vector<runtime::BufferView>& ports, const char* name) {
    for (const auto& view : ports) if (view.port.value == name) return view;
    return {};
}

std::uint32_t randomBits(std::uint32_t value) noexcept {
    value ^= value >> 16; value *= 0x7feb352du;
    value ^= value >> 15; value *= 0x846ca68bu;
    return value ^ (value >> 16);
}

class Unison final : public runtime::DspNode {
public:
    void bind(const runtime::NodeBinding& binding) override {
        frequency_ = port(binding.inputs, "frequency");
        output_ = port(binding.outputs, "audio");
        parameters_ = binding.parameters;
        rate_ = binding.sampleRate;
        velocity_ = binding.velocity;
        triggers_ = binding.triggers;
        notes_ = binding.note;
        states_.resize(binding.voiceCount);
        tables_ = &sawTables();
        reset();
    }
    void reset() override {
        std::fill(states_.begin(), states_.end(), State{});
        cachedCount_ = 0;
    }
    void process(std::uint32_t voice, std::uint32_t frames) override {
        auto& state = states_[voice];
        for (std::uint32_t frame = 0; frame < frames; ++frame) {
            const auto count = static_cast<std::uint32_t>(std::clamp(std::lround(parameters_.at(0, frame)), 1l, 8l));
            const bool trigger = triggers_ && triggers_[voice] && triggers_[voice][frame] > .5f;
            if (trigger) {
                const auto seed = static_cast<std::uint32_t>(std::llround(parameters_.at(6, frame)));
                const auto note = notes_ && notes_[voice] ? static_cast<std::uint32_t>(std::lround(notes_[voice][frame])) : 69u;
                const auto serial = ++state.serial;
                for (std::uint32_t index = 0; index < kUnisonMaximum; ++index)
                    state.phases[index] = parameters_.at(3, frame) >= .5f ?
                        static_cast<double>(randomBits(seed ^ (note * 0x9e3779b9u) ^ (serial * 0x85ebca6bu) ^ (index * 0xc2b2ae35u))) / 4294967296.0 : 0;
            }
            update(count, parameters_.at(1, frame), parameters_.at(2, frame), parameters_.at(4, frame));
            float hz = parameters_.at(7, frame);
            if (frequency_.data) {
                const auto input = *frequency_.at(voice, 0, frame);
                if (std::isfinite(input)) hz = input;
            }
            hz = std::clamp(hz, 0.f, static_cast<float>(rate_ * .45));
            double left = 0, right = 0;
            for (std::uint32_t index = 0; index < count; ++index) {
                const double tuned = std::min(hz * ratios_[index], rate_ * .45);
                const auto value = saw(state.phases[index], tuned);
                left += value * left_[index]; right += value * right_[index];
                state.phases[index] += tuned / rate_;
                state.phases[index] -= std::floor(state.phases[index]);
            }
            const float velocity = velocity_ && velocity_[voice] ? velocity_[voice][frame] : 1.f;
            const double level = parameters_.at(5, frame) * velocity;
            *output_.at(voice, 0, frame) = static_cast<float>(left * level);
            *output_.at(voice, 1, frame) = static_cast<float>(right * level);
        }
    }
private:
    struct State {
        std::array<double, kUnisonMaximum> phases{};
        std::uint32_t serial{0};
    };
    double lookup(std::uint32_t level, double phase) const noexcept {
        const double position = phase * kSawTableSize;
        const auto index = static_cast<std::uint32_t>(position);
        const auto& table = (*tables_)[level];
        return table[index] + (table[index + 1] - table[index]) * (position - index);
    }
    double saw(double phase, double frequency) const noexcept {
        if (frequency <= 0) return 0;
        const double allowance = std::clamp(rate_ * .45 / frequency, 1.0, static_cast<double>(1u << kSawTableLevels));
        const auto limit = static_cast<std::uint32_t>(allowance);
        const auto level = std::min(kSawTableLevels - 1, static_cast<std::uint32_t>(std::bit_width(limit) - 1));
        if (!level) return lookup(0, phase);
        // Fade in a bank only once all its harmonics fit below the guard band.
        const double blend = std::clamp(allowance / (1u << level) - 1, 0.0, 1.0);
        return lookup(level - 1, phase) * (1 - blend) + lookup(level, phase) * blend;
    }
    void update(std::uint32_t count, float detune, float spread, float blend) noexcept {
        if (count == cachedCount_ && detune == detune_ && spread == spread_ && blend == blend_) return;
        cachedCount_ = count; detune_ = detune; spread_ = spread; blend_ = blend;
        double sumLeft = 0, sumRight = 0;
        const auto center0 = (count - 1) / 2, center1 = count / 2;
        for (std::uint32_t index = 0; index < count; ++index) {
            const double position = count > 1 ? 2.0 * index / (count - 1) - 1 : 0;
            ratios_[index] = std::exp2(position * detune / 1200);
            const double weight = count == 1 ? 1 : blend / count +
                ((index == center0 || index == center1) ? (1 - blend) / (count % 2 ? 1 : 2) : 0);
            const double angle = (position * spread + 1) * std::numbers::pi / 4;
            left_[index] = weight * std::cos(angle); right_[index] = weight * std::sin(angle);
            sumLeft += left_[index]; sumRight += right_[index];
        }
        for (std::uint32_t index = 0; index < count; ++index) {
            left_[index] /= sumLeft; right_[index] /= sumRight;
        }
    }
    runtime::BufferView frequency_{}, output_{};
    runtime::ParamView parameters_{};
    const float* const* velocity_{nullptr};
    const float* const* triggers_{nullptr};
    const float* const* notes_{nullptr};
    const Tables* tables_{nullptr};
    double rate_{48000};
    std::vector<State> states_;
    std::array<double, kUnisonMaximum> ratios_{}, left_{}, right_{};
    std::uint32_t cachedCount_{0};
    float detune_{0}, spread_{0}, blend_{0};
};
}

std::unique_ptr<runtime::DspNode> makeUnisonOscillator() { return std::make_unique<Unison>(); }
std::uint64_t unisonStateBytes(std::uint32_t voices) noexcept {
    // Charge the shared banks conservatively for every instance, plus all eight phases.
    return sizeof(Tables) + static_cast<std::uint64_t>(voices) * 128 + 1024;
}
}
