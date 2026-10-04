#include <nodsynth/runtime/VoiceAllocator.h>

#include <algorithm>
#include <cmath>

namespace nodsynth::runtime {
namespace {
void stableSort(std::vector<MidiEvent>& events) {
    for (std::size_t index = 1; index < events.size(); ++index) {
        const MidiEvent key = events[index];
        std::size_t cursor = index;
        while (cursor > 0 && events[cursor - 1].sampleOffset > key.sampleOffset) {
            events[cursor] = events[cursor - 1];
            --cursor;
        }
        events[cursor] = key;
    }
}

float* channel(std::vector<float>& storage, std::uint32_t voice, std::uint32_t maxFrames) {
    return storage.data() + static_cast<std::size_t>(voice) * maxFrames;
}
} // namespace

void VoiceAllocator::prepare(
    std::uint32_t voices,
    double sampleRate,
    std::uint32_t maxFrames,
    double releaseHoldSeconds,
    double stealFadeSeconds) {
    voiceCount_ = voices;
    maxFrames_ = maxFrames;
    sampleRate_ = sampleRate;
    activeCount_ = 0;
    age_ = 1;
    voices_.assign(voices, {});
    const auto samples = static_cast<std::size_t>(voices) * maxFrames;
    gates_.assign(samples, 0.f);
    notes_.assign(samples, 0.f);
    velocities_.assign(samples, 0.f);
    triggers_.assign(samples, 0.f);
    attenuations_.assign(samples, 1.f);
    ordered_.clear();
    ordered_.reserve(2048);
    for (auto& pedal : sustain_) pedal = false;
    setReleaseHold(releaseHoldSeconds);
    stealFadeSamples_ = std::max(1, static_cast<int>(std::lround(std::max(0.0, stealFadeSeconds) * sampleRate)));
}

void VoiceAllocator::setReleaseHold(double seconds) noexcept {
    releaseHoldSamples_ = std::max(1, static_cast<int>(std::lround(std::max(0.0, seconds) * sampleRate_)));
}

void VoiceAllocator::panic() noexcept {
    for (std::uint32_t index = 0; index < voiceCount_; ++index) freeVoice(index);
    for (auto& pedal : sustain_) pedal = false;
    activeCount_ = 0;
}

void VoiceAllocator::renderBlock(std::span<const MidiEvent> events, std::uint32_t frames) {
    frames = std::min(frames, maxFrames_);
    ordered_.clear();
    for (const auto& event : events) {
        if (ordered_.size() == ordered_.capacity()) break;
        auto copy = event;
        if (copy.sampleOffset >= frames) copy.sampleOffset = frames == 0 ? 0 : frames - 1;
        ordered_.push_back(copy);
    }
    stableSort(ordered_);

    for (std::uint32_t voice = 0; voice < voiceCount_; ++voice) {
        std::fill_n(channel(triggers_, voice, maxFrames_), frames, 0.f);
    }

    std::size_t eventIndex = 0;
    for (std::uint32_t frame = 0; frame < frames; ++frame) {
        while (eventIndex < ordered_.size() && ordered_[eventIndex].sampleOffset <= frame) {
            const int triggered = apply(ordered_[eventIndex]);
            if (triggered >= 0) channel(triggers_, static_cast<std::uint32_t>(triggered), maxFrames_)[frame] = 1.f;
            ++eventIndex;
        }
        activeCount_ = 0;
        for (std::uint32_t voice = 0; voice < voiceCount_; ++voice) {
            auto& state = voices_[voice];
            if (state.activity == Voice::Activity::releasing) {
                if (--state.releaseSamplesLeft <= 0) freeVoice(voice);
            }
            if (state.activity != Voice::Activity::free) ++activeCount_;
            channel(gates_, voice, maxFrames_)[frame] = state.gate ? 1.f : 0.f;
            channel(notes_, voice, maxFrames_)[frame] = state.activity == Voice::Activity::free ? 0.f : state.note;
            channel(velocities_, voice, maxFrames_)[frame] = state.velocity;
            channel(attenuations_, voice, maxFrames_)[frame] = state.fade;
            if (state.fade < 1.f) state.fade = std::min(1.f, state.fade + state.fadeStep);
        }
    }
}

VoiceStatus VoiceAllocator::status(std::uint32_t voice) const noexcept {
    const auto& state = voices_[voice];
    return {
        state.activity == Voice::Activity::free,
        state.activity == Voice::Activity::releasing,
        state.channel,
        state.note,
        state.velocity,
        state.fade,
    };
}

const float* VoiceAllocator::gate(std::uint32_t voice) const noexcept {
    return gates_.data() + static_cast<std::size_t>(voice) * maxFrames_;
}
const float* VoiceAllocator::note(std::uint32_t voice) const noexcept {
    return notes_.data() + static_cast<std::size_t>(voice) * maxFrames_;
}
const float* VoiceAllocator::velocity(std::uint32_t voice) const noexcept {
    return velocities_.data() + static_cast<std::size_t>(voice) * maxFrames_;
}
const float* VoiceAllocator::triggers(std::uint32_t voice) const noexcept {
    return triggers_.data() + static_cast<std::size_t>(voice) * maxFrames_;
}
const float* VoiceAllocator::attenuation(std::uint32_t voice) const noexcept {
    return attenuations_.data() + static_cast<std::size_t>(voice) * maxFrames_;
}

int VoiceAllocator::apply(const MidiEvent& event) {
    if (event.channel > 15) return -1;
    switch (event.type) {
        case MidiType::noteOn:
            if (event.data1 > 127) return -1;
            if (event.data2 == 0) return apply(MidiEvent{event.sampleOffset, MidiType::noteOff, event.channel, event.data1, 0});
            if (const int existing = findMatch(event.channel, event.data1); existing >= 0) {
                startNote(static_cast<std::uint32_t>(existing), event.channel, event.data1, event.data2 / 127.f, false);
                return existing;
            }
            if (const int chosen = allocateVoice(); chosen >= 0) {
                const bool stolen = voices_[static_cast<std::size_t>(chosen)].activity != Voice::Activity::free;
                startNote(
                    static_cast<std::uint32_t>(chosen), event.channel, event.data1, event.data2 / 127.f, stolen);
                return chosen;
            }
            return -1;
        case MidiType::noteOff:
            if (const int match = findMatch(event.channel, event.data1); match >= 0) {
                if (sustain_[event.channel]) voices_[static_cast<std::size_t>(match)].pendingSustain = true;
                else releaseVoice(static_cast<std::uint32_t>(match));
            }
            return -1;
        case MidiType::sustain:
            sustain_[event.channel] = event.data2 >= 64;
            if (!sustain_[event.channel]) {
                for (std::uint32_t index = 0; index < voiceCount_; ++index) {
                    auto& voice = voices_[index];
                    if (voice.channel == event.channel && voice.pendingSustain) releaseVoice(index);
                }
            }
            return -1;
        case MidiType::allNotesOff:
            for (std::uint32_t index = 0; index < voiceCount_; ++index) {
                if (voices_[index].activity != Voice::Activity::free && voices_[index].channel == event.channel) {
                    releaseVoice(index);
                }
            }
            return -1;
        case MidiType::allSoundOff:
            for (std::uint32_t index = 0; index < voiceCount_; ++index) {
                if (voices_[index].channel == event.channel || voices_[index].activity != Voice::Activity::free) {
                    if (voices_[index].activity != Voice::Activity::free && voices_[index].channel == event.channel) {
                        freeVoice(index);
                    }
                }
            }
            return -1;
    }
    return -1;
}

void VoiceAllocator::startNote(
    std::uint32_t index, std::uint8_t channel, std::uint8_t note, float velocity, bool stolen) {
    auto& voice = voices_[index];
    voice.activity = Voice::Activity::held;
    voice.channel = channel;
    voice.note = note;
    voice.velocity = velocity;
    voice.gate = true;
    voice.pendingSustain = false;
    voice.onAge = age_++;
    voice.releaseSamplesLeft = 0;
    if (stolen) {
        voice.fade = 0.f;
        voice.fadeStep = 1.f / static_cast<float>(std::max(1, stealFadeSamples_));
    } else {
        voice.fade = 1.f;
        voice.fadeStep = 0.f;
    }
}

void VoiceAllocator::releaseVoice(std::uint32_t index) {
    auto& voice = voices_[index];
    if (voice.activity == Voice::Activity::free) return;
    voice.activity = Voice::Activity::releasing;
    voice.gate = false;
    voice.pendingSustain = false;
    voice.releaseAge = age_++;
    voice.releaseSamplesLeft = releaseHoldSamples_;
}

void VoiceAllocator::freeVoice(std::uint32_t index) {
    voices_[index] = {};
    voices_[index].fade = 1.f;
}

int VoiceAllocator::findMatch(std::uint8_t channel, std::uint8_t note) const noexcept {
    int found = -1;
    std::uint64_t newest = 0;
    for (std::uint32_t index = 0; index < voiceCount_; ++index) {
        const auto& voice = voices_[index];
        if (voice.activity == Voice::Activity::free || voice.channel != channel || voice.note != note) continue;
        if (found < 0 || voice.onAge >= newest) {
            found = static_cast<int>(index);
            newest = voice.onAge;
        }
    }
    return found;
}

int VoiceAllocator::allocateVoice() const noexcept {
    for (std::uint32_t index = 0; index < voiceCount_; ++index) {
        if (voices_[index].activity == Voice::Activity::free) return static_cast<int>(index);
    }
    int oldestRelease = -1;
    std::uint64_t oldestReleaseAge = 0;
    int oldestHeld = -1;
    std::uint64_t oldestHeldAge = 0;
    for (std::uint32_t index = 0; index < voiceCount_; ++index) {
        const auto& voice = voices_[index];
        if (voice.activity == Voice::Activity::releasing &&
            (oldestRelease < 0 || voice.releaseAge < oldestReleaseAge)) {
            oldestRelease = static_cast<int>(index);
            oldestReleaseAge = voice.releaseAge;
        }
        if (voice.activity == Voice::Activity::held && (oldestHeld < 0 || voice.onAge < oldestHeldAge)) {
            oldestHeld = static_cast<int>(index);
            oldestHeldAge = voice.onAge;
        }
    }
    return oldestRelease >= 0 ? oldestRelease : oldestHeld;
}
} // namespace nodsynth::runtime
