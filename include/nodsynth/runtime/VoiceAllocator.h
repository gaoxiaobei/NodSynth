#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace nodsynth::runtime {
enum class MidiType : std::uint8_t { noteOn, noteOff, sustain, allNotesOff, allSoundOff, pitchBend };

inline constexpr float kPitchBendSemitones = 2.f;
inline constexpr std::uint32_t kMaxBlockMidiEvents = 2048;

struct MidiEvent {
    std::uint32_t sampleOffset{0};
    MidiType type{MidiType::noteOn};
    std::uint8_t channel{0};
    std::uint8_t data1{0};
    std::uint8_t data2{0};
};

struct VoiceStatus {
    bool free{true};
    bool releasing{false};
    std::uint8_t channel{0};
    std::uint8_t note{0};
    float velocity{0.f};
    float attenuation{1.f};
};

class VoiceAllocator {
public:
    void prepare(
        std::uint32_t voices,
        double sampleRate,
        std::uint32_t maxFrames,
        double releaseHoldSeconds,
        double stealFadeSeconds);
    void setReleaseHold(double seconds) noexcept;
    void panic() noexcept;

    void renderBlock(std::span<const MidiEvent> events, std::uint32_t frames);

    [[nodiscard]] std::uint32_t voiceCount() const noexcept { return voiceCount_; }
    [[nodiscard]] std::uint32_t maxFrames() const noexcept { return maxFrames_; }
    [[nodiscard]] std::uint32_t activeCount() const noexcept { return activeCount_; }
    [[nodiscard]] VoiceStatus status(std::uint32_t voice) const noexcept;
    [[nodiscard]] const float* gate(std::uint32_t voice) const noexcept;
    [[nodiscard]] const float* note(std::uint32_t voice) const noexcept;
    [[nodiscard]] const float* velocity(std::uint32_t voice) const noexcept;
    [[nodiscard]] const float* triggers(std::uint32_t voice) const noexcept;
    [[nodiscard]] const float* attenuation(std::uint32_t voice) const noexcept;

private:
    struct Voice {
        enum class Activity : std::uint8_t { free, held, releasing };
        Activity activity{Activity::free};
        std::uint8_t channel{0};
        std::uint8_t note{0};
        float velocity{0.f};
        float fade{1.f};
        float fadeStep{0.f};
        bool pendingSustain{false};
        bool gate{false};
        std::uint64_t onAge{0};
        std::uint64_t releaseAge{0};
        int releaseSamplesLeft{0};
    };

    int apply(const MidiEvent& event);
    void startNote(std::uint32_t index, std::uint8_t channel, std::uint8_t note, float velocity, bool stolen);
    void releaseVoice(std::uint32_t index);
    void freeVoice(std::uint32_t index);
    [[nodiscard]] int findMatch(std::uint8_t channel, std::uint8_t note) const noexcept;
    [[nodiscard]] int allocateVoice() const noexcept;

    std::uint32_t voiceCount_{0};
    std::uint32_t maxFrames_{0};
    std::uint32_t activeCount_{0};
    double sampleRate_{48000.0};
    int releaseHoldSamples_{1};
    int stealFadeSamples_{1};
    std::uint64_t age_{1};
    bool sustain_[16]{};
    float bendSemitones_[16]{};
    std::vector<Voice> voices_;
    std::vector<float> gates_;
    std::vector<float> notes_;
    std::vector<float> velocities_;
    std::vector<float> triggers_;
    std::vector<float> attenuations_;
    std::vector<MidiEvent> ordered_;
};
} // namespace nodsynth::runtime
