#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace nodsynth::effects {
struct Parameter {
    std::string id, unit;
    double minimum{0}, maximum{1}, defaultValue{0};
    bool automatable{true};
    std::string title;
};
struct Schema {
    std::string type;
    std::uint32_t version{1};
    std::vector<Parameter> parameters;
};
struct Config {
    std::string id, type;
    std::uint32_t version{1};
    std::map<std::string, double> parameters;
    bool bypass{false};
    std::string sidechain;
    std::string quality{"standard"};
    std::string pluginResource,stateResource,className;
    std::uint32_t workerTimeoutMs{30000};
    double declaredTailSeconds{-1};
    struct Lane {
        std::string parameter,interpolation{"linear"};
        struct Point {std::uint32_t tick{0};double value{0};};
        std::vector<Point> points;
    };
    std::vector<Lane> automation;
};
struct ParameterEvent {
    std::uint32_t sampleOffset{0}, parameter{0};
    double value{0};
};
struct Block {
    float* left{nullptr};
    float* right{nullptr};
    std::uint32_t frames{0};
    const float* sidechainLeft{nullptr};
    const float* sidechainRight{nullptr};
    const double* bpm{nullptr}; // Optional sample-accurate tempo, default 120.
    std::span<const ParameterEvent> parameters;
};

// Stereo, in-place processing. Prepare owns all allocation; latency is reported in samples.
// Musical predelay/echo time is tail, not compensation latency. Bypass retains latency and state.
class Effect {
public:
    virtual ~Effect() = default;
    virtual bool prepare(double sampleRate, std::uint32_t maxFrames, const Config& config, std::string& error) = 0;
    virtual void reset() noexcept = 0;
    virtual void process(const Block& block) noexcept = 0;
    virtual void setBypass(bool bypass) noexcept = 0;
    [[nodiscard]] virtual std::uint32_t latencySamples() const noexcept = 0;
    [[nodiscard]] virtual double tailSeconds() const noexcept = 0;
    [[nodiscard]] virtual std::uint64_t committedBytes() const noexcept = 0;
    [[nodiscard]] virtual const Schema* parameterSchema() const noexcept {return nullptr;}
    [[nodiscard]] virtual const char* errorReason() const noexcept {return nullptr;}
    [[nodiscard]] virtual std::uint64_t workerMemoryBytes() const noexcept {return 0;}
};
[[nodiscard]] std::vector<Schema> schemas();
[[nodiscard]] const Schema* findSchema(const std::string& type);
[[nodiscard]] bool validateConfig(const Config& config, std::string& error);
[[nodiscard]] std::unique_ptr<Effect> makeEffect(const std::string& type);
[[nodiscard]] std::uint64_t stateBudget(const std::string& type,double sampleRate) noexcept;
}
