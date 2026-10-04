#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <nodsynth/model/Identifiers.h>

namespace nodsynth::runtime {
struct BufferView {
    model::PortId port{};
    float* data{nullptr};
    std::uint32_t channels{0};
    std::uint32_t capacity{0};
    std::uint32_t voiceStride{0};
    std::uint32_t channelStride{0};

    [[nodiscard]] float* at(std::uint32_t voice, std::uint32_t channel, std::uint32_t frame) const noexcept {
        return data + static_cast<std::size_t>(voice) * voiceStride +
               static_cast<std::size_t>(channel) * channelStride + frame;
    }
};

struct ParamView {
    const float* data{nullptr};
    std::uint32_t count{0};
    std::uint32_t capacity{0};

    [[nodiscard]] float at(std::uint32_t index, std::uint32_t frame) const noexcept {
        return data[static_cast<std::size_t>(index) * capacity + frame];
    }
};

struct NodeBinding {
    double sampleRate{};
    std::uint32_t maxFrames{};
    std::uint32_t voiceCount{};
    std::vector<BufferView> inputs;
    std::vector<BufferView> outputs;
    ParamView parameters{};
    const float* const* note{nullptr};
    const float* const* gate{nullptr};
    const float* const* velocity{nullptr};
    const float* const* triggers{nullptr};
    const float* const* attenuation{nullptr};
};

class DspNode {
public:
    virtual ~DspNode() = default;
    virtual void bind(const NodeBinding& binding) = 0;
    virtual void reset() = 0;
    virtual void process(std::uint32_t voice, std::uint32_t frames) = 0;
    virtual void commit(std::uint32_t /*voice*/, std::uint32_t /*frames*/) {}
    [[nodiscard]] virtual bool needsCommit() const noexcept { return false; }
    virtual void setMix(float* /*left*/, float* /*right*/) {}
};

class ImplementationRegistry {
public:
    virtual ~ImplementationRegistry() = default;
    [[nodiscard]] virtual bool contains(const model::NodeTypeId& typeId) const = 0;
    [[nodiscard]] virtual std::uint64_t stateBytes(
        const model::NodeTypeId& typeId,
        double sampleRate,
        std::uint32_t maxFrames,
        std::uint32_t voices) const = 0;
    [[nodiscard]] virtual std::unique_ptr<DspNode> instantiate(const model::NodeTypeId& typeId) const = 0;
};

[[nodiscard]] inline std::uint32_t delayLineCapacity(double sampleRate, std::uint32_t maxFrames) noexcept {
    const auto seconds = static_cast<std::uint64_t>(sampleRate * 2.0);
    const auto total = seconds + static_cast<std::uint64_t>(maxFrames) + 8ull;
    return total > 0xffffffffull ? 0xffffffffu : static_cast<std::uint32_t>(total);
}
} // namespace nodsynth::runtime
