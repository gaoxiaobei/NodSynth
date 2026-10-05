#include <nodsynth/song/AudioAnalysis.h>

#include <cmath>
#include <limits>
#include <vector>

#include <nodsynth/runtime/WavFile.h>

namespace nodsynth::song {
namespace {
struct Biquad {
    double b0{1};
    double b1{0};
    double b2{0};
    double a1{0};
    double a2{0};
    double z1{0};
    double z2{0};

    float process(float input) {
        const double output = b0 * input + z1;
        z1 = b1 * input - a1 * output + z2;
        z2 = b2 * input - a2 * output;
        return static_cast<float>(output);
    }
};

struct KWeight {
    Biquad shelf;
    Biquad highpass;
};

KWeight weighting(std::uint32_t sampleRate) {
    KWeight filter;
    if (sampleRate == 48000) {
        filter.shelf = {1.53512485958697, -2.69169618940638, 1.19839281085285, -1.69065929318241, 0.73248077421585, 0, 0};
        filter.highpass = {1.0, -2.0, 1.0, -1.99004745483398, 0.99007225036621, 0, 0};
    } else if (sampleRate == 44100) {
        filter.shelf = {1.5308412300503478, -2.650979995154729, 1.1690790799215869, -1.6636551132560202, 0.7125954280732254, 0, 0};
        filter.highpass = {1.0, -2.0, 1.0, -1.9891696736297957, 0.9891990357870394, 0, 0};
    }
    return filter;
}

double lufs(double meanSquare) {
    if (!(meanSquare > 0.0)) return -std::numeric_limits<double>::infinity();
    return -0.691 + 10.0 * std::log10(meanSquare);
}
} // namespace

AudioAnalysis analyzeWav(const std::filesystem::path& path, float silenceThreshold) {
    AudioAnalysis analysis;
    runtime::WavData wav;
    std::string error;
    if (!runtime::readWav(path, wav, error) || wav.channels == 0) {
        analysis.message = error.empty() ? "failed to read WAV" : error;
        return analysis;
    }
    analysis.ok = true;
    analysis.sampleRate = wav.sampleRate;
    analysis.channels = wav.channels;
    analysis.frames = wav.interleaved.size() / wav.channels;
    analysis.message = "analyzed";
    if (analysis.frames == 0) {
        analysis.leadingSilence = true;
        analysis.trailingSilence = true;
        return analysis;
    }
    const float threshold = std::max(0.f, silenceThreshold);
    bool heard = false;
    std::uint64_t leading = 0;
    std::uint64_t trailing = 0;
    for (std::uint64_t frame = 0; frame < analysis.frames; ++frame) {
        float peak = 0.f;
        for (std::uint32_t channel = 0; channel < wav.channels; ++channel) {
            peak = std::max(peak, std::fabs(wav.interleaved[static_cast<std::size_t>(frame) * wav.channels + channel]));
        }
        analysis.peak = std::max(analysis.peak, peak);
        if (peak <= threshold) {
            ++analysis.silentFrames;
            if (!heard) ++leading;
            ++trailing;
        } else {
            heard = true;
            trailing = 0;
        }
    }
    analysis.leadingSilence = leading == analysis.frames || leading > 0;
    analysis.trailingSilence = trailing > 0;

    if ((wav.sampleRate == 48000 || wav.sampleRate == 44100) && wav.channels >= 1) {
        std::vector<float> weighted(static_cast<std::size_t>(analysis.frames) * 2, 0.f);
        for (std::uint32_t channel = 0; channel < std::min<std::uint32_t>(wav.channels, 2); ++channel) {
            auto filter = weighting(wav.sampleRate);
            for (std::uint64_t frame = 0; frame < analysis.frames; ++frame) {
                const float input = wav.interleaved[static_cast<std::size_t>(frame) * wav.channels + channel];
                weighted[static_cast<std::size_t>(frame) * 2 + channel] = filter.highpass.process(filter.shelf.process(input));
            }
        }
        const auto block = std::max<std::uint64_t>(1, static_cast<std::uint64_t>(std::llround(0.4 * wav.sampleRate)));
        const auto hop = std::max<std::uint64_t>(1, block / 4);
        std::vector<double> powers;
        for (std::uint64_t start = 0; start < analysis.frames;) {
            const auto count = std::min(block, analysis.frames - start);
            if (analysis.frames >= block && count < block) break;
            double energy = 0.0;
            for (std::uint64_t frame = 0; frame < count; ++frame) {
                const float left = weighted[static_cast<std::size_t>(start + frame) * 2];
                const float right = wav.channels > 1 ? weighted[static_cast<std::size_t>(start + frame) * 2 + 1] : left;
                energy += static_cast<double>(left) * left + static_cast<double>(right) * right;
            }
            powers.push_back(energy / static_cast<double>(count));
            if (start + hop >= analysis.frames) break;
            start += hop;
            if (analysis.frames < block) break;
        }
        std::vector<double> absolute;
        for (const double power : powers) {
            if (lufs(power) >= -70.0) absolute.push_back(power);
        }
        if (!absolute.empty()) {
            double mean = 0.0;
            for (const double power : absolute) mean += power;
            mean /= static_cast<double>(absolute.size());
            const double relativeGate = lufs(mean) - 10.0;
            double gated = 0.0;
            int kept = 0;
            for (const double power : absolute) {
                if (lufs(power) >= relativeGate) {
                    gated += power;
                    ++kept;
                }
            }
            if (kept > 0) analysis.loudnessLufs = lufs(gated / kept);
        }
    }
    return analysis;
}

persist::Json analysisJson(const AudioAnalysis& analysis) {
    persist::Json json = persist::Json::object();
    json.set("status", persist::Json::string(analysis.ok ? "ok" : "rejected"));
    json.set("message", persist::Json::string(analysis.message));
    json.set("sampleRate", persist::Json::number(analysis.sampleRate));
    json.set("channels", persist::Json::number(analysis.channels));
    json.set("frames", persist::Json::number(static_cast<double>(analysis.frames)));
    json.set("peak", persist::Json::number(analysis.peak));
    json.set("silentFrames", persist::Json::number(static_cast<double>(analysis.silentFrames)));
    json.set("leadingSilence", persist::Json::boolean(analysis.leadingSilence));
    json.set("trailingSilence", persist::Json::boolean(analysis.trailingSilence));
    if (analysis.loudnessLufs) json.set("loudnessLufs", persist::Json::number(*analysis.loudnessLufs));
    return json;
}
} // namespace nodsynth::song
