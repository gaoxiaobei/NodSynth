#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <nodsynth/song/Mixer.h>
#include <nodsynth/song/SongRenderer.h>
#include <nodsynth/nodes/BuiltinNodes.h>
#include <nodsynth/persist/ProjectFile.h>
#include <nodsynth/runtime/WavFile.h>
#include <nodsynth/nodes/GlobalEffect.h>
#include <cmath>
#include <numbers>

using namespace nodsynth;
namespace {
std::vector<float> impulse(const effects::Config& config,std::uint32_t rate,std::uint32_t block,double seconds=1) {
    std::string error;auto effect=effects::makeEffect(config.type);REQUIRE(effect);REQUIRE(effect->prepare(rate,block,config,error));
    REQUIRE(effect->committedBytes()<=effects::stateBudget(config.type,rate));
    const auto frames=static_cast<std::uint32_t>(rate*seconds);std::vector<float> output(frames*2),left(block),right(block);
    for(std::uint32_t start=0;start<frames;start+=block) {
        const auto count=std::min(block,frames-start);std::fill(left.begin(),left.end(),0);std::fill(right.begin(),right.end(),0);
        if(!start) left[0]=1;
        effect->process({left.data(),right.data(),count});
        for(std::uint32_t frame=0;frame<count;++frame) {output[(start+frame)*2]=left[frame];output[(start+frame)*2+1]=right[frame];}
    }
    return output;
}
TEST_CASE("Q4 EQ matches calibrated center and cutoff responses", "[production][effect][eq]") {
    for(auto rate:{44100u,48000u,96000u}) for(int mode:{0,1,2,3,4}) {
        effects::Config config;config.type="eq";config.parameters={{"mode",mode},{"frequency",1000},{"gainDb",12},{"q",.7071067811865476}};
        auto effect=effects::makeEffect("eq");std::string error;REQUIRE(effect->prepare(rate,128,config,error));
        std::vector<float> left(128),right(128);double inputEnergy=0,outputEnergy=0;
        for(std::uint32_t begin=0;begin<rate;++begin) {
            left[0]=static_cast<float>(.1*std::sin(2*std::numbers::pi*1000*begin/rate));right[0]=0;
            const double input=left[0];effect->process({left.data(),right.data(),1});
            REQUIRE(right[0]==0);REQUIRE(std::isfinite(left[0]));
            if(begin>rate/2) {inputEnergy+=input*input;outputEnergy+=left[0]*left[0];}
        }
        const double db=10*std::log10(outputEnergy/inputEnergy);
        REQUIRE(db==Catch::Approx(mode==0?12:mode==1 || mode==2?-3.01029995664:6).margin(.02));
    }
}
TEST_CASE("Q4 compressor static knee and attack release follow analytic curves", "[production][effect][compressor]") {
    for(auto rate:{44100u,48000u,96000u}) for(double inputDb:{-30.,-20.,-18.,-16.,-6.}) {
        effects::Config config;config.type="compressor";config.parameters={{"thresholdDb",-18},{"ratio",4},{"kneeDb",6},{"attackMs",10},{"releaseMs",100},{"detectorHighpass",0}};
        std::string error;auto effect=effects::makeEffect("compressor");REQUIRE(effect->prepare(rate,128,config,error));
        const float input=static_cast<float>(std::pow(10.,inputDb/20));float left=input,right=input*.5f;
        const double over=inputDb+18,expected=over>=3?-.75*over:over>-3?-.75*(over+3)*(over+3)/12:0;
        for(std::uint32_t frame=0;frame<rate/2;++frame) {
            left=input;right=input*.5f;effect->process({&left,&right,1});
            if(frame==rate/100-1) REQUIRE(20*std::log10(left/input)==Catch::Approx(expected*(1-std::exp(-1.))).margin(.001));
        }
        REQUIRE(20*std::log10(left/input)==Catch::Approx(expected).margin(.001));
        REQUIRE(right==Catch::Approx(left*.5).margin(1e-7));
        // Quiet program material releases the detector gain on its declared time constant.
        for(std::uint32_t frame=0;frame<rate/10;++frame) {left=.001f;right=0;effect->process({&left,&right,1});}
        REQUIRE(20*std::log10(left/.001)==Catch::Approx(expected*std::exp(-1.)).margin(.001));
    }
}
TEST_CASE("Q4 external sidechain is independent sample aligned and rejects cycles", "[production][mixer][sidechain]") {
    auto document=song::createSong({120,4,4,480,1});
    song::Track kick;kick.id="kick";kick.output="detector";kick.panMode="balance";kick.gainMode="multiply";
    song::Track bass;bass.id="bass";bass.panMode="balance";bass.gainMode="multiply";
    effects::Config compressor;compressor.id="duck";compressor.type="compressor";compressor.sidechain="kick";
    compressor.parameters={{"thresholdDb",-18},{"ratio",4},{"attackMs",1},{"detectorHighpass",0},{"kneeDb",0}};bass.inserts={compressor};
    document.tracks={kick,bass};song::Bus detector;detector.id="detector";detector.mute=true;document.buses={detector};
    std::string error;
    const auto render=[&](std::uint32_t block) {
        song::SongMixer mixer;REQUIRE(mixer.prepare(document,48000,block,error));
        std::vector<float> k(block),b(block,.1f),output(block*2),result(2048);
        for(std::uint32_t begin=0;begin<1024;begin+=block) {
            const auto frames=std::min(block,1024-begin);
            for(std::uint32_t frame=0;frame<frames;++frame) k[frame]=begin+frame>=257?1:0;
            mixer.setTrackInput(0,k.data(),k.data(),frames);mixer.setTrackInput(1,b.data(),b.data(),frames);mixer.process(begin,frames,output.data());
            std::copy_n(output.begin(),frames*2,result.begin()+begin*2);
        }return result;
    };
    const auto reference=render(128);REQUIRE(reference[256*2]==.1f);REQUIRE(reference[257*2]<.1f);
    REQUIRE(render(64)==reference);REQUIRE(render(512)==reference);
    effects::Config latency;latency.id="lookahead";latency.type="limiter";latency.bypass=true;
    document.tracks[0].inserts={latency};const auto aligned=render(128);
    for(std::size_t frame=0;frame<265;++frame) {REQUIRE(aligned[frame*2]==0);REQUIRE(aligned[frame*2+1]==0);}
    REQUIRE(std::equal(reference.begin(),reference.end()-530,aligned.begin()+530));
    REQUIRE(render(64)==aligned);REQUIRE(render(512)==aligned);
    document.tracks[0].inserts={compressor};document.tracks[0].inserts[0].sidechain="bass";
    REQUIRE_FALSE(song::validateRouting(document,error));REQUIRE(error.find("cycle")!=std::string::npos);
}
}
TEST_CASE("Q3 musical delay impulse stereo ping pong and bypass have explicit timing", "[production][effect][mixer][music-delay]") {
    effects::Config config;config.id="echo";config.type="delay";config.parameters={{"timeMs",10},{"feedback",0},{"wet",1}};
    for(auto rate:{44100u,48000u,96000u}) {
        const auto reference=impulse(config,rate,128,.05);
        REQUIRE(reference[(rate/100)*2]==1);
        for(std::size_t frame=0;frame<reference.size()/2;++frame) {
            REQUIRE(reference[frame*2+1]==0);
            if(frame!=rate/100) REQUIRE(reference[frame*2]==0);
        }
        REQUIRE(impulse(config,rate,64,.05)==reference);REQUIRE(impulse(config,rate,512,.05)==reference);
    }
    config.parameters["feedback"]=.5;config.parameters["pingPong"]=1;
    const auto ping=impulse(config,48000,128,.05);
    double rightEnergy=0;for(std::size_t i=960;i<1440;++i) rightEnergy+=ping[i*2+1]*ping[i*2+1];REQUIRE(rightEnergy>.01);
    config.bypass=true;const auto bypass=impulse(config,48000,128,.05);REQUIRE(bypass[0]==1);
    for(std::size_t i=1;i<bypass.size();++i) REQUIRE(bypass[i]==0);
    config.bypass=false;config.parameters["syncBeats"]=.5;config.parameters["feedback"]=0;config.parameters["pingPong"]=0;
    REQUIRE(impulse(config,48000,128,.3)[24000]==1);
}
TEST_CASE("Q3 stereo FDN reverb is deterministic decays and remains finite at extreme settings", "[production][effect][mixer][reverb]") {
    effects::Config config;config.id="room";config.type="reverb";config.parameters={{"preDelayMs",20},{"decay",.2},{"wet",1}};
    for(auto rate:{44100u,48000u,96000u}) {
        const auto reference=impulse(config,rate,128,.7);
        REQUIRE(impulse(config,rate,64,.7)==reference);REQUIRE(impulse(config,rate,512,.7)==reference);
        double early=0,late=0,stereoDifference=0;
        for(std::size_t frame=0;frame<reference.size()/2;++frame) {
            const auto l=reference[frame*2],r=reference[frame*2+1];REQUIRE(std::isfinite(l));REQUIRE(std::isfinite(r));
            if(frame<rate*.05) {REQUIRE(l==0);REQUIRE(r==0);}
            if(frame<rate*.3) early+=l*l+r*r;else late+=l*l+r*r;
            stereoDifference+=std::fabs(l-r);
        }
        REQUIRE(early>0);REQUIRE(late<early*.001);REQUIRE(stereoDifference>0);
    }
    config.parameters={{"decay",15},{"damping",20000},{"lowCut",20},{"highCut",20000},{"wet",1}};
    for(float sample:impulse(config,48000,128,1)) REQUIRE(std::isfinite(sample));
}
TEST_CASE("Q3 routing preserves post mute return tails and pre fader sends explicitly", "[production][mixer][routing]") {
    auto song=song::createSong({120,4,4,480,1});song::Track track;track.id="source";track.panMode="balance";track.gainMode="multiply";
    track.mute={{1,1920}};track.muteFadeMs=0;track.sends={{"echo",1,false}};song.tracks.push_back(track);
    song::Bus bus;bus.id="echo";bus.isReturn=true;effects::Config echo;echo.id="delay";echo.type="delay";
    echo.parameters={{"timeMs",10},{"feedback",0},{"wet",1}};bus.inserts={echo};song.buses.push_back(bus);
    song::SongMixer mixer;std::string error;REQUIRE(mixer.prepare(song,48000,128,error));
    std::vector<float> left(128),right(128),master(256);left[0]=1;right[0]=.5;
    mixer.setTrackInput(0,left.data(),right.data(),128);mixer.process(0,128,master.data());REQUIRE(master[0]==1);
    std::fill(left.begin(),left.end(),1);std::fill(right.begin(),right.end(),1);
    for(int block=1;block<4;++block) {mixer.setTrackInput(0,left.data(),right.data(),128);mixer.process(block*128,128,master.data());}
    REQUIRE(master[96*2]==1);REQUIRE(master[96*2+1]==.5); // Echo at frame 480 despite source audio mute.
    song.buses[0].mute=true;REQUIRE(mixer.prepare(song,48000,128,error));
    left.assign(128,1);right.assign(128,1);mixer.setTrackInput(0,left.data(),right.data(),128);mixer.process(128,128,master.data());
    for(float sample:master) REQUIRE(sample==0);
    song.buses[0].mute=false;song.tracks[0].sends[0].preFader=true;REQUIRE(mixer.prepare(song,48000,128,error));
    for(int block=0;block<5;++block) {mixer.setTrackInput(0,left.data(),right.data(),128);mixer.process(block*128,128,master.data());}
    REQUIRE(master[0]==1);
    song.buses[0].sends={{"echo",1,false}};REQUIRE_FALSE(song::validateRouting(song,error));REQUIRE(error.find("cycle")!=std::string::npos);
}
TEST_CASE("Q3 route edits persist undo reject invalid graphs and remix shared returns from dry cache", "[production][mixer][song][routed-render]") {
    const auto root=std::filesystem::temp_directory_path()/"nod-mixer-render";std::filesystem::remove_all(root);std::filesystem::create_directories(root);
    auto document=song::createSong({120,4,4,480,1});document.baseDirectory=root;
    song::Track track;track.id="lead";track.panMode="balance";track.gainMode="multiply";
    song::Clip clip;clip.id="phrase";clip.length=1920;clip.notes={{"note",0,240,69,100,0}};track.clips.push_back(clip);document.tracks.push_back(track);
    std::string error;auto patch=persist::projectFromGraph(nodes::unisonPatch());REQUIRE(persist::saveProject(root/"patch.json",patch,error));
    REQUIRE(song::bindPatch(document,"lead","patch.json",root/"patch.json",error));
    const auto batch=persist::Json::parse(R"({"schemaVersion":1,"commands":[{"op":"create-bus","id":"room","return":true,"inserts":[{"id":"reverb","type":"reverb","parameters":{"wet":1,"decay":0.2}}]},{"op":"set-sends","target":"lead","sends":[{"target":"room","gain":0.2,"position":"post-fader"}]}]})",error);
    REQUIRE(batch);REQUIRE(song::applyCommands(document,*batch).ok);REQUIRE(document.buses.size()==1);
    REQUIRE(song::undoSong(document).ok);REQUIRE(document.buses.empty());REQUIRE(song::redoSong(document).ok);
    REQUIRE(song::saveSong(root/"song.json",document,error));auto loaded=song::loadSong(root/"song.json",error);REQUIRE(loaded);
    song::SongRenderOptions options;options.tailSeconds=.7;options.cacheDirectory=root/"cache";
    const auto cold=song::renderSong(*loaded,options,root/"cold.wav",root/"stems");REQUIRE(cold.ok);REQUIRE(cold.stems.size()==3);
    REQUIRE(std::filesystem::exists(root/"stems/dry/lead.wav"));REQUIRE(std::filesystem::exists(root/"stems/returns/room.wav"));
    REQUIRE(song::renderSong(*loaded,options,root/"warm.wav").ok);REQUIRE(song::hashFile(root/"cold.wav")==song::hashFile(root/"warm.wav"));
    loaded->buses[0].inserts[0].parameters["decay"]=.3;
    const auto remix=song::renderSong(*loaded,options,root/"remix.wav");REQUIRE(remix.ok);REQUIRE(remix.cacheHit);
    options.useCache=false;REQUIRE(song::renderSong(*loaded,options,root/"fresh.wav").ok);
    REQUIRE(song::hashFile(root/"remix.wav")==song::hashFile(root/"fresh.wav"));
    std::filesystem::remove_all(root);
}
TEST_CASE("Q3 Global graph nodes and insert effects share sample exact DSP", "[production][mixer][effect]") {
    constexpr std::uint32_t frames=128;
    for(const auto* type:{"delay","reverb"}) {
        const auto* schema=effects::findSchema(type);REQUIRE(schema);
        std::vector<float> input(frames*2),output(frames*2),parameters(frames*schema->parameters.size()),left(frames),right(frames);
        effects::Config config;config.type=type;
        for(std::size_t index=0;index<schema->parameters.size();++index) {
            const auto& p=schema->parameters[index];const auto value=p.id=="wet"?1:p.defaultValue;
            config.parameters[p.id]=value;std::fill_n(parameters.data()+index*frames,frames,static_cast<float>(value));
        }
        auto node=nodes::makeGlobalEffect(type);runtime::NodeBinding binding;binding.sampleRate=48000;binding.maxFrames=frames;binding.voiceCount=1;
        binding.inputs={{{"audio-in"},input.data(),2,frames,frames*2,frames}};binding.outputs={{{"audio-out"},output.data(),2,frames,frames*2,frames}};
        binding.parameters={parameters.data(),static_cast<std::uint32_t>(schema->parameters.size()),frames};node->bind(binding);
        auto effect=effects::makeEffect(type);std::string error;REQUIRE(effect->prepare(48000,frames,config,error));
        // Float parameters are the graph contract; prepare the insert at the same precision.
        for(std::size_t index=0;index<schema->parameters.size();++index) config.parameters[schema->parameters[index].id]=parameters[index*frames];
        REQUIRE(effect->prepare(48000,frames,config,error));
        for(int block=0;block<200;++block) {
            std::fill(input.begin(),input.end(),0);std::fill(left.begin(),left.end(),0);std::fill(right.begin(),right.end(),0);
            if(!block) {input[0]=left[0]=1;input[frames]=right[0]=.5f;}
            node->process(0,frames);effect->process({left.data(),right.data(),frames});
            for(std::uint32_t frame=0;frame<frames;++frame) {REQUIRE(output[frame]==left[frame]);REQUIRE(output[frames+frame]==right[frame]);}
        }
    }
}
TEST_CASE("Q3 effect automation keeps physical bases persists and repeats across block sizes", "[production][mixer][effect]") {
    auto document=song::createSong({120,4,4,480,1});song::Track track;track.id="audio";track.panMode="balance";track.gainMode="multiply";
    effects::Config delay;delay.id="echo";delay.type="delay";delay.parameters={{"timeMs",10},{"feedback",0},{"wet",0}};track.inserts={delay};document.tracks.push_back(track);
    std::string error;const auto batch=persist::Json::parse(R"({"schemaVersion":1,"commands":[{"op":"set-effect-automation","target":"audio","effect":"echo","parameter":"wet","interpolation":"step","points":[{"tick":2,"value":1},{"tick":10,"value":0}]}]})",error);
    REQUIRE(batch);REQUIRE(song::applyCommands(document,*batch).ok);
    const auto restored=song::songFromJson(song::toJson(document),error);REQUIRE(restored);
    REQUIRE(restored->tracks[0].inserts[0].automation[0].points.size()==2);
    const auto render=[&](std::uint32_t block) {
        song::SongMixer mixer;REQUIRE(mixer.prepare(*restored,48000,block,error));
        std::vector<float> result(2000),left(block,1),right(block,1),out(block*2);
        for(std::uint32_t begin=0;begin<1000;begin+=block) {
            const auto frames=std::min(block,1000-begin);mixer.setTrackInput(0,left.data(),right.data(),frames);mixer.process(begin,frames,out.data());
            std::copy_n(out.begin(),frames*2,result.begin()+begin*2);
        }
        return result;
    };
    const auto reference=render(128);REQUIRE(reference[0]==1);REQUIRE(reference[200]==0);REQUIRE(reference[960]==1);REQUIRE(reference[1000]==1);
    REQUIRE(render(64)==reference);REQUIRE(render(512)==reference);
    REQUIRE(song::undoSong(document).ok);REQUIRE(document.tracks[0].inserts[0].automation.empty());REQUIRE(song::redoSong(document).ok);
}
