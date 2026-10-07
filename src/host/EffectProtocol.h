#pragma once
#include <nodsynth/effects/Effect.h>
#include <cstdint>

namespace nodsynth::host::protocol {
inline constexpr std::uint32_t version=2,maxFrames=8192,maxParameters=4096,maxEvents=maxFrames*16+maxParameters;
enum Command : std::uint32_t {prepare,process,reset,stop};
struct Parameter {std::uint32_t id{0};bool automatable{false};double value{0};char title[128]{};};
struct Shared {
    std::uint32_t protocolVersion{version},parentPid{0},command{prepare},frames{0},eventCount{0},parameterCount{0},latency{0};
    double rate{48000},tailSeconds{0};
    std::uint32_t capacity{128};
    bool ok{false};
    bool inspectionOnly{false};
    std::uint32_t classCount{0};
    char classes[128][256]{},pluginName[256]{},vendor[256]{},pluginVersion[256]{},subCategories[256]{};
    char plugin[4096]{},state[4096]{},className[256]{},error[1024]{};
    Parameter parameters[maxParameters]{};
    effects::ParameterEvent events[maxEvents]{};
    float inputLeft[maxFrames]{},inputRight[maxFrames]{},outputLeft[maxFrames]{},outputRight[maxFrames]{};
    double bpm[maxFrames]{};
};
}
