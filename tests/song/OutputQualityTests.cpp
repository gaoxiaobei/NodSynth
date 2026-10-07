#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <nodsynth/song/AudioAnalysis.h>
#include <nodsynth/song/AudioExport.h>
#include <nodsynth/song/SongRenderer.h>
#include <cmath>
#include <numbers>
#include <fstream>
#include <nodsynth/song/Mixer.h>
#include <nodsynth/nodes/BuiltinNodes.h>
#include <nodsynth/persist/ProjectFile.h>

using namespace nodsynth;
namespace {
struct Fixture {
    std::filesystem::path root;
    Fixture(const char* name):root(std::filesystem::temp_directory_path()/name) {std::filesystem::remove_all(root);std::filesystem::create_directories(root);}
    ~Fixture() {std::filesystem::remove_all(root);}
};
runtime::WavData tone(std::uint32_t rate,std::uint32_t channels,double hz,double amplitude,double seconds) {
    runtime::WavData audio;audio.sampleRate=rate;audio.channels=channels;audio.interleaved.resize(static_cast<std::size_t>(rate*seconds)*channels);
    for(std::size_t frame=0;frame<audio.interleaved.size()/channels;++frame) for(std::uint32_t channel=0;channel<channels;++channel)
        audio.interleaved[frame*channels+channel]=static_cast<float>(amplitude*std::sin(2*std::numbers::pi*hz*frame/rate));
    return audio;
}
TEST_CASE("Q4 lookahead limiter bounds calibrated true peaks retains bypass latency and block invariance", "[production][output][limiter]") {
    Fixture fixture("nod-output-limiter");std::string error;
    for(auto rate:{44100u,48000u,96000u}) for(const auto* quality:{"standard","high"}) {
        effects::Config config;config.type="limiter";config.quality=quality;config.parameters={{"ceilingDb",-1},{"lookaheadMs",5},{"releaseMs",50}};
        auto input=tone(rate,2,rate*.25,2,.12);
        for(std::size_t frame=0;frame<input.interleaved.size()/2;++frame) {
            input.interleaved[frame*2]=static_cast<float>(2*std::sin(std::numbers::pi*frame*.5+std::numbers::pi/4));
            input.interleaved[frame*2+1]=frame==300?-4:input.interleaved[frame*2]*.3f;
        }
        const auto render=[&](std::uint32_t block,bool bypass) {
            auto limiter=effects::makeEffect("limiter");config.bypass=bypass;REQUIRE(limiter->prepare(rate,block,config,error));
            REQUIRE(limiter->committedBytes()<=effects::stateBudget("limiter",rate));
            const auto latency=limiter->latencySamples();REQUIRE(latency==static_cast<std::uint32_t>(std::ceil(rate*.005))+25);
            const auto frames=input.interleaved.size()/2+latency;std::vector<float> left(block),right(block),result(frames*2);
            for(std::size_t begin=0;begin<frames;begin+=block) {
                const auto count=static_cast<std::uint32_t>(std::min<std::size_t>(block,frames-begin));
                for(std::uint32_t frame=0;frame<count;++frame) {
                    left[frame]=(begin+frame)*2<input.interleaved.size()?input.interleaved[(begin+frame)*2]:0;
                    right[frame]=(begin+frame)*2<input.interleaved.size()?input.interleaved[(begin+frame)*2+1]:0;
                }
                limiter->process({left.data(),right.data(),count});
                for(std::uint32_t frame=0;frame<count;++frame) {result[(begin+frame)*2]=left[frame];result[(begin+frame)*2+1]=right[frame];}
            }
            for(std::size_t frame=0;frame<latency;++frame) {REQUIRE(result[frame*2]==0);REQUIRE(result[frame*2+1]==0);}
            if(bypass) REQUIRE(std::equal(input.interleaved.begin(),input.interleaved.end(),result.begin()+latency*2));
            return result;
        };
        auto output=input;output.interleaved=render(128,false);REQUIRE(render(64,false)==output.interleaved);REQUIRE(render(512,false)==output.interleaved);
        REQUIRE(runtime::writeWav(fixture.root/"limited.wav",output,error));const auto report=song::analyzeWav(fixture.root/"limited.wav");
        REQUIRE(report.truePeakDbtp);REQUIRE(*report.truePeakDbtp<=-1+.001);render(128,true);
    }
}
TEST_CASE("Q4 saturation oversampling suppresses folded harmonics and preserves stereo bypass timing", "[production][output][saturation]") {
    constexpr std::uint32_t count=8192;constexpr double fundamental=1400.;
    std::string error;
    const auto amplitude=[&](const auto& audio,double bin) {
        double sine=0,cosine=0;for(std::size_t i=0;i<count;++i) {const auto phase=2*std::numbers::pi*bin*i/count;sine+=audio[i]*std::sin(phase);cosine+=audio[i]*std::cos(phase);}
        return std::hypot(sine,cosine)*2/count;
    };
    std::vector<float> naive(count);for(std::size_t i=0;i<count;++i) naive[i]=static_cast<float>(std::tanh(8*std::sin(2*std::numbers::pi*fundamental*i/count)));
    for(const auto* quality:{"standard","high"}) {
        effects::Config config;config.type="saturation";config.quality=quality;config.parameters={{"driveDb",20*std::log10(8.)},{"outputDb",0}};
        const auto render=[&](std::uint32_t block,bool bypass) {
            auto effect=effects::makeEffect("saturation");config.bypass=bypass;REQUIRE(effect->prepare(48000,block,config,error));
            REQUIRE(effect->latencySamples()==32);REQUIRE(effect->committedBytes()<=effects::stateBudget("saturation",48000));
            std::vector<float> left(block),right(block),result(count);constexpr std::uint32_t skip=256;
            for(std::uint32_t begin=0;begin<count+skip;begin+=block) {
                const auto frames=std::min(block,count+skip-begin);
                for(std::uint32_t frame=0;frame<frames;++frame) {left[frame]=static_cast<float>(std::sin(2*std::numbers::pi*fundamental*(begin+frame)/count));right[frame]=0;}
                effect->process({left.data(),right.data(),frames});
                for(std::uint32_t frame=0;frame<frames;++frame) {REQUIRE(right[frame]==0);REQUIRE(std::isfinite(left[frame]));if(begin+frame>=skip) result[begin+frame-skip]=left[frame];}
            }
            return result;
        };
        const auto audio=render(128,false);REQUIRE(render(64,false)==audio);REQUIRE(render(512,false)==audio);
        const auto alias=amplitude(audio,count-5*fundamental);const auto reference=amplitude(naive,count-5*fundamental);
        INFO("quality="<<quality<<" alias="<<alias<<" naive="<<reference);
        REQUIRE(alias<reference*.1);REQUIRE(amplitude(audio,fundamental)>.5);
        const auto bypass=render(128,true);
        for(std::size_t i=0;i<count;++i) REQUIRE(bypass[i]==static_cast<float>(std::sin(2*std::numbers::pi*fundamental*(i+256-32)/count)));
    }
}

TEST_CASE("Q4 limiter adversarial signals respect sample timed changing ceilings", "[production][output][limiter][ceiling-automation]") {
    Fixture fixture("nod-output-ceiling");std::string error;
    for(auto rate:{44100u,48000u,96000u}) for(const auto* quality:{"standard","high"}) for(int signal=0;signal<6;++signal) {
        const auto count=rate/4,change=count/2;
        effects::Config config;config.type="limiter";config.quality=quality;config.parameters={{"ceilingDb",-1},{"releaseMs",5},{"lookaheadMs",1}};
        auto effect=effects::makeEffect("limiter");REQUIRE(effect->prepare(rate,128,config,error));const auto latency=effect->latencySamples();
        runtime::WavData audio;audio.sampleRate=rate;audio.channels=2;audio.interleaved.resize((count+latency)*2);
        std::array<float,128> left{},right{};std::uint32_t seed=1979;
        for(std::uint32_t begin=0;begin<count+latency;begin+=128) {
            const auto frames=std::min(128u,count+latency-begin);
            for(std::uint32_t frame=0;frame<frames;++frame) {
                const auto sample=begin+frame;seed=seed*1664525u+1013904223u;double value=0;
                if(sample<count) switch(signal) {
                    case 0:value=3;break;
                    case 1:value=(sample%2?3:-3);break;
                    case 2:value=6*std::sin(2*std::numbers::pi*(.02*sample+.42*sample*sample/(2*count)));break;
                    case 3:value=(static_cast<double>(seed)/UINT32_MAX-.5)*12;break;
                    case 4:value=sample%251==0?16:0;break;
                    default:value=2*(std::sin(.71*sample)+std::sin(1.13*sample)+std::sin(2.8*sample));break;
                }
                left[frame]=static_cast<float>(value);right[frame]=static_cast<float>(-.37*value);
            }
            const effects::ParameterEvent event{change>=begin?change-begin:0,0,-6};
            const bool changes=begin<=change && change<begin+frames;
            effect->process({left.data(),right.data(),frames,nullptr,nullptr,nullptr,changes?std::span<const effects::ParameterEvent>{&event,1}:std::span<const effects::ParameterEvent>{}});
            for(std::uint32_t frame=0;frame<frames;++frame) {REQUIRE(std::isfinite(left[frame]));audio.interleaved[(begin+frame)*2]=left[frame];audio.interleaved[(begin+frame)*2+1]=right[frame];}
        }
        REQUIRE(runtime::writeWav(fixture.root/"whole.wav",audio,error));const auto whole=song::analyzeWav(fixture.root/"whole.wav");
        REQUIRE(whole.truePeakDbtp);REQUIRE(*whole.truePeakDbtp<=-1+.001);
        audio.interleaved.erase(audio.interleaved.begin(),audio.interleaved.begin()+(change+latency+64)*2);
        REQUIRE(runtime::writeWav(fixture.root/"after.wav",audio,error));const auto after=song::analyzeWav(fixture.root/"after.wav");
        REQUIRE(after.truePeakDbtp);INFO("signal="<<signal<<" rate="<<rate<<" quality="<<quality);REQUIRE(*after.truePeakDbtp<=-6+.001);
    }
}

TEST_CASE("Q4 compensated master tracks returns previews and PCM float master export repeat from cache", "[production][output][pdc]") {
    Fixture fixture("nod-output-pdc");std::string error;
    auto document=song::createSong({120,4,4,480,1});document.baseDirectory=fixture.root;
    song::Track track;track.id="lead";track.panMode="balance";track.gainMode="multiply";
    song::Clip clip;clip.id="phrase";clip.length=1920;clip.notes={{"note",0,240,69,100,0}};track.clips={clip};document.tracks={track};
    auto patch=persist::projectFromGraph(nodes::unisonPatch());REQUIRE(persist::saveProject(fixture.root/"patch.json",patch,error));
    REQUIRE(song::bindPatch(document,"lead","patch.json",fixture.root/"patch.json",error));
    effects::Config limiter;limiter.id="ceiling";limiter.type="limiter";limiter.bypass=true;limiter.parameters={{"lookaheadMs",5}};
    document.tracks[0].inserts={limiter};document.masterInserts={limiter};
    song::Bus bus;bus.id="parallel";bus.output="";bus.inserts={limiter,limiter};bus.inserts[1].id="second";
    document.buses={bus};document.tracks[0].sends={{"parallel",1,false}};
    song::SongRenderOptions options;options.tailSeconds=.1;options.cacheDirectory=fixture.root/"cache";
    const auto delayed=song::renderSong(document,options,fixture.root/"delayed.wav",fixture.root/"stems");REQUIRE(delayed.ok);REQUIRE(delayed.latencySamples==530);
    runtime::WavData master,dry,post;REQUIRE(runtime::readWav(fixture.root/"delayed.wav",master,error));
    REQUIRE(runtime::readWav(fixture.root/"stems/dry/lead.wav",dry,error));REQUIRE(runtime::readWav(fixture.root/"stems/lead.wav",post,error));
    REQUIRE(master.interleaved==dry.interleaved);REQUIRE(post.interleaved==dry.interleaved);
    runtime::WavData busAudio;REQUIRE(runtime::readWav(fixture.root/"stems/buses/parallel.wav",busAudio,error));REQUIRE(busAudio.interleaved==dry.interleaved);
    bool foundBus=false,foundTrack=false;
    for(const auto& stem:delayed.stems) {
        if(stem.role=="bus") {foundBus=true;REQUIRE(stem.latencySamples==795);}
        if(stem.trackId=="lead" && stem.role=="post-track") {foundTrack=true;REQUIRE(stem.latencySamples==265);}
    }
    REQUIRE(foundBus);REQUIRE(foundTrack);
    const auto warm=song::renderSong(document,options,fixture.root/"warm.wav");REQUIRE(warm.ok);REQUIRE(song::hashFile(fixture.root/"warm.wav")==song::hashFile(fixture.root/"delayed.wav"));
    options.format="pcm24";options.dither="tpdf";
    const auto pcm=song::renderSong(document,options,fixture.root/"pcm.wav");REQUIRE(pcm.ok);
    REQUIRE(pcm.floatMasterHash==song::hashFile(fixture.root/"delayed.wav"));REQUIRE(std::filesystem::exists(fixture.root/"pcm.float.wav"));
    options.previewOutput=fixture.root/"alias.float.wav";
    REQUIRE_FALSE(song::renderSong(document,options,fixture.root/"alias.wav").ok);
    options.previewOutput.clear();options.format="float32";options.dither="none";options.useCache=false;
    options.previewStartTick=120;options.previewEndTick=360;
    REQUIRE(song::renderSong(document,options,fixture.root/"preview.wav").ok);
    runtime::WavData preview;REQUIRE(runtime::readWav(fixture.root/"preview.wav",preview,error));
    REQUIRE(std::equal(preview.interleaved.begin(),preview.interleaved.end(),master.interleaved.begin()+12000));
}
}
TEST_CASE("Q4 BS1770 mono stereo calibration silence and sample rates have distinct units", "[production][output][meter]") {
    Fixture f("nod-output-meter");std::string error;
    for(auto rate:{44100u,48000u,96000u}) {
        const auto mono=tone(rate,1,1000,.1,6),stereo=tone(rate,2,1000,.1,6);
        REQUIRE(runtime::writeWav(f.root/"mono.wav",mono,error));REQUIRE(runtime::writeWav(f.root/"stereo.wav",stereo,error));
        const auto a=song::analyzeWav(f.root/"mono.wav"),b=song::analyzeWav(f.root/"stereo.wav");
        REQUIRE(a.ok);REQUIRE(b.ok);REQUIRE(a.loudnessLufs);REQUIRE(b.loudnessLufs);
        REQUIRE(*a.loudnessLufs==Catch::Approx(-23).margin(.15));
        REQUIRE(*b.loudnessLufs-*a.loudnessLufs==Catch::Approx(10*std::log10(2)).margin(.11)); // Histogram bins are 0.1 LU.
        REQUIRE(a.truePeakDbtp);REQUIRE(*a.truePeakDbtp==Catch::Approx(-20).margin(.1));
        REQUIRE(a.shortTermMaxLufs);REQUIRE(*a.shortTermMaxLufs==Catch::Approx(-23).margin(.15));
        REQUIRE(a.loudnessRangeLu);REQUIRE(*a.loudnessRangeLu<.2);
    }
    auto silent=tone(48000,2,1000,0,6);REQUIRE(runtime::writeWav(f.root/"silent.wav",silent,error));
    const auto silence=song::analyzeWav(f.root/"silent.wav");REQUIRE(silence.ok);REQUIRE_FALSE(silence.loudnessLufs);REQUIRE_FALSE(silence.truePeakDbtp);REQUIRE_FALSE(silence.loudnessRangeLu);
    auto shortAudio=tone(48000,1,1000,.1,.1);REQUIRE(runtime::writeWav(f.root/"short.wav",shortAudio,error));
    const auto shortReport=song::analyzeWav(f.root/"short.wav");REQUIRE_FALSE(shortReport.loudnessLufs);REQUIRE_FALSE(shortReport.shortTermMaxLufs);REQUIRE(shortReport.truePeakDbtp);
}
TEST_CASE("Q4 true peak detects intersample overshoot above sample peak", "[production][output][meter]") {
    Fixture f("nod-output-truepeak");auto audio=tone(48000,1,12000,.9,.2);
    for(std::size_t i=0;i<audio.interleaved.size();++i) {
        const double fade=std::min({1.0,i/240.0,(audio.interleaved.size()-1-i)/240.0});
        audio.interleaved[i]=static_cast<float>(.9*fade*std::sin(2*std::numbers::pi*12000*i/48000+std::numbers::pi/4));
    }
    std::string error;REQUIRE(runtime::writeWav(f.root/"phase.wav",audio,error));const auto result=song::analyzeWav(f.root/"phase.wav");
    REQUIRE(result.ok);REQUIRE(result.truePeakDbtp);REQUIRE(result.samplePeakDb);
    REQUIRE(*result.truePeakDbtp>*result.samplePeakDb+2.5);
    REQUIRE(*result.truePeakDbtp==Catch::Approx(20*std::log10(.9)).margin(.25));
}
TEST_CASE("Q4 PCM24 quantization TPDF seed headroom and odd mono data round trip", "[production][output][pcm]") {
    Fixture f("nod-output-pcm");std::string error;double gain=0;
    auto audio=tone(48000,2,1000,.5,.2);
    for(auto bits:{16u,24u}) {
        const auto path=f.root/(std::to_string(bits)+".wav");
        REQUIRE(song::writePcm(path,audio,static_cast<std::uint16_t>(bits),false,"none",1979,gain,error));REQUIRE(gain==1);
        runtime::WavData decoded;REQUIRE(runtime::readWav(path,decoded,error));REQUIRE(decoded.bitsPerSample==bits);
        const double lsb=1.0/(std::uint64_t{1}<<(bits-1));
        for(std::size_t i=0;i<audio.interleaved.size();++i) REQUIRE(std::fabs(audio.interleaved[i]-decoded.interleaved[i])<=lsb*.501);
        REQUIRE(song::writePcm(path,audio,static_cast<std::uint16_t>(bits),false,"tpdf",1979,gain,error));const auto hash=song::hashFile(path);
        REQUIRE(song::writePcm(f.root/"repeat.wav",audio,static_cast<std::uint16_t>(bits),false,"tpdf",1979,gain,error));REQUIRE(song::hashFile(f.root/"repeat.wav")==hash);
        REQUIRE(song::writePcm(f.root/"other.wav",audio,static_cast<std::uint16_t>(bits),false,"tpdf",1980,gain,error));REQUIRE(song::hashFile(f.root/"other.wav")!=hash);
    }
    runtime::WavData odd;odd.sampleRate=48000;odd.channels=1;odd.interleaved={-.5f,0,.5f};
    REQUIRE(song::writePcm(f.root/"odd.wav",odd,24,false,"none",1,gain,error));runtime::WavData read;REQUIRE(runtime::readWav(f.root/"odd.wav",read,error));REQUIRE(read.interleaved==odd.interleaved);
    odd.interleaved={1.1f};REQUIRE_FALSE(song::writePcm(f.root/"over.wav",odd,24,false,"none",1,gain,error));
    REQUIRE(song::writePcm(f.root/"over.wav",odd,24,true,"tpdf",1,gain,error));REQUIRE(gain<1);
    odd.interleaved.assign(200000,0);REQUIRE(song::writePcm(f.root/"dither.wav",odd,16,false,"tpdf",1,gain,error));REQUIRE(runtime::readWav(f.root/"dither.wav",read,error));
    double mean=0,power=0;for(float value:read.interleaved) {mean+=value;power+=value*value;}
    const double lsb=1.0/32768;REQUIRE(std::fabs(mean/read.interleaved.size())<lsb*.01);REQUIRE(std::sqrt(power/read.interleaved.size())==Catch::Approx(lsb*.5).margin(lsb*.01));
}
