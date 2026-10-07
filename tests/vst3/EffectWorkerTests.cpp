#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <nodsynth/host/EffectWorker.h>
#include <nodsynth/host/Vst3Host.h>
#include <fstream>
#include <cmath>
#include <array>
#include <tuple>
#include <nodsynth/song/Mixer.h>
#include <nodsynth/song/SongRenderer.h>
#include <nodsynth/nodes/BuiltinNodes.h>
#include <nodsynth/persist/ProjectFile.h>
using namespace nodsynth;

TEST_CASE("Q3 isolated VST3 effect accepts stereo input state automation bypass and tempo", "[production][vst3-effect][worker]") {
    std::string error;host::EffectWorkerOptions options;options.executable=NOD_VST3_WORKER;options.plugin=NOD_EFFECT_PROBE;
    auto effect=host::makeWorkerEffect(options);effects::Config config;config.type="vst3";config.parameters={{"vst3:7",.25},{"vst3:11",1}};
    REQUIRE(effect->prepare(48000,128,config,error));REQUIRE(effect->latencySamples()==128);REQUIRE(effect->parameterSchema()->parameters.size()==3);
    std::array<float,128> left{},right{};std::array<double,128> tempo{};
    for(std::size_t frame=0;frame<128;++frame) {left[frame]=1;right[frame]=.5f;tempo[frame]=frame<64?120:240;}
    effect->process({left.data(),right.data(),128,nullptr,nullptr,tempo.data()});REQUIRE_FALSE(effect->errorReason());
    for(float value:left) REQUIRE(value==0);
    left.fill(0);right.fill(0);tempo.fill(120);effect->process({left.data(),right.data(),128,nullptr,nullptr,tempo.data()});
    for(std::size_t frame=0;frame<128;++frame) {REQUIRE(left[frame]==(frame<64?.5f:1.f));REQUIRE(right[frame]==left[frame]*.5f);}
    effect->reset();REQUIRE_FALSE(effect->errorReason());left.fill(1);right.fill(.5f);
    const effects::ParameterEvent point{17,0,.75};effect->process({left.data(),right.data(),128,nullptr,nullptr,tempo.data(),{&point,1}});
    left.fill(0);right.fill(0);effect->process({left.data(),right.data(),128});
    for(std::size_t frame=0;frame<128;++frame) REQUIRE(left[frame]==(frame<17?.5f:1.5f));
    config.bypass=true;REQUIRE(effect->prepare(48000,128,config,error));left.fill(.7f);right.fill(.2f);effect->process({left.data(),right.data(),128});
    left.fill(0);right.fill(0);effect->process({left.data(),right.data(),128});for(std::size_t frame=0;frame<128;++frame) {REQUIRE(left[frame]==.7f);REQUIRE(right[frame]==.2f);}
    const auto path=std::filesystem::temp_directory_path()/"nod-effect-probe-state.bin";
    host::Vst3Plugin plugin;REQUIRE(plugin.open(NOD_EFFECT_PROBE,error));REQUIRE(plugin.activate(48000,128,error));
    std::vector<host::Vst3ParamPoint> statePoints{{0,7,.125}};REQUIRE(plugin.process(left.data(),right.data(),128,{},error,left.data(),right.data(),&statePoints));
    std::vector<char> bytes;REQUIRE(plugin.saveState(bytes,error));{std::ofstream file(path,std::ios::binary);file.write(bytes.data(),bytes.size());REQUIRE(file.good());}
    options.state=path;effect=host::makeWorkerEffect(options);config.parameters.clear();config.bypass=false;REQUIRE(effect->prepare(48000,128,config,error));
    left.fill(1);right.fill(0);effect->process({left.data(),right.data(),128});left.fill(0);effect->process({left.data(),right.data(),128});for(float sample:left) REQUIRE(sample==.25f);
    std::filesystem::remove(path);
}
TEST_CASE("Q3 VST3 effect worker failures and latency changes are diagnosed", "[production][vst3-effect][worker-failure]") {
    for(const auto& item:std::vector<std::pair<double,std::string>>{{.3,"process failed"},{.5,"timeout"},{.75,"crash"},{1,"changed latency"}}) {
        host::EffectWorkerOptions options;options.executable=NOD_VST3_WORKER;options.plugin=NOD_EFFECT_PROBE;options.timeoutMs=500;
        auto effect=host::makeWorkerEffect(options);effects::Config config;config.type="vst3";config.parameters={{"vst3:9",item.first}};std::string error;
        REQUIRE(effect->prepare(48000,128,config,error));std::array<float,128> left{},right{};effect->process({left.data(),right.data(),128});
        INFO("mode="<<item.first<<" reason="<<(effect->errorReason()?effect->errorReason():"none"));
        REQUIRE(effect->errorReason());REQUIRE(std::string(effect->errorReason()).find(item.second)!=std::string::npos);REQUIRE(std::isnan(left[0]));
    }
    host::EffectWorkerOptions options;options.executable=NOD_VST3_WORKER;options.plugin=NOD_VST3_PLUGIN;
    auto synth=host::makeWorkerEffect(options);effects::Config config;config.type="vst3";std::string error;REQUIRE_FALSE(synth->prepare(48000,128,config,error));REQUIRE(error.find("input")!=std::string::npos);
    options.plugin=NOD_EFFECT_PROBE;auto effect=host::makeWorkerEffect(options);config.parameters={{"vst3:999",.5}};
    REQUIRE_FALSE(effect->prepare(48000,128,config,error));REQUIRE(error.find("unknown")!=std::string::npos);
}

TEST_CASE("Q3 Song VST3 serial inserts compensate parallel paths and automate across blocks", "[production][vst3-effect][mixer]") {
    auto document=song::createSong({120,4,4,480,1});
    document.resources.push_back({"plugin",NOD_EFFECT_PROBE,song::hashFile(NOD_EFFECT_PROBE),"vst3-plugin"});
    effects::Config effect;effect.id="gain";effect.type="vst3";effect.pluginResource="plugin";effect.parameters={{"vst3:7",.25}};
    song::Track wet;wet.id="wet";wet.panMode="balance";wet.gainMode="multiply";wet.output="bus";wet.inserts={effect};
    song::Track dry;dry.id="dry";dry.panMode="balance";dry.gainMode="multiply";document.tracks={wet,dry};
    song::Bus bus;bus.id="bus";bus.inserts={effect};document.buses={bus};document.masterInserts={effect};
    const auto render=[&](std::uint32_t block) {
        song::SongMixer mixer;std::string error;REQUIRE(mixer.prepare(document,48000,block,error));REQUIRE(mixer.latencySamples()==384);
        const auto report=mixer.effectReport(document,false);REQUIRE(report.asArray().size()==3);
        REQUIRE(report.asArray()[0].find("workerPrivateBytes")->asNumber()>0);
        std::vector<float> left(block),right(block),output(block*2),result(2048);
        for(std::uint32_t begin=0;begin<1024;begin+=block) {
            const auto frames=std::min(block,1024-begin);std::fill(left.begin(),left.end(),0);std::fill(right.begin(),right.end(),0);
            if(begin==0) {left[0]=1;right[0]=.5;}
            mixer.setTrackInput(0,left.data(),right.data(),frames);mixer.setTrackInput(1,left.data(),right.data(),frames);
            mixer.process(begin,frames,output.data());REQUIRE_FALSE(mixer.errorReason());std::copy_n(output.begin(),frames*2,result.begin()+begin*2);
        }
        return result;
    };
    auto reference=render(128);REQUIRE(reference[384*2]==.625f);REQUIRE(reference[384*2+1]==.3125f);
    REQUIRE(render(64)==reference);REQUIRE(render(512)==reference);
    document.tracks[0].inserts[0].bypass=true;reference=render(128);REQUIRE(reference[384*2]==.75f);
    document.tracks[0].inserts[0].bypass=false;
    document.tracks[0].inserts[0].automation.push_back({"vst3:7","step",{{0,.75}}});
    reference=render(128);REQUIRE(reference[384*2]==.875f);REQUIRE(render(64)==reference);
}

TEST_CASE("Q3 Song plugin state collection relocation cache and cancellation preserve dependencies", "[production][vst3-effect][song]") {
    const auto root=std::filesystem::temp_directory_path()/"nod-song-effect-acceptance";
    std::filesystem::remove_all(root);std::filesystem::create_directories(root);
    auto document=song::createSong({120,4,4,480,1});document.baseDirectory=root;
    song::Track track;track.id="lead";track.panMode="balance";track.gainMode="multiply";
    song::Clip clip;clip.id="phrase";clip.length=1920;clip.notes={{"note",0,240,69,100,0}};track.clips={clip};document.tracks={track};
    std::string error;auto patch=persist::projectFromGraph(nodes::unisonPatch());REQUIRE(persist::saveProject(root/"patch.json",patch,error));
    REQUIRE(song::bindPatch(document,"lead","patch.json",root/"patch.json",error));
    host::Vst3Plugin plugin;REQUIRE(plugin.open(NOD_EFFECT_PROBE,error));REQUIRE(plugin.activate(48000,128,error));
    std::vector<char> state;REQUIRE(plugin.saveState(state,error));
    {std::ofstream output(root/"state.bin",std::ios::binary);output.write(state.data(),state.size());}
    auto commands=persist::Json::array();
    for(const auto& [op,id,path]:std::vector<std::tuple<std::string,std::string,std::string>>{
        {"add-plugin-resource","plugin",NOD_EFFECT_PROBE},{"add-plugin-state","state","state.bin"}}) {
        auto command=persist::Json::object();command.set("op",persist::Json::string(op));command.set("id",persist::Json::string(id));
        command.set("path",persist::Json::string(path));commands.push(std::move(command));
    }
    auto batch=persist::Json::object();batch.set("schemaVersion",persist::Json::number(1));batch.set("commands",std::move(commands));
    REQUIRE(song::applyCommands(document,batch).ok);
    effects::Config effect;effect.id="external";effect.type="vst3";effect.pluginResource="plugin";effect.stateResource="state";effect.parameters={{"vst3:7",.25}};
    document.masterInserts={effect};
    const auto collect=persist::Json::parse(R"({"schemaVersion":1,"commands":[{"op":"collect-resources"}]})",error);REQUIRE(collect);
    REQUIRE(song::applyCommands(document,*collect).ok);
    REQUIRE(document.resources.back().path=="assets/plugin-state/"+song::hashFile(root/"state.bin")+".bin");
    REQUIRE(song::undoSong(document).ok);REQUIRE(document.resources.back().path=="state.bin");REQUIRE(song::redoSong(document).ok);
    std::filesystem::create_directories(root/"moved");REQUIRE(song::saveSong(root/"moved/song.json",document,error));
    auto moved=song::loadSong(root/"moved/song.json",error);REQUIRE(moved);
    REQUIRE(song::hashFile(song::resolveResourcePath(moved->baseDirectory,moved->resources.back().path))==moved->resources.back().hash);
    song::SongRenderOptions options;options.sampleRate=8000;options.tailSeconds=.2;options.cacheDirectory=root/"cache";
    auto cold=song::renderSong(*moved,options,root/"cold.wav",root/"stems");INFO(cold.message);REQUIRE(cold.ok);REQUIRE(cold.latencySamples==128);
    REQUIRE(cold.effects.asArray().size()==1);REQUIRE(cold.effects.asArray()[0].find("stateHash"));
    auto warm=song::renderSong(*moved,options,root/"warm.wav");REQUIRE(warm.ok);REQUIRE(warm.cacheHit);
    REQUIRE(song::hashFile(root/"cold.wav")==song::hashFile(root/"warm.wav"));
    std::size_t finished=0;for(const auto& entry:std::filesystem::recursive_directory_iterator(root/"cache")) if(entry.path().filename()=="mix.wav") ++finished;
    REQUIRE(finished==0);
    options.freezeExternal=true;REQUIRE(song::renderSong(*moved,options,root/"frozen.wav").ok);
    options.previewStartTick=0;options.previewEndTick=480;
    const auto cached=song::renderSong(*moved,options,root/"preview.wav");REQUIRE(cached.ok);REQUIRE(cached.cacheHit);
    REQUIRE(cached.effects.asArray().size()==1);REQUIRE(cached.effects.asArray()[0].find("frozen")->asBool());
    options.useCache=false;const auto fresh=song::renderSong(*moved,options,root/"fresh-preview.wav");REQUIRE(fresh.ok);
    REQUIRE(song::hashFile(root/"preview.wav")==song::hashFile(root/"fresh-preview.wav"));
    options.previewStartTick.reset();options.previewEndTick.reset();options.useCache=true;options.freezeExternal=false;
    moved->masterInserts[0].parameters["vst3:9"]=.3;
    const auto failed=song::renderSong(*moved,options,root/"failed.wav",root/"failed-stems");REQUIRE_FALSE(failed.ok);
    REQUIRE(failed.message.find("process failed")!=std::string::npos);REQUIRE_FALSE(std::filesystem::exists(root/"failed.wav"));
    for(const auto& entry:std::filesystem::recursive_directory_iterator(root/"failed-stems")) REQUIRE(entry.path().extension()!=".wav");
    moved->masterInserts[0].parameters.erase("vst3:9");
    REQUIRE(song::renderSong(*moved,options,root/"recovered.wav").ok);
    {std::ofstream output(song::resolveResourcePath(moved->baseDirectory,moved->resources.back().path),std::ios::app|std::ios::binary);output.put('x');}
    const auto changed=song::renderSong(*moved,options,root/"changed.wav");REQUIRE_FALSE(changed.ok);REQUIRE(changed.code=="resource-hash-mismatch");
    std::filesystem::remove_all(root);
}

TEST_CASE("Q3 mda Delay selected class controller and worker match direct licensed reference", "[production][vst3-effect][reference]") {
    for(auto block:{128u,512u}) {
        std::string error;host::EffectWorkerOptions options;options.executable=NOD_VST3_WORKER;options.plugin=NOD_MDA_EFFECT;
        options.className="mda Delay";options.declaredTailSeconds=1;
        auto worker=host::makeWorkerEffect(options);effects::Config config;config.type="vst3";
        REQUIRE(worker->prepare(48000,block,config,error));REQUIRE(worker->tailSeconds()==1);
        const auto* schema=worker->parameterSchema();REQUIRE(schema->parameters.size()==7);
        REQUIRE(schema->parameters[1].title=="L Delay");REQUIRE(schema->parameters[2].title=="R Delay");
        host::Vst3Plugin reference;REQUIRE(reference.open(NOD_MDA_EFFECT,error,"mda Delay"));REQUIRE(reference.activate(48000,block,error));
        std::vector<float> left(block),right(block),expectedLeft(block),expectedRight(block);double energy=0;
        for(std::uint32_t begin=0;begin<48000;begin+=block) {
            const auto frames=std::min(block,48000-begin);std::fill(left.begin(),left.end(),0);std::fill(right.begin(),right.end(),0);
            if(!begin) {left[0]=1;right[0]=.5;}
            REQUIRE(reference.process(expectedLeft.data(),expectedRight.data(),frames,{},error,left.data(),right.data()));
            worker->process({left.data(),right.data(),frames});REQUIRE_FALSE(worker->errorReason());
            for(std::uint32_t frame=0;frame<frames;++frame) {REQUIRE(left[frame]==expectedLeft[frame]);REQUIRE(right[frame]==expectedRight[frame]);if(begin+frame>100) energy+=left[frame]*left[frame]+right[frame]*right[frame];}
        }
        REQUIRE(energy>0);worker->reset();REQUIRE(worker->tailSeconds()==1);
    }
}

TEST_CASE("Q5 VST3 instrument states persist relocate and invalidate frozen dry sound", "[production][vst3-state][song]") {
    const auto root=std::filesystem::temp_directory_path()/"nod-instrument-state-acceptance";
    std::filesystem::remove_all(root);std::filesystem::create_directories(root);
    host::Vst3Plugin plugin;std::string error;REQUIRE(plugin.open(NOD_VST3_PLUGIN,error));REQUIRE(plugin.activate(8000,128,error));
    std::array<float,128> left{},right{};
    const auto save=[&](double value,const char* name) {
        std::vector<host::Vst3ParamPoint> points{{0,0,value}};REQUIRE(plugin.process(left.data(),right.data(),128,{},error,nullptr,nullptr,&points));
        std::vector<char> state;REQUIRE(plugin.saveState(state,error));std::ofstream output(root/name,std::ios::binary);output.write(state.data(),state.size());REQUIRE(output.good());
    };
    save(.125,"quiet.bin");save(.5,"loud.bin");
    auto document=song::createSong({120,4,4,480,1});document.baseDirectory=root;
    song::Track track;track.id="lead";track.panMode="balance";track.gainMode="multiply";
    song::Clip clip;clip.id="phrase";clip.length=1920;clip.notes={{"note",0,240,69,100,0}};track.clips={clip};document.tracks={track};
    document.resources={{"plugin",NOD_VST3_PLUGIN,song::hashFile(NOD_VST3_PLUGIN),"vst3-plugin"},
        {"quiet","quiet.bin",song::hashFile(root/"quiet.bin"),"plugin-state"},{"loud","loud.bin",song::hashFile(root/"loud.bin"),"plugin-state"}};
    auto batch=persist::Json::parse(R"({"schemaVersion":1,"commands":[{"op":"set-vst3-instrument","track":"lead","pluginResource":"plugin","className":"NodSynth","stateResource":"quiet"},{"op":"collect-resources"}]})",error);
    REQUIRE(batch);const auto applied=song::applyCommands(document,*batch);INFO(applied.message);REQUIRE(applied.ok);
    REQUIRE(document.instruments.back().stateResourceId=="quiet");REQUIRE(song::undoSong(document).ok);REQUIRE(document.tracks[0].instrumentId.empty());REQUIRE(song::redoSong(document).ok);
    song::SongRenderOptions options;options.sampleRate=8000;options.tailSeconds=.2;options.freezeExternal=true;options.cacheDirectory=root/"cache";
    options.tools.push_back({"vst3",NOD_VST3_WORKER,10000});
    const auto quiet=song::renderSong(document,options,root/"quiet.wav");INFO(quiet.message);REQUIRE(quiet.ok);
    document.instruments.back().stateResourceId="loud";
    const auto loud=song::renderSong(document,options,root/"loud.wav");INFO(loud.message);REQUIRE(loud.ok);REQUIRE_FALSE(loud.cacheHit);
    REQUIRE(loud.mixHash!=quiet.mixHash);REQUIRE(loud.peak==Catch::Approx(quiet.peak*4).margin(1e-6));
    REQUIRE(song::saveSong(root/"moved/song.json",document,error));auto moved=song::loadSong(root/"moved/song.json",error);REQUIRE(moved);
    options.useCache=false;const auto relocated=song::renderSong(*moved,options,root/"relocated.wav");REQUIRE(relocated.ok);
    REQUIRE(song::hashFile(root/"loud.wav")==song::hashFile(root/"relocated.wav"));
    std::filesystem::remove_all(root);
}

TEST_CASE("Q0 isolated plugin inspection discovers instrument classes and stable parameter availability", "[production][plugin-inspection]") {
    host::EffectWorkerOptions options;options.executable=NOD_VST3_WORKER;options.plugin=NOD_MDA_EFFECT;options.className="mda JX10";
    host::PluginInspection result;std::string error;
    REQUIRE_FALSE(host::inspectPlugin(options,result,error));REQUIRE(error.find("input")!=std::string::npos);
    options.inspectionOnly=true;REQUIRE(host::inspectPlugin(options,result,error));REQUIRE(result.info.name=="mda JX10");
    REQUIRE(result.classes.size()>10);REQUIRE(result.workerPrivateBytes>0);
    REQUIRE(std::find(result.classes.begin(),result.classes.end(),"mda Delay")!=result.classes.end());
    const auto frequency=std::find_if(result.parameters.begin(),result.parameters.end(),[](const auto& p){return p.title=="VCF Freq";});
    REQUIRE(frequency!=result.parameters.end());REQUIRE(frequency->id=="vst3:6");REQUIRE(frequency->automatable);
    REQUIRE(std::any_of(result.parameters.begin(),result.parameters.end(),[](const auto& p){return !p.automatable;}));
}
