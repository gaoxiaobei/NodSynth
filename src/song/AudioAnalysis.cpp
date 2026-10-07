#include <nodsynth/song/AudioAnalysis.h>

#include <cmath>
#include <algorithm>
#include <limits>
#include <vector>
#include <memory>
#include <array>
#include <ebur128.h>

#include <nodsynth/runtime/WavFile.h>

namespace nodsynth::song {
namespace {
double dbToAmplitude(double thresholdDb) { return std::pow(10.0, thresholdDb / 20.0); }

AudioAnalysis analyzeWav(const std::filesystem::path& path, const AnalyzeOptions& options, float silenceThreshold) {
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
    analysis.windowSeconds = std::max(0.0, options.windowMs) * 0.001;
    analysis.thresholdAmplitude = dbToAmplitude(options.thresholdDb);
    if (analysis.frames == 0) {
        analysis.leadingSilence = true;
        analysis.trailingSilence = true;
        analysis.fullySilent = true;
        analysis.samplePeakDb = -240.0;
        return analysis;
    }
    const float threshold = std::max(0.f, silenceThreshold);
    const auto windowFrames = std::max<std::uint64_t>(
        1, static_cast<std::uint64_t>(std::llround(std::max(0.0, options.windowMs) * 0.001 * wav.sampleRate)));
    const double minSilenceSeconds = std::max(0.0, options.minSilenceMs) * 0.001;
    bool heard = false;
    std::uint64_t leading = 0;
    std::uint64_t trailing = 0;
    double energy = 0.0;
    double leftEnergy = 0, rightEnergy = 0, crossEnergy = 0, sumLeft = 0, sumRight = 0;
    double windowEnergy = 0.0;
    std::uint64_t windowSamples = 0;
    std::uint64_t windowStart = 0;
    std::optional<std::uint64_t> firstActiveFrame;
    std::optional<std::uint64_t> lastActiveEnd;
    std::uint64_t activeFrames = 0;

    auto closeWindow = [&](std::uint64_t endFrame) {
        if (endFrame <= windowStart) return;
        const auto samples = windowSamples == 0 ? 1 : windowSamples;
        const double rms = std::sqrt(windowEnergy / static_cast<double>(samples));
        if (rms > analysis.thresholdAmplitude) {
            if (!firstActiveFrame) firstActiveFrame = windowStart;
            lastActiveEnd = endFrame;
            activeFrames += endFrame - windowStart;
        }
        windowEnergy = 0.0;
        windowSamples = 0;
        windowStart = endFrame;
    };

    for (std::uint64_t frame = 0; frame < analysis.frames; ++frame) {
        float peak = 0.f;
        const double left = wav.interleaved[static_cast<std::size_t>(frame) * wav.channels];
        const double right = wav.channels > 1 ? wav.interleaved[static_cast<std::size_t>(frame) * wav.channels + 1] : left;
        for (std::uint32_t channel = 0; channel < wav.channels; ++channel) {
            const float sample = wav.interleaved[static_cast<std::size_t>(frame) * wav.channels + channel];
            if (!std::isfinite(sample)) { analysis.ok = false; analysis.message = "audio contains NaN or infinity"; return analysis; }
            peak = std::max(peak, std::fabs(sample));
            energy += static_cast<double>(sample) * sample;
            windowEnergy += static_cast<double>(sample) * sample;
            ++windowSamples;
        }
        leftEnergy += left * left; rightEnergy += right * right; crossEnergy += left * right;
        sumLeft += left; sumRight += right;
        const double mid = (left + right) * .5, side = (left - right) * .5;
        analysis.midEnergy += mid * mid; analysis.sideEnergy += side * side;
        analysis.peak = std::max(analysis.peak, peak);
        if (peak <= threshold) {
            ++analysis.silentFrames;
            if (!heard) ++leading;
            ++trailing;
        } else {
            heard = true;
            trailing = 0;
        }
        if ((frame - windowStart + 1) >= windowFrames) closeWindow(frame + 1);
    }
    if (windowStart < analysis.frames) closeWindow(analysis.frames);

    analysis.rms = std::sqrt(energy / static_cast<double>(wav.interleaved.size()));
    const double count = static_cast<double>(analysis.frames);
    analysis.midEnergy /= count; analysis.sideEnergy /= count;
    analysis.monoRms = std::sqrt(analysis.midEnergy);
    analysis.dcLeft = sumLeft / count; analysis.dcRight = sumRight / count;
    const double covariance = crossEnergy - sumLeft * sumRight / count;
    const double varianceLeft = std::max(0.0, leftEnergy - sumLeft * sumLeft / count);
    const double varianceRight = std::max(0.0, rightEnergy - sumRight * sumRight / count);
    if (varianceLeft > 0 && varianceRight > 0)
        analysis.correlation = std::clamp(covariance / std::sqrt(varianceLeft * varianceRight), -1.0, 1.0);
    if (analysis.peak > 0.f) analysis.samplePeakDb = 20.0 * std::log10(static_cast<double>(analysis.peak));
    else analysis.samplePeakDb = -240.0;
    analysis.leadingSilence = leading == analysis.frames || leading > 0;
    analysis.trailingSilence = trailing > 0;
    analysis.activeRatio = static_cast<double>(activeFrames) / static_cast<double>(analysis.frames);
    analysis.fullySilent = !firstActiveFrame.has_value();
    if (analysis.fullySilent) {
        analysis.activityStartSeconds.reset();
        analysis.activityEndSeconds.reset();
        analysis.leadingSilenceSeconds = 0.0;
        analysis.trailingSilenceSeconds = 0.0;
    } else {
        analysis.activityStartSeconds = static_cast<double>(*firstActiveFrame) / wav.sampleRate;
        analysis.activityEndSeconds = static_cast<double>(*lastActiveEnd) / wav.sampleRate;
        const double leadingSeconds = *analysis.activityStartSeconds;
        const double trailingSeconds =
            static_cast<double>(analysis.frames - *lastActiveEnd) / wav.sampleRate;
        analysis.leadingSilenceSeconds = leadingSeconds >= minSilenceSeconds ? leadingSeconds : 0.0;
        analysis.trailingSilenceSeconds = trailingSeconds >= minSilenceSeconds ? trailingSeconds : 0.0;
    }

    if(wav.channels<=2 && wav.sampleRate>=8000 && wav.sampleRate<=192000) {
        struct MeterDelete {void operator()(ebur128_state* meter) const {ebur128_destroy(&meter);}};
        std::unique_ptr<ebur128_state,MeterDelete> meter(ebur128_init(wav.channels,wav.sampleRate,
            EBUR128_MODE_I|EBUR128_MODE_LRA|EBUR128_MODE_TRUE_PEAK|EBUR128_MODE_HISTOGRAM));
        if(!meter) {analysis.ok=false;analysis.message="failed to prepare BS.1770 meter";return analysis;}
        const auto hop=std::max<std::uint64_t>(1,wav.sampleRate/10);
        for(std::uint64_t origin=0;origin<analysis.frames;origin+=hop) {
            const auto count=std::min(hop,analysis.frames-origin);
            if(ebur128_add_frames_float(meter.get(),wav.interleaved.data()+origin*wav.channels,static_cast<std::size_t>(count))!=EBUR128_SUCCESS) {
                analysis.ok=false;analysis.message="BS.1770 measurement failed";return analysis;
            }
            if(origin+count>=wav.sampleRate*3ull) {
                double value=0;
                if(ebur128_loudness_shortterm(meter.get(),&value)==EBUR128_SUCCESS && std::isfinite(value))
                    analysis.shortTermMaxLufs=analysis.shortTermMaxLufs?std::max(*analysis.shortTermMaxLufs,value):value;
            }
        }
        double value=0;
        if(analysis.frames>=wav.sampleRate*.4 && ebur128_loudness_global(meter.get(),&value)==EBUR128_SUCCESS && std::isfinite(value)) analysis.loudnessLufs=value;
        if(analysis.frames>=wav.sampleRate*6ull && !analysis.fullySilent && ebur128_loudness_range(meter.get(),&value)==EBUR128_SUCCESS && std::isfinite(value)) analysis.loudnessRangeLu=value;
        double truePeak=0;
        for(std::uint32_t channel=0;channel<wav.channels;++channel) {
            if(ebur128_true_peak(meter.get(),channel,&value)==EBUR128_SUCCESS) truePeak=std::max(truePeak,value);
        }
        // Flush interpolation history only for true peak, after loudness windows have been measured.
        std::array<float,128> zeros{};
        if(ebur128_add_frames_float(meter.get(),zeros.data(),64)!=EBUR128_SUCCESS) {analysis.ok=false;analysis.message="true-peak flush failed";return analysis;}
        for(std::uint32_t channel=0;channel<wav.channels;++channel) if(ebur128_true_peak(meter.get(),channel,&value)==EBUR128_SUCCESS) truePeak=std::max(truePeak,value);
        if(truePeak>0) analysis.truePeakDbtp=20*std::log10(truePeak);
    }
    return analysis;
}
} // namespace

AudioAnalysis analyzeWav(const std::filesystem::path& path, float silenceThreshold) {
    return analyzeWav(path, AnalyzeOptions{}, silenceThreshold);
}

AudioAnalysis analyzeWav(const std::filesystem::path& path, const AnalyzeOptions& options) {
    return analyzeWav(path, options, 0.0001f);
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
    json.set("leadingSilenceSeconds", persist::Json::number(analysis.leadingSilenceSeconds));
    json.set("trailingSilenceSeconds", persist::Json::number(analysis.trailingSilenceSeconds));
    json.set(
        "activityStartSeconds",
        analysis.activityStartSeconds ? persist::Json::number(*analysis.activityStartSeconds) : persist::Json::null());
    json.set(
        "activityEndSeconds",
        analysis.activityEndSeconds ? persist::Json::number(*analysis.activityEndSeconds) : persist::Json::null());
    json.set("activeRatio", persist::Json::number(analysis.activeRatio));
    json.set("windowSeconds", persist::Json::number(analysis.windowSeconds));
    json.set("thresholdAmplitude", persist::Json::number(analysis.thresholdAmplitude));
    if (analysis.samplePeakDb) json.set("samplePeakDb", persist::Json::number(*analysis.samplePeakDb));
    json.set("rms", persist::Json::number(analysis.rms));
    json.set("fullySilent", persist::Json::boolean(analysis.fullySilent));
    json.set("correlation", analysis.correlation ? persist::Json::number(*analysis.correlation) : persist::Json::null());
    json.set("midEnergy", persist::Json::number(analysis.midEnergy));
    json.set("sideEnergy", persist::Json::number(analysis.sideEnergy));
    json.set("monoRms", persist::Json::number(analysis.monoRms));
    json.set("dcLeft", persist::Json::number(analysis.dcLeft));
    json.set("dcRight", persist::Json::number(analysis.dcRight));
    if (analysis.loudnessLufs) json.set("loudnessLufs", persist::Json::number(*analysis.loudnessLufs));
    json.set("truePeakDbtp",analysis.truePeakDbtp?persist::Json::number(*analysis.truePeakDbtp):persist::Json::null());
    json.set("shortTermMaxLufs",analysis.shortTermMaxLufs?persist::Json::number(*analysis.shortTermMaxLufs):persist::Json::null());
    json.set("loudnessRangeLu",analysis.loudnessRangeLu?persist::Json::number(*analysis.loudnessRangeLu):persist::Json::null());
    json.set("meterVersion",persist::Json::string(analysis.meterVersion));
    json.set("meterPolicy",persist::Json::string("BS.1770-4/EBU R128 mono/stereo; integrated needs 400ms, short-term 3s, reported LRA needs 6s; silence/unsupported layout or rate yields null; true peak flushes interpolation history; histogram gating precision 0.1 LU"));
    return json;
}
} // namespace nodsynth::song
