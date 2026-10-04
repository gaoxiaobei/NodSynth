#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nodsynth/compiler/Diagnostic.h>
#include <nodsynth/model/GraphEditor.h>
#include <nodsynth/model/SchemaRegistry.h>
#include <nodsynth/runtime/DspNode.h>
#include <nodsynth/runtime/Engine.h>

namespace nodsynth::runtime {
class GraphSession {
public:
    GraphSession(EngineConfig config, model::SchemaRegistry registry, const ImplementationRegistry& implementations);
    ~GraphSession();
    GraphSession(const GraphSession&) = delete;
    GraphSession& operator=(const GraphSession&) = delete;

    [[nodiscard]] model::GraphEditor& editor() noexcept { return editor_; }
    [[nodiscard]] const model::GraphEditor& editor() const noexcept { return editor_; }
    [[nodiscard]] Engine& engine() noexcept { return engine_; }
    [[nodiscard]] const std::vector<compiler::Diagnostic>& diagnostics() const noexcept { return diagnostics_; }
    [[nodiscard]] const std::string& prepareMessage() const noexcept { return prepareMessage_; }
    [[nodiscard]] std::uint64_t soundingRevision() const noexcept { return soundingRevision_; }
    [[nodiscard]] bool sounding() const noexcept { return sounding_; }

    void compileSync();
    void requestCompile();
    void waitIdle();
    void poll();
    bool undo();
    bool redo();
    bool setParameter(const model::NodeId& node, const model::ParameterId& parameter, double value);
    [[nodiscard]] compiler::CompileResult preview(const model::Connection& connection) const;

    void setTestHook(std::function<void()> hook);

private:
    struct Job {
        std::uint64_t revision{0};
        model::GraphSnapshot snapshot;
        model::SchemaRegistry registry;
        PrepareConfig config{};
        double smoothSeconds{0.01};
    };

    struct JobResult {
        std::uint64_t revision{0};
        PlanResult prepared;
        compiler::CompileResult compiled;
        double releaseHold{0.5};
    };

    Job capture() const;
    JobResult runJob(const Job& job) const;
    void install(JobResult result);
    void syncParameters();
    void threadMain();

    Engine engine_;
    model::GraphEditor editor_;
    model::SchemaRegistry registry_;
    const ImplementationRegistry& implementations_;
    std::vector<compiler::Diagnostic> diagnostics_;
    std::string prepareMessage_;
    std::uint64_t soundingRevision_{0};
    bool sounding_{false};
    std::function<void()> testHook_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_{false};
    bool dirty_{false};
    bool running_{false};
    std::optional<Job> queued_;
    std::optional<JobResult> pending_;
    std::thread thread_;
};
} // namespace nodsynth::runtime
