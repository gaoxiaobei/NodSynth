#include <nodsynth/song/Mixer.h>
#include <nodsynth/song/AudioExport.h>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <numbers>
#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#endif

int main(int argc,char** argv) {
    using namespace nodsynth;using persist::Json;
    auto measurements=Json::array();
    for(const auto* quality:{"standard","high"}) for(auto rate:{44100u,48000u,96000u}) for(auto block:{64u,128u,512u}) {
        auto document=song::createSong({128,4,4,480,2});
        effects::Config eq;eq.id="eq";eq.type="eq";eq.parameters={{"frequency",1000},{"gainDb",3}};
        effects::Config comp;comp.id="duck";comp.type="compressor";comp.sidechain="track0";
        for(int index=0;index<8;++index) {
            song::Track track;track.id="track"+std::to_string(index);track.panMode="balance";track.gainMode="multiply";track.gain=.25;
            track.inserts={eq};if(index) track.inserts.push_back(comp);track.sends={{"room",.2,false},{"echo",.1,false}};document.tracks.push_back(track);
        }
        for(const auto* type:{"reverb","delay"}) {
            song::Bus bus;bus.id=std::string(type)=="reverb"?"room":"echo";bus.isReturn=true;
            effects::Config effect;effect.id=type;effect.type=type;effect.parameters={{"wet",1}};bus.inserts={effect};document.buses.push_back(bus);
        }
        effects::Config limiter;limiter.id="ceiling";limiter.type="limiter";limiter.quality=quality;document.masterInserts={limiter};
        const auto started=std::chrono::steady_clock::now();song::SongMixer mixer;std::string error;
        if(!mixer.prepare(document,rate,block,error)) {std::cerr<<error;return 2;}
        const auto prepared=std::chrono::steady_clock::now();
        std::vector<float> left(block),right(block),output(block*2);std::uint64_t hash=14695981039346656037ull;double peak=0;
        for(std::uint32_t begin=0;begin<rate*3;begin+=block) {
            const auto frames=std::min(block,rate*3-begin);
            for(int track=0;track<8;++track) {
                for(std::uint32_t frame=0;frame<frames;++frame) {
                    const auto sample=begin+frame;const double t=static_cast<double>(sample)/rate;
                    const double trigger=std::fmod(t,.46875);
                    left[frame]=static_cast<float>(track?std::sin(2*std::numbers::pi*(110+track*37)*t)*.3:std::exp(-trigger*30)*std::sin(2*std::numbers::pi*60*t));
                    right[frame]=track?left[frame]*.8f:left[frame];
                }mixer.setTrackInput(track,left.data(),right.data(),frames);
            }
            mixer.process(begin,frames,output.data());
            for(std::uint32_t sample=0;sample<frames*2;++sample) {
                if(!std::isfinite(output[sample])) return 3;peak=std::max(peak,std::fabs(static_cast<double>(output[sample])));
                const auto* bytes=reinterpret_cast<const unsigned char*>(&output[sample]);for(std::size_t index=0;index<sizeof(float);++index) {hash^=bytes[index];hash*=1099511628211ull;}
            }
        }
        const double elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-prepared).count();
        auto item=Json::object();item.set("quality",Json::string(quality));item.set("sampleRate",Json::number(rate));item.set("blockSize",Json::number(block));
        item.set("prepareMs",Json::number(std::chrono::duration<double,std::milli>(prepared-started).count()));
        item.set("mixInputAndMeasurementMs",Json::number(elapsed));item.set("audioSeconds",Json::number(3));item.set("realtimeFactor",Json::number(3000/elapsed));
        item.set("mixerCommittedBytes",Json::number(static_cast<double>(mixer.committedBytes())));item.set("latencySamples",Json::number(mixer.latencySamples()));
        item.set("samplePeak",Json::number(peak));item.set("sampleHash",Json::string(std::to_string(hash)));
#if defined(_WIN32)
        PROCESS_MEMORY_COUNTERS memory{};memory.cb=sizeof(memory);
        if(GetProcessMemoryInfo(GetCurrentProcess(),&memory,sizeof(memory))) item.set("cumulativeProcessPeakWorkingSetBytes",Json::number(static_cast<double>(memory.PeakWorkingSetSize)));
#endif
        measurements.push(std::move(item));
    }
    for(std::size_t index=0;index<measurements.asArray().size();index+=3) for(std::size_t offset=1;offset<3;++offset)
        if(measurements.asArray()[index].find("sampleHash")->asString()!=measurements.asArray()[index+offset].find("sampleHash")->asString()) return 4;
    auto report=Json::object();report.set("scope",Json::string("8 generated stereo inputs, 8 EQ, 7 external-sidechain compressors, shared delay/reverb and master limiter; includes input generation and hashing; not a complete instrument project"));
    report.set("buildMode",Json::string(
#ifdef NDEBUG
        "Release"
#else
        "Debug"
#endif
    ));
    const auto* processor=std::getenv("PROCESSOR_IDENTIFIER");report.set("processor",processor?Json::string(processor):Json::null());
    report.set("measurements",std::move(measurements));report.set("auditionStatus",Json::string("unheard"));
    if(argc==2) {std::string error;if(!song::writeJsonAtomic(argv[1],report,error)) {std::cerr<<error;return 5;}}
    else std::cout<<report.dump()<<'\n';return 0;
}
