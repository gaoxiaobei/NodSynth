#pragma once
#include <nodsynth/runtime/DspNode.h>
#include <string>
namespace nodsynth::nodes {
[[nodiscard]] std::unique_ptr<runtime::DspNode> makeGlobalEffect(std::string type);
[[nodiscard]] std::uint64_t globalEffectStateBytes(const std::string& type,double rate,std::uint32_t maxFrames) noexcept;
}
