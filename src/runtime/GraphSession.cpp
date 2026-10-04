#include <nodsynth/runtime/GraphSession.h>

#include <algorithm>
#include <cmath>
#include <utility>

#include <nodsynth/compiler/GraphCompiler.h>

namespace nodsynth::runtime {
namespace {
double releaseHoldFor(const model::GraphSnapshot& graph, const model::SchemaRegistry& registry) {
    double hold = 0.5;
    for (const auto& node : graph.nodes) {
        if (node.typeId.value != "nod.adsr") continue;
        double value = 0.2;
        if (const auto* schema = registry.find(node.typeId)) {
            for (const auto& parameter : schema->parameters) {
                if (parameter.id.value == "release") value = parameter.defaultValue;
            }
        }
        const auto stored = node.parameters.find(model::ParameterId{"release"});
        if (stored != node.parameters.end()) value = stored->second;
        hold = std::max(hold, value);
    }
    return hold + 0.05;
}
} // namespace

GraphSession::GraphSession(
    EngineConfig config, model::SchemaRegistry registry, const ImplementationRegistry& implementations)
    : engine_(std::move(config)), registry_(std::move(registry)), implementations_(implementations) {
    thread_ = std::thread([this] { threadMain(); });
}

GraphSession::~GraphSession() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

GraphSession::Job GraphSession::capture() const {
    return {
        editor_.structureRevision(),
        editor_.document().snapshot(),
        registry_,
        engine_.config(),
        engine_.parameterSmoothSeconds(),
    };
}

GraphSession::JobResult GraphSession::runJob(const Job& job) const {
    JobResult result;
    result.revision = job.revision;
    result.compiled = compiler::GraphCompiler{compiler::CompilerLimits{.maxVoices = job.config.voiceCount}}.compile(
        job.snapshot, job.registry);
    result.releaseHold = releaseHoldFor(job.snapshot, job.registry);
    if (!result.compiled.graph) return result;
    result.prepared = preparePlan(
        *result.compiled.graph, job.registry, implementations_, job.config, engine_.voices(), job.smoothSeconds);
    return result;
}

void GraphSession::install(JobResult result) {
    if (result.revision != editor_.structureRevision()) return;
    diagnostics_ = std::move(result.compiled.diagnostics);
    if (!result.compiled.graph) {
        prepareMessage_ = "graph was rejected";
        return;
    }
    if (result.prepared.error != PrepareError::none || result.prepared.plan == nullptr) {
        prepareMessage_ = result.prepared.message.empty() ? prepareErrorText(result.prepared.error) : result.prepared.message;
        return;
    }
    engine_.setReleaseHold(result.releaseHold);
    const auto staged = engine_.stage(std::move(result.prepared.plan));
    if (!staged.accepted) {
        prepareMessage_ = staged.reason == nullptr ? "plan was rejected" : staged.reason;
        return;
    }
    sounding_ = true;
    soundingRevision_ = result.revision;
    prepareMessage_.clear();
}

void GraphSession::compileSync() {
    const Job job = capture();
    install(runJob(job));
}

void GraphSession::requestCompile() {
    {
        std::lock_guard lock(mutex_);
        queued_ = capture();
        dirty_ = true;
    }
    cv_.notify_all();
}

void GraphSession::waitIdle() {
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [&] { return !dirty_ && !running_; });
}

void GraphSession::poll() {
    std::optional<JobResult> ready;
    {
        std::lock_guard lock(mutex_);
        ready = std::move(pending_);
        pending_.reset();
    }
    if (ready) install(std::move(*ready));
}

bool GraphSession::undo() {
    if (!editor_.undo()) return false;
    if (editor_.recentChange() == model::ChangeKind::structure) requestCompile();
    else if (editor_.recentChange() == model::ChangeKind::parameter) syncParameters();
    return true;
}

bool GraphSession::redo() {
    if (!editor_.redo()) return false;
    if (editor_.recentChange() == model::ChangeKind::structure) requestCompile();
    else if (editor_.recentChange() == model::ChangeKind::parameter) syncParameters();
    return true;
}

bool GraphSession::setParameter(const model::NodeId& node, const model::ParameterId& parameter, double value) {
    const auto* record = editor_.document().findNode(node);
    if (record == nullptr || !std::isfinite(value)) return false;
    const auto* schema = registry_.find(record->typeId);
    if (schema == nullptr) return false;
    const auto found = std::ranges::find(schema->parameters, parameter, &model::ParameterSchema::id);
    if (found == schema->parameters.end() || value < found->minimum || value > found->maximum) return false;
    if (!editor_.setParameter(node, parameter, value)) return false;
    engine_.setParameter(node, parameter, static_cast<float>(value));
    return true;
}

compiler::CompileResult GraphSession::preview(const model::Connection& connection) const {
    return compiler::previewConnection(
        editor_.document().snapshot(), registry_, connection,
        compiler::CompilerLimits{.maxVoices = engine_.config().voiceCount});
}

void GraphSession::setTestHook(std::function<void()> hook) { testHook_ = std::move(hook); }

void GraphSession::syncParameters() {
    for (const auto& node : editor_.document().nodes()) {
        const auto* schema = registry_.find(node.typeId);
        if (schema == nullptr) continue;
        for (const auto& parameter : schema->parameters) {
            const auto stored = node.parameters.find(parameter.id);
            const double value = stored == node.parameters.end() ? parameter.defaultValue : stored->second;
            engine_.setParameter(node.id, parameter.id, static_cast<float>(value));
        }
    }
}

void GraphSession::threadMain() {
    while (true) {
        Job job;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [&] { return stop_ || dirty_; });
            if (stop_ && !dirty_) return;
            job = std::move(*queued_);
            queued_.reset();
            dirty_ = false;
            running_ = true;
        }
        if (testHook_) testHook_();
        auto result = runJob(job);
        {
            std::lock_guard lock(mutex_);
            pending_ = std::move(result);
            running_ = false;
            cv_.notify_all();
            if (stop_ && !dirty_) return;
        }
    }
}
} // namespace nodsynth::runtime
