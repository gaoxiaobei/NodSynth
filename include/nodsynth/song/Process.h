#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nodsynth::song {
struct ProcessRequest {
    std::vector<std::string> arguments;
    std::uint32_t timeoutMs{30000};
};

struct ProcessResult {
    bool started{false};
    bool timedOut{false};
    int exitCode{-1};
    std::string message;
};

[[nodiscard]] ProcessResult runProcess(const ProcessRequest& request);
} // namespace nodsynth::song
