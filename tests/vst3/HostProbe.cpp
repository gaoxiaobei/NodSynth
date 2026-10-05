#include <nodsynth/host/Vst3Host.h>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {
bool fail(const std::string& message)
{
    std::cerr << message << '\n';
    return false;
}

float peak(const float* left, const float* right, std::uint32_t frames)
{
    float value = 0.f;
    for (std::uint32_t index = 0; index < frames; ++index) {
        value = std::max(value, std::max(std::fabs(left[index]), std::fabs(right[index])));
    }
    return value;
}
} // namespace

int crashCheck(const char* self)
{
    std::string command = std::string("\"") + self + "\" --crash";
    std::vector<char> writable(command.begin(), command.end());
    writable.push_back('\0');
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (CreateProcessA(nullptr, writable.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process) == 0) {
        return fail("the crash worker did not start") ? 1 : 1;
    }
    WaitForSingleObject(process.hProcess, 15000);
    DWORD code = 0;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (code == 0 || code == STILL_ACTIVE) return fail("a crashed plugin worker was reported as success") ? 1 : 1;
    std::cout << "plugin worker crash code " << code << '\n';
    return 0;
}

int main(int argc, char** argv)
{
    if (argc > 1 && std::string(argv[1]) == "--crash") std::abort();
    if (argc > 1 && std::string(argv[1]) == "--crash-check") return crashCheck(argv[0]);

    const std::vector<float> impulse{1.f, 0.f, 0.f, 0.f};
    const auto delayed = nodsynth::host::delayForLatency(impulse, 2, 2);
    if (delayed.size() != 8 || delayed[0] != 0.f || delayed[4] != 1.f) {
        return fail("latency compensation did not delay the impulse") ? 1 : 1;
    }

    const char* path = argc > 1 ? argv[1] : NOD_VST3_PATH;
    const std::string className = argc > 2 ? argv[2] : "";
    nodsynth::host::Vst3Plugin plugin;
    std::string error;
    if (!plugin.open(path, error, className)) return fail(error.empty() ? "open failed" : error) ? 1 : 1;
    std::cout << plugin.info().name << " " << plugin.info().subCategories << " latency=" << plugin.info().latencySamples << '\n';
    if (!plugin.activate(48000.0, 128, error)) return fail(error.empty() ? "activate failed" : error) ? 1 : 1;
    std::cout << "reported latency " << plugin.info().latencySamples << '\n';
    constexpr std::uint32_t kFrames = 128;
    const bool delayEffect = plugin.info().name.find("Delay") != std::string::npos;
    if (delayEffect) {
        constexpr std::uint32_t kTotal = 48000;
        std::vector<float> left(kTotal, 0.f);
        std::vector<float> right(kTotal, 0.f);
        std::vector<float> input(kFrames, 0.f);
        input[0] = 1.f;
        std::fill(left.begin(), left.begin() + kFrames, 0.25f);
        for (std::uint32_t rendered = 0; rendered < kTotal; rendered += kFrames) {
            const float* in = rendered == 0 ? input.data() : nullptr;
            if (!plugin.process(left.data() + rendered, right.data() + rendered, kFrames, {}, error, in, in)) return fail(error) ? 1 : 1;
        }
        std::uint32_t wet = 0;
        float wetPeak = 0.f;
        for (std::uint32_t index = 64; index < kTotal; ++index) {
            const float sample = std::max(std::fabs(left[index]), std::fabs(right[index]));
            if (sample > wetPeak) {
                wetPeak = sample;
                wet = index;
            }
        }
        std::cout << "first " << left[0] << " dry " << left[0] << " wet " << wetPeak << " at " << wet << '\n';
        if (!(wetPeak > 0.01f) || wet < 128) return fail("the delay effect did not repeat the impulse") ? 1 : 1;
        const auto aligned = nodsynth::host::delayForLatency({1.f}, 1, static_cast<std::int32_t>(wet));
        if (aligned.size() <= wet || aligned[wet] != 1.f) return fail("delay compensation missed the effect peak") ? 1 : 1;
        if (plugin.timelineSamples() != kTotal) return fail("the host timeline did not advance with the render") ? 1 : 1;
        std::vector<char> state;
        if (!plugin.saveState(state, error) || state.empty()) return fail(error.empty() ? "saveState failed" : error) ? 1 : 1;
        if (!plugin.restoreState(state, error)) return fail(error.empty() ? "restoreState failed" : error) ? 1 : 1;
        std::fill(left.begin(), left.begin() + kFrames, 0.f);
        input[0] = 1.f;
        if (!plugin.process(left.data(), right.data(), kFrames, {}, error, input.data(), input.data())) {
            return fail(error.empty() ? "process after restore failed" : error) ? 1 : 1;
        }
        if (std::fabs(left[0]) < 0.2f) return fail("restored delay state no longer passes audio") ? 1 : 1;
        std::cout << "delay peak at " << wet << " compensated\n";
        return 0;
    }
    std::vector<char> state;
    if (!plugin.saveState(state, error) || state.empty()) return fail(error.empty() ? "saveState failed" : error) ? 1 : 1;
    if (!plugin.restoreState(state, error)) return fail(error.empty() ? "restoreState failed" : error) ? 1 : 1;
    std::vector<float> left(kFrames, 0.f);
    std::vector<float> right(kFrames, 0.f);
    std::vector<nodsynth::host::Vst3Midi> notes{{0, true, 0, 69, 100}};
    float heard = 0.f;
    for (int block = 0; block < 12; ++block) {
        if (!plugin.process(left.data(), right.data(), kFrames, notes, error)) return fail(error) ? 1 : 1;
        heard = std::max(heard, peak(left.data(), right.data(), kFrames));
        notes.clear();
    }
    if (!(heard > 0.01f)) return fail("the hosted plugin stayed silent") ? 1 : 1;
    if (plugin.timelineSamples() != static_cast<std::int64_t>(kFrames) * 12) return fail("the host timeline did not advance") ? 1 : 1;
    std::cout << "hosted peak " << heard << '\n';
    return 0;
}
