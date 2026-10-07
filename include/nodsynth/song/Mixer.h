#pragma once
#include <nodsynth/song/Automation.h>

namespace nodsynth::song {
[[nodiscard]] persist::Json effectsJson(const std::vector<effects::Config>& configs);
[[nodiscard]] bool parseEffects(const persist::Json& json,std::vector<effects::Config>& configs,std::string& error);
[[nodiscard]] persist::Json sendsJson(const std::vector<Send>& sends);
[[nodiscard]] bool parseSends(const persist::Json& json,std::vector<Send>& sends,std::string& error);
[[nodiscard]] persist::Json routingJson(const SongDocument& song);
[[nodiscard]] bool parseRouting(const persist::Json& json,SongDocument& song,std::string& error);
[[nodiscard]] bool validateRouting(const SongDocument& song,std::string& error);
[[nodiscard]] bool hasRouting(const SongDocument& song) noexcept;

class SongMixer {
public:
    [[nodiscard]] bool prepare(const SongDocument& song,std::uint32_t rate,std::uint32_t maxFrames,std::string& error,const std::filesystem::path& baseDirectory={});
    void reset() noexcept;
    void setTrackInput(std::size_t index,const float* left,const float* right,std::uint32_t frames) noexcept;
    void process(std::int64_t origin,std::uint32_t frames,float* interleavedMaster) noexcept;
    [[nodiscard]] const float* trackOutput(std::size_t index) const noexcept;
    [[nodiscard]] const float* busOutput(std::size_t index) const noexcept;
    [[nodiscard]] std::uint32_t latencySamples() const noexcept { return latency_; }
    [[nodiscard]] std::uint32_t trackLatency(std::size_t index) const noexcept {return index<tracks_?nodes_[index].outputLatency:0;}
    [[nodiscard]] std::uint32_t busLatency(std::size_t index) const noexcept {return index<buses_?nodes_[tracks_+index].outputLatency:0;}
    [[nodiscard]] double tailSeconds() const noexcept { return tail_; }
    [[nodiscard]] std::uint64_t committedBytes() const noexcept { return bytes_; }
    [[nodiscard]] const char* errorReason() const noexcept;
    [[nodiscard]] persist::Json effectReport(const SongDocument& song,bool frozen) const;
private:
    struct Edge {
        std::size_t source{0},target{0};
        double gain{1}; bool preFader{false};
        std::vector<float> compensation;
        std::size_t position{0};
    };
    struct Node {
        std::vector<float> left,right,post;
        std::vector<float> inputCompensation;
        std::size_t inputPosition{0};
        struct Insert {
            std::unique_ptr<effects::Effect> effect;
            std::vector<PreparedLane> lanes;
            std::vector<effects::ParameterEvent> events;
            std::optional<std::size_t> detectorSource;
            std::vector<float> detectorLeft,detectorRight,detectorCompensation;
            std::size_t detectorPosition{0};
        };
        std::vector<Insert> inserts;
        std::optional<PreparedTrackMix> trackMix;
        double gain{1},pan{0}; bool mute{false};
        std::uint32_t outputLatency{0};
        std::vector<std::size_t> edges;
    };
    std::vector<Node> nodes_;
    std::vector<Edge> edges_;
    std::vector<std::size_t> order_;
    std::vector<double> bpm_;
    std::vector<std::pair<std::int64_t,double>> tempo_;
    std::size_t tracks_{0},buses_{0},master_{0};
    std::uint32_t capacity_{0},latency_{0};
    std::uint64_t bytes_{0};
    double tail_{0};
};
}
