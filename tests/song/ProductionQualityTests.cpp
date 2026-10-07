#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <nodsynth/compiler/GraphCompiler.h>
#include <nodsynth/nodes/BuiltinNodes.h>
#include <nodsynth/persist/ProjectFile.h>
#include <nodsynth/runtime/Engine.h>
#include <nodsynth/runtime/WavFile.h>
#include <nodsynth/song/AudioAnalysis.h>
#include <nodsynth/song/Automation.h>
#include <nodsynth/song/SongRenderer.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <numbers>
#include <nodsynth/model/GraphEditor.h>
#include <nodsynth/song/Process.h>
#include <nodsynth/song/AudioExport.h>
#include <nodsynth/song/Presets.h>

TEST_CASE("Q5 production roles expose actual macros metadata and compile across sample rates", "[production][role-pack]") {
    using namespace nodsynth;
    for(const auto* id:{"production-lead","production-pad","production-pluck","production-offbeat-bass","production-sub","production-riser","production-impact"}) {
        const auto info=song::findPreset(NOD_PRESETS_ROOT,id);REQUIRE(info);REQUIRE_FALSE(info->hash.empty());
        REQUIRE(info->production.find("auditionStatus")->asString()=="unheard");
        REQUIRE(info->production.find("recommendedMidiRange")->asArray().size()==2);
        std::string error;const auto graph=render::loadPatch(std::filesystem::path(NOD_PRESETS_ROOT)/info->patchPath,error);REQUIRE(graph);
        REQUIRE(song::validateMacros(*graph,error));REQUIRE(graph->macros.size()>=3);
        auto document=song::createSong({120,4,4,480,1});song::Track track;
        track.parameterValues={{"macro:tone",.4},{"macro:contour",.47},{"macro:release",.3}};
        for(auto rate:{44100u,48000u,96000u}) {
            const auto registry=nodes::builtinRegistry();const auto compiled=compiler::GraphCompiler{}.compile(*graph,registry);REQUIRE(compiled.graph);
            runtime::EngineConfig config;config.audio.sampleRate=rate;runtime::Engine engine(config);
            auto prepared=runtime::preparePlan(*compiled.graph,registry,nodes::builtinImplementations(),engine.config(),engine.voices(),0);REQUIRE(prepared.plan);
            std::vector<song::PreparedLane> lanes;REQUIRE(song::prepareAutomation(document,track,*graph,prepared.plan.get(),rate,lanes,error));REQUIRE(lanes.size()==3);
        }
    }
}

TEST_CASE("Q2 v2 lowpass cutoff response follows TPT reference without legacy state-output boost", "[production][filter-calibration]") {
    using namespace nodsynth;
    constexpr std::uint32_t block=128;
    for(auto rate:{44100u,48000u,96000u}) for(double cutoff:{1000.,20000.}) for(double resonance:{0.,.5,.98}) {
        const auto hz=std::min(cutoff,rate*.45);
        std::vector<float> input(block),output(block),parameters(block*3);
        std::fill_n(parameters.data(),block,static_cast<float>(cutoff));std::fill_n(parameters.data()+block,block,static_cast<float>(resonance));
        auto node=nodes::builtinImplementations().instantiate(model::NodeTypeId{"nod.lowpass-v2"});
        runtime::NodeBinding binding;binding.sampleRate=rate;binding.maxFrames=block;binding.voiceCount=1;
        binding.inputs={{{"audio-in"},input.data(),1,block,block,block}};binding.outputs={{{"audio-out"},output.data(),1,block,block,block}};
        binding.parameters={parameters.data(),3,block};node->bind(binding);double inputEnergy=0,outputEnergy=0;
        for(std::uint32_t begin=0;begin<rate/2;begin+=block) {
            const auto frames=std::min(block,rate/2-begin);
            for(std::uint32_t frame=0;frame<frames;++frame) input[frame]=static_cast<float>(.001*std::sin(2*std::numbers::pi*hz*(begin+frame)/rate));
            node->process(0,frames);
            for(std::uint32_t frame=0;frame<frames;++frame) if(begin+frame>rate/4) {inputEnergy+=input[frame]*input[frame];outputEnergy+=output[frame]*output[frame];}
        }
        const auto expected=-20*std::log10(2*(1-resonance));
        INFO("rate="<<rate<<" cutoff="<<cutoff<<" resonance="<<resonance);
        REQUIRE(10*std::log10(outputEnergy/inputEnergy)==Catch::Approx(expected).margin(.01));
    }
}

using namespace nodsynth;
using Catch::Approx;

TEST_CASE("Q3 Global delay follows Song tempo and diagnoses buffer overflow", "[production][music-delay][host-tempo]") {
    model::GraphSnapshot graph;model::NodeRecord delay;delay.id={"echo"};delay.typeId={"nod.music-delay"};delay.schemaVersion=1;
    delay.parameters[model::ParameterId{"syncBeats"}]=1;graph.nodes.push_back(delay);
    auto document=song::createSong({90,4,4,480,1});document.tempo.push_back({480,500000});
    song::Track track;std::vector<song::PreparedLane> lanes;std::string error;
    REQUIRE(song::prepareAutomation(document,track,graph,nullptr,48000,lanes,error));REQUIRE(lanes.size()==1);
    REQUIRE(song::parameterAt(lanes[0],0)==Approx(90));REQUIRE(song::parameterAt(lanes[0],32000)==Approx(120));
    graph.nodes[0].parameters[model::ParameterId{"syncBeats"}]=16;lanes.clear();
    REQUIRE_FALSE(song::prepareAutomation(document,track,graph,nullptr,48000,lanes,error));REQUIRE(error.starts_with("delay-time:"));
    track.parameterValues["echo/tempoBpm"]=240;lanes.clear();
    REQUIRE(song::prepareAutomation(document,track,graph,nullptr,48000,lanes,error));
    REQUIRE(lanes.size()==1);REQUIRE(lanes[0].points.empty());REQUIRE(lanes[0].base==240);
}

namespace {
persist::Json json(const std::string& text) {
    std::string error;
    auto value = persist::Json::parse(text, error);
    REQUIRE(value);
    return *value;
}
song::ApplyResult apply(song::SongDocument& document, const std::string& commands) {
    return song::applyCommands(document, json("{\"schemaVersion\":1,\"commands\":" + commands + "}"));
}

struct Fixture {
    std::filesystem::path root;
    explicit Fixture(const char* name) : root(std::filesystem::temp_directory_path() / name) {
        std::filesystem::remove_all(root); std::filesystem::create_directories(root);
    }
    ~Fixture() { std::filesystem::remove_all(root); }
};

TEST_CASE("Q1 custom macro ranges curves conflicts and editor persistence are explicit", "[production][macro]") {
    auto graph=nodes::stereoFilterPatch();const auto targets=song::parameterTargets(graph);
    const auto cutoff=std::find_if(targets.begin(),targets.end(),[](const auto& target){return target.parameter.value=="cutoff";});REQUIRE(cutoff!=targets.end());
    graph.macros={{"tone",{{cutoff->node,cutoff->parameter,200,12800,"log"}}}};
    auto document=song::createSong({120,4,4,480,1});song::Track track;
    song::ParameterLane lane;lane.id="macro:tone";lane.valueDomain="normalized";lane.interpolation="linear";lane.points={{0,0},{480,1}};track.parameterAutomation={lane};
    std::vector<song::PreparedLane> prepared;std::string error;
    REQUIRE(song::prepareAutomation(document,track,graph,nullptr,48000,prepared,error));REQUIRE(prepared.size()==1);
    REQUIRE(song::parameterAt(prepared[0],0)==Approx(200));REQUIRE(song::parameterAt(prepared[0],12000)==Approx(1600));REQUIRE(song::parameterAt(prepared[0],24000)==Approx(12800));
    track.parameterValues["macro:tone"]=.5;prepared.clear();REQUIRE(song::prepareAutomation(document,track,graph,nullptr,48000,prepared,error));REQUIRE(prepared[0].base==Approx(1600));
    auto direct=lane;direct.id=cutoff->id;direct.valueDomain="physical";direct.points={{0,200},{480,400}};track.parameterAutomation.push_back(direct);prepared.clear();
    REQUIRE_FALSE(song::prepareAutomation(document,track,graph,nullptr,48000,prepared,error));REQUIRE(error.find("multiple lanes")!=std::string::npos);
    const auto query=song::parametersJson(graph);REQUIRE(query.asArray().back().find("mapping")->asArray()[0].find("depth")->asNumber()==12600);
    Fixture fixture("nod-custom-macro");auto project=persist::projectFromGraph(graph);REQUIRE(persist::saveProject(fixture.root/"patch.json",project,error));
    const auto loaded=persist::loadProject(fixture.root/"patch.json",error);REQUIRE(loaded);REQUIRE(loaded->graph.macros[0].mappings[0].curve=="log");
    model::GraphEditor editor;editor.load(loaded->graph);REQUIRE(editor.document().snapshot().macros.size()==1);
    REQUIRE(editor.removeNode(cutoff->node));REQUIRE(editor.document().snapshot().macros.empty());REQUIRE(editor.undo());REQUIRE(editor.document().snapshot().macros.size()==1);
    auto edited=graph.macros;edited[0].mappings[0].maximum=6400;
    REQUIRE(editor.setMacros(edited));REQUIRE(editor.document().snapshot().macros[0].mappings[0].maximum==6400);
    REQUIRE(editor.undo());REQUIRE(editor.document().snapshot().macros[0].mappings[0].maximum==12800);
    REQUIRE(editor.redo());REQUIRE(editor.document().snapshot().macros[0].mappings[0].maximum==6400);
    graph.macros[0].mappings[0].minimum=0;REQUIRE_FALSE(song::validateMacros(graph,error));
    graph.macros[0].mappings[0].minimum=200;graph.macros[0].id="brightness";REQUIRE_FALSE(song::validateMacros(graph,error));
}

std::vector<float> renderPatch(const model::GraphSnapshot& graph, std::uint32_t rate, std::uint32_t block) {
    auto registry = nodes::builtinRegistry();
    auto compiled = compiler::GraphCompiler{}.compile(graph, registry);
    REQUIRE(compiled.graph);
    runtime::EngineConfig config;
    config.audio.sampleRate = rate; config.audio.maxFrames = block; config.parameterSmoothSeconds = 0;
    runtime::Engine engine(config);
    auto prepared = runtime::preparePlan(*compiled.graph, registry, nodes::builtinImplementations(), engine.config(), engine.voices(), 0);
    REQUIRE(prepared.plan);
    REQUIRE(engine.stage(std::move(prepared.plan)).accepted);
    const auto frames = rate / 4;
    std::vector<float> result(frames * 2), left(block), right(block);
    const runtime::MidiEvent on{0, runtime::MidiType::noteOn, 0, 69, 100};
    for (std::uint32_t start = 0; start < frames; start += block) {
        const auto count = std::min(block, frames - start);
        float* outputs[]{left.data(), right.data()};
        engine.process(outputs, 2, count, start == 0 ? std::span<const runtime::MidiEvent>(&on, 1) : std::span<const runtime::MidiEvent>{});
        for (std::uint32_t i = 0; i < count; ++i) { result[(start + i) * 2] = left[i]; result[(start + i) * 2 + 1] = right[i]; }
    }
    return result;
}

struct FilterHarness {
    std::uint32_t frames{128};
    std::unique_ptr<runtime::DspNode> node;
    std::vector<float> input, output, modulation, parameters;
    FilterHarness(const char* type, float base, float depth, float mod, double rate = 48000)
        : node(nodes::builtinImplementations().instantiate(model::NodeTypeId{type})),
          input(frames * 2), output(frames * 2), modulation(frames, mod), parameters(frames * 3) {
        input[0] = 1;
        for (std::uint32_t i = 0; i < frames; ++i) { parameters[i] = base; parameters[frames + i] = .1f; parameters[frames * 2 + i] = depth; }
        runtime::NodeBinding binding;
        binding.sampleRate = rate; binding.maxFrames = frames; binding.voiceCount = 1;
        binding.inputs = {{{"audio-in"}, input.data(), 2, frames, frames * 2, frames},
                          {{"cutoff-mod"}, modulation.data(), 1, frames, frames, frames}};
        binding.outputs = {{{"audio-out"}, output.data(), 2, frames, frames * 2, frames}};
        binding.parameters = {parameters.data(), 3, frames};
        node->bind(binding); node->reset(); node->process(0, frames);
    }
};
}

TEST_CASE("Q1 migration requires decisions compiles and retains legacy source audio", "[production][migration]") {
    const auto original=nodes::sinePatch();std::string error;
    const auto decisions=json(R"({"gain":{"gain":{"base":1}},"mono":{"oscillator/audio->gain/audio-in":{"pan":0,"level":1.4142135623730951}}})");
    REQUIRE_FALSE(song::migrateProductionGraph(original,json("{}"),error));
    const auto migrated=song::migrateProductionGraph(original,decisions,error);REQUIRE(migrated);REQUIRE(migrated->nodes.size()==original.nodes.size()+2);
    REQUIRE(original.nodes[4].typeId.value=="nod.gain");REQUIRE(migrated->nodes[4].typeId.value=="nod.gain-v2");
    for(auto rate:{44100u,48000u,96000u}) {
        const auto before=renderPatch(original,rate,128),after=renderPatch(*migrated,rate,128);
        REQUIRE(before.size()==after.size());for(std::size_t sample=0;sample<before.size();++sample) REQUIRE(after[sample]==Approx(before[sample]).margin(2e-7));
        REQUIRE(renderPatch(*migrated,rate,64)==after);REQUIRE(renderPatch(*migrated,rate,512)==after);
    }
    auto filter=nodes::filterPatch();
    auto settings=json(R"({"cutoff":{"filter":{"action":"normalized","baseHz":800,"depthOctaves":4}},"gain":{"gain":{"base":1}},"mono":{"oscillator/audio->filter/audio-in":{"pan":0,"level":1}}})");
    REQUIRE_FALSE(song::migrateProductionGraph(filter,settings,error));REQUIRE(error.find("normalized source")!=std::string::npos);
    settings.set("cutoff",json(R"({"filter":{"action":"normalized","baseHz":800,"depthOctaves":4,"sourceIsNormalized":true}})"));
    for(auto& node:filter.nodes) if(node.id.value=="cutoff") {node.parameters[model::ParameterId{"scale"}]=1;node.parameters[model::ParameterId{"bias"}]=0;}
    const auto modulated=song::migrateProductionGraph(filter,settings,error);REQUIRE(modulated);
    const auto targets=song::parameterTargets(*modulated);const auto cutoff=std::find_if(targets.begin(),targets.end(),[](const auto& target){return target.id=="filter/cutoff";});
    REQUIRE(cutoff!=targets.end());REQUIRE(cutoff->automatable);REQUIRE(cutoff->base==800);
    auto disconnected=settings;disconnected.set("cutoff",json(R"({"filter":{"action":"disconnect","baseHz":800}})"));
    const auto removed=song::migrateProductionGraph(filter,disconnected,error);REQUIRE(removed);
    REQUIRE(std::none_of(removed->connections.begin(),removed->connections.end(),[](const auto& wire){return wire.to.nodeId.value=="filter" && wire.to.portId.value=="cutoff-mod";}));
    auto unused=decisions;unused.set("gain",json(R"({"gain":{"base":1},"unknown":{}})"));REQUIRE_FALSE(song::migrateProductionGraph(original,unused,error));REQUIRE(error.find("unused decision")!=std::string::npos);
    Fixture fixture("nod-migrate-cli");auto project=persist::projectFromGraph(original);project.root.set("custom",persist::Json::string("preserved"));project.viewport.zoom=2;
    REQUIRE(persist::saveProject(fixture.root/"old.json",project,error));const auto sourceHash=song::hashFile(fixture.root/"old.json");
    REQUIRE(song::writeJsonAtomic(fixture.root/"decisions.json",decisions,error));
    const auto invoke=[&]() {return song::runProcess({{NOD_EXECUTABLE,"patch","migrate",(fixture.root/"old.json").string(),"--output",(fixture.root/"new.json").string(),"--decisions",(fixture.root/"decisions.json").string(),"--json"}});};
    REQUIRE(invoke().exitCode==0);REQUIRE(song::hashFile(fixture.root/"old.json")==sourceHash);
    const auto restored=persist::loadProject(fixture.root/"new.json",error);REQUIRE(restored);REQUIRE(restored->viewport.zoom==2);REQUIRE(restored->root.find("custom")->asString()=="preserved");
    REQUIRE(invoke().exitCode!=0);
}

TEST_CASE("Q1 custom macros render physical equivalents and invalidate their audio cache", "[production][macro][macro-render]") {
    Fixture fixture("nod-macro-render");std::string error;auto graph=nodes::stereoFilterPatch();
    graph.macros={{"tone",{{model::NodeId{"filter"},model::ParameterId{"cutoff"},200,12800,"log"}}}};
    auto project=persist::projectFromGraph(graph);REQUIRE(persist::saveProject(fixture.root/"patch.json",project,error));
    auto document=song::createSong({120,4,4,480,1});document.baseDirectory=fixture.root;
    song::Track track;track.id="lead";song::Clip clip;clip.id="phrase";clip.length=1920;clip.notes={{"note",0,240,69,100,0}};track.clips={clip};track.parameterValues["macro:tone"]=.5;document.tracks={track};
    REQUIRE(song::bindPatch(document,"lead","patch.json",fixture.root/"patch.json",error));
    song::SongRenderOptions options;options.tailSeconds=.1;options.cacheDirectory=fixture.root/"cache";
    REQUIRE(song::renderSong(document,options,fixture.root/"macro.wav").ok);
    document.tracks[0].parameterValues.clear();document.tracks[0].parameterValues["filter/cutoff"]=1600;
    REQUIRE(song::renderSong(document,options,fixture.root/"physical.wav").ok);REQUIRE(song::hashFile(fixture.root/"macro.wav")==song::hashFile(fixture.root/"physical.wav"));
    document.tracks[0].parameterValues.clear();document.tracks[0].parameterValues["macro:tone"]=.5;
    project.graph.macros[0].mappings[0].maximum=3200;REQUIRE(persist::saveProject(fixture.root/"patch.json",project,error));
    REQUIRE_FALSE(song::renderSong(document,options,fixture.root/"stale.wav").ok);
    REQUIRE(song::bindPatch(document,"lead","patch.json",fixture.root/"patch.json",error));
    const auto changed=song::renderSong(document,options,fixture.root/"changed.wav");REQUIRE(changed.ok);REQUIRE_FALSE(changed.cacheHit);
    REQUIRE(song::hashFile(fixture.root/"changed.wav")!=song::hashFile(fixture.root/"macro.wav"));
    options.useCache=false;REQUIRE(song::renderSong(document,options,fixture.root/"fresh.wav").ok);REQUIRE(song::hashFile(fixture.root/"changed.wav")==song::hashFile(fixture.root/"fresh.wav"));
}

TEST_CASE("Q0 schema targets expose blockers and reject addresses precisely", "[production][song]") {
    const auto graph = nodes::filterPatch();
    const auto targets = song::parameterTargets(graph);
    const auto find = [&](const char* id) -> const song::ParameterTarget& {
        const auto target = std::find_if(targets.begin(), targets.end(), [&](const auto& t) { return t.id == id; });
        REQUIRE(target != targets.end()); return *target;
    };
    REQUIRE_FALSE(find("filter/cutoff").automatable);
    REQUIRE(find("filter/cutoff").reason == "control-override");
    REQUIRE(find("filter/cutoff").blockingConnections[0].from.nodeId.value == "cutoff");
    REQUIRE(find("envelope/attack").automatable);
    REQUIRE(find("oscillator/waveform").reason == "unsupported-target");
    song::SongDocument document; song::Track track;
    for (const auto& [id, code] : std::vector<std::pair<std::string, std::string>>{
            {"filter/cutoff", "control-override"}, {"oscillator/waveform", "unsupported-target"}, {"missing/cutoff", "unknown-address"}}) {
        track.parameterValues = {{id, 1}};
        std::vector<song::PreparedLane> lanes; std::string error;
        REQUIRE_FALSE(song::prepareAutomation(document, track, graph, nullptr, 48000, lanes, error));
        REQUIRE(song::automationErrorCode(error) == code);
    }
    const auto query = song::parametersJson(graph).dump();
    REQUIRE(query.find("blockingConnections") != std::string::npos);
    REQUIRE(query.find("control-override") != std::string::npos);
    const auto preview = song::productionMigrationPreview(graph);
    REQUIRE_FALSE(preview.find("applied")->asBool());
    REQUIRE(std::any_of(preview.find("requirements")->asArray().begin(),preview.find("requirements")->asArray().end(),[](const auto& item){return item.find("code")->asString()=="explicit-modulation-mapping-required";}));
}

TEST_CASE("Q1 cutoff combines octaves sample accurately without stereo crosstalk", "[production][dsp]") {
    for (const auto rate : {44100u, 48000u, 96000u}) for (const auto* type : {"nod.lowpass-v2", "nod.highpass-v2"}) {
        FilterHarness modulated(type, 1000, 2, .5f, rate), absolute(type, 2000, 0, 0, rate);
        REQUIRE(modulated.output == absolute.output);
        REQUIRE(std::any_of(modulated.output.begin(), modulated.output.begin() + 128, [](float value) { return value != 0; }));
        for (std::size_t i = 128; i < 256; ++i) REQUIRE(modulated.output[i] == 0);
        FilterHarness invalid(type, 1000, 8, std::numeric_limits<float>::infinity(), rate);
        for (const auto value : invalid.output) REQUIRE(std::isfinite(value));
    }
    auto graph = nodes::stereoFilterPatch();
    song::SongDocument document; song::Track track;
    track.parameterAutomation.push_back({"filter/cutoff", {{0, 200}, {480, 5000}}, "physical", "linear"});
    std::vector<song::PreparedLane> lanes; std::string error;
    REQUIRE(song::prepareAutomation(document, track, graph, nullptr, 48000, lanes, error));
    REQUIRE(lanes.size() == 1);
    REQUIRE(song::parameterAt(lanes[0], 12000) == 2600);
}

TEST_CASE("Q1 stereo voice path and legacy patches are invariant to block size", "[production][dsp]") {
    for (const auto rate : {44100u, 48000u, 96000u}) {
        for (const auto& graph : {nodes::sinePatch(), nodes::filterPatch(), nodes::stereoFilterPatch()}) {
            const auto reference = renderPatch(graph, rate, 128);
            for (const auto block : {64u, 512u}) {
                const auto other = renderPatch(graph, rate, block);
                REQUIRE(other.size() == reference.size());
                for (std::size_t i = 0; i < reference.size(); ++i) REQUIRE(other[i] == Approx(reference[i]).margin(1e-6));
            }
            const bool stereo = graph.nodes.back().typeId.value == "nod.lowpass-v2";
            double difference = 0;
            for (std::size_t i = 0; i < reference.size(); i += 2) difference += std::fabs(reference[i] - reference[i + 1]);
            if (stereo) REQUIRE(difference > 1);
            else REQUIRE(difference == 0);
        }
    }
}

TEST_CASE("Q1 stereo gain mix and voice summing keep channels independent", "[production][dsp]") {
    constexpr std::uint32_t frames = 8;
    std::vector<float> a(frames * 2), b(frames * 2), control(frames, .25f), output(frames * 2), base(frames, .5f);
    std::fill(a.begin(), a.begin() + frames, 1); std::fill(a.begin() + frames, a.end(), 2);
    std::fill(b.begin(), b.begin() + frames, 3); std::fill(b.begin() + frames, b.end(), 4);
    runtime::NodeBinding binding;
    binding.sampleRate = 48000; binding.maxFrames = frames; binding.voiceCount = 1;
    binding.inputs = {{{"audio-in"}, a.data(), 2, frames, frames * 2, frames},
                      {{"gain"}, control.data(), 1, frames, frames, frames}};
    binding.outputs = {{{"audio-out"}, output.data(), 2, frames, frames * 2, frames}};
    binding.parameters = {base.data(), 1, frames};
    auto gain = nodes::builtinImplementations().instantiate(model::NodeTypeId{"nod.gain-v2"});
    gain->bind(binding); gain->process(0, frames);
    REQUIRE(output[0] == .125f); REQUIRE(output[frames] == .25f);
    binding.inputs = {{{"a"}, a.data(), 2, frames, frames * 2, frames},
                      {{"b"}, b.data(), 2, frames, frames * 2, frames},
                      {{"mix"}, control.data(), 1, frames, frames, frames}};
    binding.outputs[0].port.value = "audio";
    auto mix = nodes::builtinImplementations().instantiate(model::NodeTypeId{"nod.mix-v2"});
    mix->bind(binding); mix->process(0, frames);
    REQUIRE(output[0] == 1.5f); REQUIRE(output[frames] == 2.5f);
    std::vector<float> voices(frames * 4);
    std::copy(a.begin(), a.end(), voices.begin()); std::copy(b.begin(), b.end(), voices.begin() + frames * 2);
    std::vector<float> atten0(frames, .5f), atten1(frames, .25f);
    const float* attenuation[]{atten0.data(), atten1.data()};
    binding.voiceCount = 2; binding.attenuation = attenuation;
    binding.inputs = {{{"voices"}, voices.data(), 2, frames, frames * 2, frames}};
    std::fill(base.begin(), base.end(), 1.f);
    auto sum = nodes::builtinImplementations().instantiate(model::NodeTypeId{"nod.voice-mix-v2"});
    sum->bind(binding); sum->process(0, frames);
    REQUIRE(output[0] == 1.25f); REQUIRE(output[frames] == 2.f);
}

TEST_CASE("Q1 pump skip and mute preserve the user lane and survive undo", "[production][song]") {
    auto document = song::createSong({120, 4, 4, 480, 16});
    REQUIRE(apply(document, R"([{"op":"create-track","id":"bass"},
        {"op":"set-gain","track":"bass","gain":0.5},
        {"op":"set-gain-automation","track":"bass","points":[{"tick":0,"gain":0.4}]},
        {"op":"add-pump","track":"bass","startBar":1,"endBar":17,"period":"1/4","recovery":"1/8","depth":0.8,"skipBars":[4]},
        {"op":"set-audio-mute","track":"bass","muteBars":[8,16],"fadeMs":2}])").ok);
    REQUIRE(document.tracks[0].gainAutomation.size() == 1);
    REQUIRE(document.tracks[0].gainAutomation[0].gain == .4);
    REQUIRE(document.tracks[0].pump->skip[0].startTick == 5760);
    const auto serialized = song::mixControlsJson(document.tracks[0]).dump(-1);
    REQUIRE(song::undoSong(document).ok); REQUIRE(document.tracks.empty());
    REQUIRE(song::redoSong(document).ok); REQUIRE(song::mixControlsJson(document.tracks[0]).dump(-1) == serialized);
    std::string error;
    auto restored = song::songFromJson(song::toJson(document), error); REQUIRE(restored);
    for (const auto rate : {44100u, 48000u, 96000u}) {
        song::PreparedTrackMix mix;
        REQUIRE(song::prepareTrackMix(*restored, restored->tracks[0], rate, mix, error));
        REQUIRE(mix.gainAt(0) == Approx(.2));
        REQUIRE(mix.gainAt(rate / 100) < .1);
        REQUIRE(mix.gainAt(*song::sampleAtTick(document, 5760 + 20, rate)) == Approx(.2));
        REQUIRE(mix.gainAt(*song::sampleAtTick(document, 13440 + 20, rate)) == 0);
        REQUIRE(mix.gainAt(*song::sampleAtTick(document, 28800 + 20, rate)) == 0);
        const auto start = *song::sampleAtTick(document, 13440, rate);
        REQUIRE(mix.gainAt(start) == 0);
        REQUIRE(mix.gainAt(start - 1) < mix.gainAt(start - 50));
    }
    REQUIRE_FALSE(apply(document, R"([{"op":"add-pump","track":"bass","startBar":1,"endBar":2,"period":"1/4","recovery":"1/8","depth":0.5}])").ok);
    REQUIRE(apply(document, R"([{"op":"clear-pump","track":"bass"}])").ok);
    REQUIRE_FALSE(document.tracks[0].pump);
}

TEST_CASE("Q1 balance preserves center and v3 keeps absolute gain behavior", "[production][song]") {
    song::SongDocument document; song::Track track;
    track.gain = .5; track.gainAutomation = {{480, .4}};
    track.panMode = "balance"; track.gainMode = "multiply";
    song::PreparedTrackMix mix; std::string error; float left = 0, right = 0;
    REQUIRE(song::prepareTrackMix(document, track, 48000, mix, error));
    mix.gainsAt(0, left, right); REQUIRE(left == .5f); REQUIRE(right == .5f);
    mix.gainsAt(24000, left, right); REQUIRE(left == Approx(.2)); REQUIRE(right == Approx(.2));
    document.tracks.push_back(track);
    auto old = song::toJson(document); old.set("formatVersion", persist::Json::number(3));
    auto tracks = persist::Json::array(); auto item = persist::Json::object();
    item.set("id", persist::Json::string("old")); item.set("gain", persist::Json::number(.5));
    item.set("gainAutomation", json(R"([{"tick":480,"gain":0.4}])")); tracks.push(std::move(item)); old.set("tracks", std::move(tracks));
    auto loaded = song::songFromJson(old, error); REQUIRE(loaded);
    REQUIRE(loaded->tracks[0].gainMode == "legacy"); REQUIRE(loaded->tracks[0].panMode == "equal-power");
    REQUIRE(song::prepareTrackMix(*loaded, loaded->tracks[0], 48000, mix, error));
    REQUIRE(mix.gainAt(0) == Approx(.4));
    loaded->tracks[0].gainAutomation = {{0, .12345678}, {480, .98765432}};
    REQUIRE(song::prepareTrackMix(*loaded, loaded->tracks[0], 48000, mix, error));
    const float from = static_cast<float>(.12345678), to = static_cast<float>(.98765432);
    REQUIRE(mix.gainAt(12345) == static_cast<float>(from + (12345.0 / 24000) * (to - from)));
}

TEST_CASE("Q1 very short pumps reserve both attack and recovery", "[production][song]") {
    song::SongDocument document;
    song::Track track; track.gainMode = "multiply";
    track.pump = song::Pump{0, 480, 120, 60, 1, 1000};
    song::PreparedTrackMix mix; std::string error;
    REQUIRE(song::prepareTrackMix(document, track, 48000, mix, error));
    REQUIRE(mix.gainAt(0) == 1);
    REQUIRE(mix.gainAt(1500) == 0);
    REQUIRE(mix.gainAt(2999) > .999);
    REQUIRE(mix.gainAt(3000) == 1);
    document.tracks.push_back(track);
    document.tracks[0].pump->period = 0;
    REQUIRE_FALSE(song::validateDocument(document).ok);
    REQUIRE_FALSE(song::prepareTrackMix(document, document.tracks[0], 48000, mix, error));
}

TEST_CASE("Q1 mix control edits remix cached dry audio identically to fresh render", "[production][song]") {
    Fixture fixture("nod-production-mix-cache");
    auto patch = persist::projectFromGraph(nodes::stereoFilterPatch()); std::string error;
    REQUIRE(persist::saveProject(fixture.root / "patch.json", patch, error));
    auto document = song::createSong({120, 4, 4, 480, 2}); document.baseDirectory = fixture.root;
    REQUIRE(apply(document, R"([{"op":"create-track","id":"lead"},
        {"op":"set-instrument","track":"lead","patch":"patch.json"},
        {"op":"set-mix-mode","track":"lead","panMode":"balance"},
        {"op":"add-pattern","track":"lead","id":"notes","startBar":1,"endBar":3,
         "grid":"1/4","duration":"1/4","pitches":[60],"velocities":[100]},
        {"op":"set-parameter-automation","track":"lead","parameter":"filter/cutoff","valueDomain":"physical",
         "points":[{"tick":0,"value":300},{"tick":3840,"value":6000}]}])").ok);
    song::SongRenderOptions options; options.cacheDirectory = fixture.root / "cache"; options.tailSeconds = .1;
    REQUIRE(song::renderSong(document, options, fixture.root / "initial.wav").ok);
    REQUIRE(apply(document, R"([{"op":"set-gain","track":"lead","gain":0.6},
        {"op":"add-pump","track":"lead","startBar":1,"endBar":3,"period":"1/4","recovery":"1/8","depth":0.6,"skipBars":[1]},
        {"op":"set-audio-mute","track":"lead","muteBars":[2]}])").ok);
    auto remix = song::renderSong(document, options, fixture.root / "remix.wav");
    REQUIRE(remix.ok); REQUIRE(remix.cacheHit); REQUIRE(remix.cacheReason == "remix-dry-tracks");
    options.useCache = false;
    REQUIRE(song::renderSong(document, options, fixture.root / "fresh.wav").ok);
    runtime::WavData cached, fresh;
    REQUIRE(runtime::readWav(fixture.root / "remix.wav", cached, error));
    REQUIRE(runtime::readWav(fixture.root / "fresh.wav", fresh, error));
    REQUIRE(cached.interleaved == fresh.interleaved);
}

TEST_CASE("Q0 stereo analysis identifies anti-phase mono cancellation", "[production][song]") {
    Fixture fixture("nod-production-stereo-analysis");
    std::vector<float> samples(9600);
    for (std::size_t i = 0; i < samples.size(); i += 2) {
        const float sample = static_cast<float>(.2 * std::sin(static_cast<double>(i) * .04));
        samples[i] = sample; samples[i + 1] = -sample;
    }
    runtime::WavStream stream; std::string error;
    REQUIRE(stream.open(fixture.root / "anti.wav", 48000, 2, error));
    REQUIRE(stream.write(samples.data(), samples.size(), error)); REQUIRE(stream.commit(error));
    const auto analysis = song::analyzeWav(fixture.root / "anti.wav");
    REQUIRE(analysis.ok); REQUIRE(analysis.correlation); REQUIRE(*analysis.correlation == Approx(-1));
    REQUIRE(analysis.midEnergy == 0); REQUIRE(analysis.sideEnergy > .01); REQUIRE(analysis.monoRms == 0);
    runtime::WavData invalid;
    invalid.sampleRate = 48000; invalid.channels = 2;
    invalid.interleaved = {std::numeric_limits<float>::infinity(), 1};
    REQUIRE(runtime::writeWav(fixture.root / "invalid.wav", invalid, error));
    const auto rejected = song::analyzeWav(fixture.root / "invalid.wav");
    REQUIRE_FALSE(rejected.ok);
    REQUIRE(persist::Json::parse(song::analysisJson(rejected).dump(), error));
}
