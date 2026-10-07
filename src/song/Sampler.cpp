#include <nodsynth/song/Sampler.h>
#include <nodsynth/runtime/WavFile.h>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace nodsynth::song {
persist::Json sampleLayersJson(const std::vector<SampleLayer>& layers) {
    auto result = persist::Json::array();
    for (const auto& layer : layers) {
        auto item = persist::Json::object();
        item.set("resource", persist::Json::string(layer.resourceId));
        item.set("rootNote", persist::Json::number(layer.rootNote));
        item.set("lowNote", persist::Json::number(layer.lowNote));
        item.set("highNote", persist::Json::number(layer.highNote));
        item.set("lowVelocity", persist::Json::number(layer.lowVelocity));
        item.set("highVelocity", persist::Json::number(layer.highVelocity));
        item.set("chokeGroup", persist::Json::number(layer.chokeGroup));
        item.set("tuneCents", persist::Json::number(layer.tuneCents));
        item.set("gain", persist::Json::number(layer.gain));
        item.set("startSeconds", persist::Json::number(layer.startSeconds));
        item.set("endSeconds", persist::Json::number(layer.endSeconds));
        item.set("fadeInMs", persist::Json::number(layer.fadeInMs));
        item.set("fadeOutMs", persist::Json::number(layer.fadeOutMs));
        result.push(std::move(item));
    }
    return result;
}

bool parseSampleLayers(const persist::Json& value, std::vector<SampleLayer>& layers, std::string& error) {
    if (value.kind() != persist::Json::Kind::array || value.asArray().empty() || value.asArray().size() > 128) {
        error = "samples must contain 1..128 layers"; return false;
    }
    std::vector<SampleLayer> parsed;
    for (const auto& item : value.asArray()) {
        SampleLayer layer;
        const auto* resource = item.find("resource");
        if (!resource || resource->kind() != persist::Json::Kind::string || resource->asString().empty()) {
            error = "sample layer requires a resource id"; return false;
        }
        layer.resourceId = resource->asString();
        auto number = [&](const char* name, double& target) {
            const auto* field = item.find(name);
            if (!field) return true;
            if (field->kind() != persist::Json::Kind::number || !std::isfinite(field->asNumber())) return false;
            target = field->asNumber(); return true;
        };
        auto integer = [&](const char* name, int& target) {
            double wide = target;
            if (!number(name, wide) || wide != std::floor(wide) || wide < 0 || wide > 127) return false;
            target = static_cast<int>(wide); return true;
        };
        if (!integer("rootNote", layer.rootNote) || !integer("lowNote", layer.lowNote) || !integer("highNote", layer.highNote) ||
            !integer("lowVelocity", layer.lowVelocity) || !integer("highVelocity", layer.highVelocity) || !integer("chokeGroup", layer.chokeGroup) ||
            !number("tuneCents", layer.tuneCents) || !number("gain", layer.gain) || !number("startSeconds", layer.startSeconds) ||
            !number("endSeconds", layer.endSeconds) || !number("fadeInMs", layer.fadeInMs) || !number("fadeOutMs", layer.fadeOutMs)) {
            error = "sample layer fields must be finite numbers and MIDI ranges must be integers in 0..127"; return false;
        }
        parsed.push_back(std::move(layer));
    }
    layers = std::move(parsed); return true;
}

bool validateSampleLayers(const Instrument& instrument, const SongDocument& song, std::string& error) {
    if (instrument.samples.empty() || instrument.samples.size() > 128) { error = "sampler requires 1..128 sample layers"; return false; }
    for (std::size_t index = 0; index < instrument.samples.size(); ++index) {
        const auto& a = instrument.samples[index];
        const auto resource = std::find_if(song.resources.begin(), song.resources.end(), [&](const auto& r) { return r.id == a.resourceId; });
        if (resource == song.resources.end() || resource->kind != "sample") { error = "sample layer resource must exist and have kind sample: " + a.resourceId; return false; }
        if (resource->hash.empty()) { error = "sample resources require a content hash: " + a.resourceId; return false; }
        if (a.rootNote < 0 || a.rootNote > 127 || a.lowNote < 0 || a.highNote > 127 || a.lowNote > a.highNote ||
            a.lowVelocity < 1 || a.highVelocity > 127 || a.lowVelocity > a.highVelocity || a.chokeGroup < 0 || a.chokeGroup > 127 ||
            !std::isfinite(a.tuneCents) || std::fabs(a.tuneCents) > 2400 || !std::isfinite(a.gain) || a.gain < 0 || a.gain > 4 ||
            !std::isfinite(a.startSeconds) || a.startSeconds < 0 || !std::isfinite(a.endSeconds) || a.endSeconds < 0 ||
            (a.endSeconds && a.endSeconds <= a.startSeconds) || !std::isfinite(a.fadeInMs) || a.fadeInMs < 0 || a.fadeInMs > 1000 ||
            !std::isfinite(a.fadeOutMs) || a.fadeOutMs < 0 || a.fadeOutMs > 1000) {
            error = "sample layer range, gain, tuning or fade is invalid"; return false;
        }
        for (std::size_t previous = 0; previous < index; ++previous) {
            const auto& b = instrument.samples[previous];
            if (a.lowNote > b.highNote || b.lowNote > a.highNote || a.lowVelocity > b.highVelocity || b.lowVelocity > a.highVelocity) continue;
            if (a.lowNote != b.lowNote || a.highNote != b.highNote || a.lowVelocity != b.lowVelocity || a.highVelocity != b.highVelocity || a.chokeGroup != b.chokeGroup) {
                error = "overlapping sample ranges must be identical round-robin groups (including chokeGroup)"; return false;
            }
        }
    }
    return true;
}

namespace {
// Windowed-sinc reconstruction with a guarded cutoff (Smith, resampling audio).
// Kernels are normalized for DC and tabulated during preparation for each pitch.
std::vector<float> resample(const runtime::WavData& source, std::size_t first, std::size_t last,
    double step, std::size_t frames, const SampleLayer& config, std::uint32_t rate) {
    constexpr int taps = 64, phases = 512;
    std::array<std::array<double, taps>, phases + 1> kernel{};
    const double cutoff = .45 / std::max(1.0, step);
    for (int phase = 0; phase <= phases; ++phase) {
        double sum = 0;
        for (int tap = 0; tap < taps; ++tap) {
            const double x = tap - (taps / 2 - 1) - static_cast<double>(phase) / phases;
            const double sinc = std::fabs(x) < 1e-12 ? 2 * cutoff : std::sin(2 * std::numbers::pi * cutoff * x) / (std::numbers::pi * x);
            const double window = .42 + .5 * std::cos(std::numbers::pi * x / (taps / 2)) + .08 * std::cos(2 * std::numbers::pi * x / (taps / 2));
            kernel[phase][tap] = std::fabs(x) <= taps / 2 ? sinc * window : 0;
            sum += kernel[phase][tap];
        }
        for (auto& value : kernel[phase]) value /= sum;
    }
    std::vector<float> result(frames * 2);
    const double fadeIn = config.fadeInMs * rate / 1000, fadeOut = config.fadeOutMs * rate / 1000;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const double position = frame * step;
        const auto base = static_cast<std::int64_t>(std::floor(position));
        const double fractional = (position - base) * phases;
        const int phase = static_cast<int>(fractional);
        const double fraction = fractional - phase;
        const double fade = std::min({1.0, fadeIn > 0 ? frame / fadeIn : 1.0, fadeOut > 0 ? (frames - 1 - frame) / fadeOut : 1.0});
        for (std::uint32_t channel = 0; channel < 2; ++channel) {
            double sample = 0;
            for (int tap = 0; tap < taps; ++tap) {
                const auto offset = base + tap - (taps / 2 - 1);
                if (offset < 0 || static_cast<std::uint64_t>(offset) >= last - first) continue;
                const auto input = source.interleaved[(first + offset) * source.channels + (source.channels == 1 ? 0 : channel)];
                sample += input * (kernel[phase][tap] * (1 - fraction) + kernel[phase + 1][tap] * fraction);
            }
            result[frame * 2 + channel] = static_cast<float>(sample * config.gain * fade);
        }
    }
    return result;
}
}

bool OneShotSampler::prepare(const Instrument& instrument, const SongDocument& song, const std::filesystem::path& base,
    std::uint32_t rate, std::string& error, std::uint64_t maxBytes) {
    if (rate < 8000 || rate > 192000 || !validateSampleLayers(instrument, song, error)) {
        if (error.empty()) error = "sampler rate must be 8000..192000 Hz";
        return false;
    }
    std::vector<Layer> prepared;
    prepared.reserve(instrument.samples.size());
    std::uint64_t bytes = sizeof(*this) + instrument.samples.size() * (sizeof(Layer) + sizeof(rotations_[0]));
    for (const auto& config : instrument.samples) {
        const auto resource = std::find_if(song.resources.begin(), song.resources.end(), [&](const auto& r) { return r.id == config.resourceId; });
        const auto file = resolveResourcePath(base.empty() ? song.baseDirectory : base, resource->path);
        if (resource->hash.empty() || hashFile(file) != resource->hash) { error = "sample-hash: missing or changed sample " + resource->id; return false; }
        std::error_code ec;
        const auto fileBytes = std::filesystem::file_size(file, ec);
        if (ec || fileBytes > maxBytes || bytes > maxBytes - fileBytes) { error = "sample-budget: input sample exceeds preparation memory budget"; return false; }
        runtime::WavData audio;
        {
            std::ifstream input(file, std::ios::binary);
            std::uint32_t dataBytes = 0;
            if (!runtime::readWavHeader(input, audio, dataBytes, error)) return false;
            const auto decodedBytes = static_cast<std::uint64_t>(dataBytes) * sizeof(float) / (audio.bitsPerSample / 8);
            if (decodedBytes > maxBytes || bytes > maxBytes - decodedBytes) { error = "sample-budget: decoded sample exceeds preparation memory budget"; return false; }
        }
        if (!runtime::readWav(file, audio, error)) return false;
        if (audio.channels != 1 && audio.channels != 2) { error = "sampler supports mono or stereo WAV"; return false; }
        if (std::any_of(audio.interleaved.begin(), audio.interleaved.end(), [](float value) { return !std::isfinite(value); })) { error = "sample-nonfinite: WAV contains NaN/Inf"; return false; }
        const auto total = audio.interleaved.size() / audio.channels;
        const auto firstSeconds = config.startSeconds * audio.sampleRate;
        const auto lastSeconds = config.endSeconds ? config.endSeconds * audio.sampleRate : static_cast<double>(total);
        if (firstSeconds >= total || lastSeconds > total || lastSeconds <= firstSeconds) { error = "sample-range: start/end lies outside the sample"; return false; }
        const auto first = static_cast<std::size_t>(firstSeconds), last = static_cast<std::size_t>(lastSeconds);
        if (first >= last) { error = "sample-range: empty frame interval"; return false; }
        Layer layer; layer.config = config;
        for (int note = config.lowNote; note <= config.highNote; ++note) {
            const double transpose = std::exp2((note - config.rootNote + config.tuneCents / 100) / 12);
            const double step = transpose * audio.sampleRate / rate;
            if (transpose < .25 || transpose > 4 || step < .125 || step > 8) { error = "sample-transpose: requested note/rate exceeds supported bandlimited range"; return false; }
            const auto frames = static_cast<std::uint64_t>(std::ceil((last - first) / step));
            const auto needed = frames * 2 * sizeof(float);
            if (needed > maxBytes || bytes > maxBytes - needed || audio.interleaved.size() * sizeof(float) > maxBytes - bytes - needed) {
                error = "sample-budget: prepared pitch bank exceeds memory budget"; return false;
            }
            layer.audio[note] = resample(audio, first, last, step, static_cast<std::size_t>(frames), config, rate);
            bytes += needed;
        }
        prepared.push_back(std::move(layer));
    }
    layers_ = std::move(prepared); rotations_.resize(layers_.size()); bytes_ = bytes;
    fadeFrames_ = std::max(1u, rate / 500); reset(); return true;
}

void OneShotSampler::reset() noexcept {
    voices_ = {}; age_ = stolen_ = 0;
    for (auto& rotation : rotations_) rotation.fill(0);
}

void OneShotSampler::apply(const runtime::MidiEvent& event) noexcept {
    if (event.channel > 15 || event.data1 > 127 || event.data2 > 127) return;
    if (event.type == runtime::MidiType::allSoundOff || event.type == runtime::MidiType::allNotesOff) {
        for (auto& voice : voices_) if (voice.channel == event.channel && voice.audio && !voice.stopLeft) voice.stopLeft = fadeFrames_;
        return;
    }
    if (event.type != runtime::MidiType::noteOn || !event.data2) return;
    std::size_t first = layers_.size(), count = 0;
    for (std::size_t index = 0; index < layers_.size(); ++index) {
        const auto& config = layers_[index].config;
        if (event.data1 < config.lowNote || event.data1 > config.highNote || event.data2 < config.lowVelocity || event.data2 > config.highVelocity) continue;
        if (first == layers_.size()) first = index;
        ++count;
    }
    if (!count) return;
    const auto selected = rotations_[first][event.channel]++ % count;
    std::size_t match = 0, chosen = first;
    for (std::size_t index = first; index < layers_.size(); ++index) {
        const auto& c = layers_[index].config;
        if (event.data1 < c.lowNote || event.data1 > c.highNote || event.data2 < c.lowVelocity || event.data2 > c.highVelocity) continue;
        if (match++ == selected) { chosen = index; break; }
    }
    const auto& layer = layers_[chosen];
    if (layer.config.chokeGroup) for (auto& voice : voices_)
        if (voice.audio && voice.choke == layer.config.chokeGroup && !voice.stopLeft) voice.stopLeft = fadeFrames_;
    auto* slot = &voices_[0];
    for (auto& voice : voices_) {
        if (!voice.audio) { slot = &voice; break; }
        if (voice.age < slot->age) slot = &voice;
    }
    if (slot->audio) ++stolen_;
    // Preserve the interrupted output as a short decaying residual, including an older steal fade.
    slot->residualLeft = slot->lastLeft; slot->residualRight = slot->lastRight;
    slot->fadeLeft = fadeFrames_; slot->stopLeft = 0;
    slot->audio = &layer.audio[event.data1]; slot->position = 0; slot->age = ++age_;
    slot->channel = event.channel; slot->choke = layer.config.chokeGroup; slot->gain = event.data2 / 127.f;
}

void OneShotSampler::process(float* left, float* right, std::uint32_t frames, std::span<const runtime::MidiEvent> events) noexcept {
    std::size_t cursor = 0;
    for (std::uint32_t frame = 0; frame < frames; ++frame) {
        while (cursor < events.size() && events[cursor].sampleOffset <= frame) apply(events[cursor++]);
        float l = 0, r = 0;
        for (auto& voice : voices_) {
            float vl = 0, vr = 0;
            if (voice.audio) {
                const float stop = voice.stopLeft ? static_cast<float>(voice.stopLeft) / fadeFrames_ : 1.f;
                vl = (*voice.audio)[voice.position++] * voice.gain * stop;
                vr = (*voice.audio)[voice.position++] * voice.gain * stop;
                if (voice.position >= voice.audio->size() || (voice.stopLeft && !--voice.stopLeft)) voice.audio = nullptr;
            }
            if (voice.fadeLeft) {
                const float weight = static_cast<float>(voice.fadeLeft--) / fadeFrames_;
                vl += voice.residualLeft * weight; vr += voice.residualRight * weight;
            }
            voice.lastLeft = vl; voice.lastRight = vr; l += vl; r += vr;
        }
        left[frame] = l; right[frame] = r;
    }
}
}
