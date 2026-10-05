#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace nodsynth::host {
struct Vst3Info {
    std::string name;
    std::string vendor;
    std::string version;
    std::string subCategories;
    std::int32_t latencySamples{0};
};

struct Vst3Midi {
    std::uint32_t sampleOffset{0};
    bool noteOn{false};
    std::uint8_t channel{0};
    std::uint8_t pitch{0};
    std::uint8_t velocity{0};
};

struct Vst3Parameter {
    std::uint32_t id{0};
    std::string title;
    bool canAutomate{false};
};

struct Vst3ParamPoint {
    std::uint32_t sampleOffset{0};
    std::uint32_t id{0};
    double value{0};
};

class Vst3Plugin {
public:
    Vst3Plugin();
    ~Vst3Plugin();
    Vst3Plugin(const Vst3Plugin&) = delete;
    Vst3Plugin& operator=(const Vst3Plugin&) = delete;

    [[nodiscard]] bool open(const std::filesystem::path& path, std::string& error, const std::string& className = {});
    [[nodiscard]] bool activate(double sampleRate, std::uint32_t blockSize, std::string& error);
    [[nodiscard]] const Vst3Info& info() const noexcept { return info_; }
    [[nodiscard]] std::vector<Vst3Parameter> parameters() const;
    [[nodiscard]] std::int64_t timelineSamples() const noexcept { return timelineSamples_; }
    [[nodiscard]] bool process(
        float* left,
        float* right,
        std::uint32_t frames,
        const std::vector<Vst3Midi>& midi,
        std::string& error,
        const float* inputLeft = nullptr,
        const float* inputRight = nullptr,
        const std::vector<Vst3ParamPoint>* parameters = nullptr);
    [[nodiscard]] bool saveState(std::vector<char>& bytes, std::string& error);
    [[nodiscard]] bool restoreState(const std::vector<char>& bytes, std::string& error);
    void close() noexcept;

private:
    struct Instance;
    Instance* instance_{nullptr};
    Vst3Info info_{};
    std::int64_t timelineSamples_{0};
};

// Delay every sample by `latency` frames. The returned buffer is `latency` samples longer
// and its musical onset matches a plugin that reports that latency.
[[nodiscard]] std::vector<float> delayForLatency(const std::vector<float>& interleaved, std::uint32_t channels, std::int32_t latency);
} // namespace nodsynth::host
