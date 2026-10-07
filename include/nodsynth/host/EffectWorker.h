#pragma once
#include <nodsynth/effects/Effect.h>
#include <nodsynth/host/Vst3Host.h>
#include <filesystem>

namespace nodsynth::host {
struct EffectWorkerOptions {
    std::filesystem::path executable,plugin,state;
    std::string className;
    std::uint32_t timeoutMs{30000};
    double declaredTailSeconds{-1};
    bool inspectionOnly{false};
};
struct PluginInspection {
    Vst3Info info;
    std::vector<std::string> classes;
    std::vector<effects::Parameter> parameters;
    double tailSeconds{0};
    std::uint64_t workerPrivateBytes{0};
};
[[nodiscard]] bool inspectPlugin(EffectWorkerOptions options,PluginInspection& result,std::string& error);
[[nodiscard]] std::unique_ptr<effects::Effect> makeWorkerEffect(EffectWorkerOptions options);
int runEffectWorker(const std::string& mappingName);
}
