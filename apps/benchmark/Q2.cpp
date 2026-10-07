#include <nodsynth/compiler/GraphCompiler.h>
#include <nodsynth/nodes/BuiltinNodes.h>
#include <nodsynth/nodes/Unison.h>
#include <nodsynth/runtime/Engine.h>
#include <nodsynth/song/AudioExport.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#endif

int main(int argc,char** argv) {
    using namespace nodsynth;using persist::Json;
    const auto clock=[] {return std::chrono::steady_clock::now();};
    auto singleNodeMeasurements=Json::array();
    for(const auto rate:{44100u,48000u,96000u}) for(const auto block:{64u,128u,512u}) {
        const auto began=clock();
        std::vector<float> frequency(block,440),output(block*2),parameters(block*8);
        const std::array<float,8> defaults{8,18,.8f,0,.6f,.2f,123,440};
        for(std::uint32_t index=0;index<defaults.size();++index)
            std::fill_n(parameters.data()+index*block,block,defaults[index]);
        auto node=nodes::makeUnisonOscillator();runtime::NodeBinding binding;
        binding.sampleRate=rate;binding.maxFrames=block;binding.voiceCount=1;
        binding.inputs={{{"frequency"},frequency.data(),1,block,block,block}};
        binding.outputs={{{"audio"},output.data(),2,block,block*2,block}};
        binding.parameters={parameters.data(),8,block};node->bind(binding);
        const auto prepared=clock();std::chrono::nanoseconds dspTime{};
        std::uint64_t hash=14695981039346656037ull;double peak=0;
        const auto frames=rate*3;
        for(std::uint32_t origin=0;origin<frames;) {
            const auto count=std::min(block,frames-origin);
            const auto before=clock();node->process(0,count);dspTime+=clock()-before;
            for(std::uint32_t frame=0;frame<count;++frame) for(const auto sample:{output[frame],output[block+frame]}) {
                if(!std::isfinite(sample)) return 3;peak=std::max(peak,std::fabs(static_cast<double>(sample)));
                const auto* bytes=reinterpret_cast<const unsigned char*>(&sample);
                for(std::size_t byte=0;byte<sizeof(float);++byte) {hash^=bytes[byte];hash*=1099511628211ull;}
            }
            origin+=count;
        }
        const auto milliseconds=std::chrono::duration<double,std::milli>(dspTime).count();
        auto item=Json::object();item.set("sampleRate",Json::number(rate));item.set("blockSize",Json::number(block));
        item.set("notePolyphony",Json::number(1));item.set("oscillatorsPerNote",Json::number(8));
        item.set("frames",Json::number(frames));item.set("audioSeconds",Json::number(3));
        item.set("prepareMs",Json::number(std::chrono::duration<double,std::milli>(prepared-began).count()));
        item.set("dspMs",Json::number(milliseconds));item.set("realtimeFactor",Json::number(3000/milliseconds));
        item.set("sampleHash",Json::string(std::to_string(hash)));item.set("samplePeak",Json::number(peak));
        const auto bytes=nodes::builtinImplementations().stateBytes({"nod.unison-v2"},rate,block,1)+
            (frequency.capacity()+output.capacity()+parameters.capacity())*sizeof(float);
        item.set("preparedBudgetBytes",Json::number(static_cast<double>(bytes)));
#if defined(_WIN32)
        PROCESS_MEMORY_COUNTERS memory{};memory.cb=sizeof(memory);
        if(GetProcessMemoryInfo(GetCurrentProcess(),&memory,sizeof(memory))) item.set("processPeakWorkingSetBytes",Json::number(static_cast<double>(memory.PeakWorkingSetSize)));
#endif
        singleNodeMeasurements.push(std::move(item));
    }
    for(std::size_t index=0;index<singleNodeMeasurements.asArray().size();index+=3)
        if(singleNodeMeasurements.asArray()[index].find("sampleHash")->asString()!=singleNodeMeasurements.asArray()[index+1].find("sampleHash")->asString() ||
            singleNodeMeasurements.asArray()[index].find("sampleHash")->asString()!=singleNodeMeasurements.asArray()[index+2].find("sampleHash")->asString()) return 4;
    auto measurements=Json::array();
    for(const auto rate:{44100u,48000u,96000u}) for(const auto block:{64u,128u,512u}) {
        const auto began=clock();runtime::EngineConfig config;config.audio.sampleRate=rate;config.audio.maxFrames=block;config.parameterSmoothSeconds=0;
        runtime::Engine engine(config);auto graph=nodes::unisonPatch();graph.nodes[2].parameters[model::ParameterId{"voices"}]=8;
        const auto registry=nodes::builtinRegistry();const auto compiled=compiler::GraphCompiler{}.compile(graph,registry);if(!compiled.graph) return 2;
        auto prepared=runtime::preparePlan(*compiled.graph,registry,nodes::builtinImplementations(),engine.config(),engine.voices(),0);
        if(!prepared.plan) return 2;const auto bytes=prepared.plan->committedBytes()+engine.overheadBytes();
        if(!engine.stage(std::move(prepared.plan)).accepted) return 2;
        std::vector<float> left(block),right(block);std::array<runtime::MidiEvent,16> notes{},offs{};
        for(std::uint8_t index=0;index<16;++index) {notes[index]={0,runtime::MidiType::noteOn,0,static_cast<std::uint8_t>(48+index),127};offs[index]={0,runtime::MidiType::noteOff,0,static_cast<std::uint8_t>(48+index),0};}
        const auto dspStart=clock();const auto frames=rate*3;double peak=0,side=0,mono=0;std::uint64_t hash=14695981039346656037ull;
        for(std::uint32_t origin=0;origin<frames;) {
            const auto boundary=origin<rate*2?rate*2:frames;
            const auto count=std::min({block,frames-origin,boundary-origin});
            const auto events=origin==0?std::span<const runtime::MidiEvent>(notes):(origin==rate*2?std::span<const runtime::MidiEvent>(offs):std::span<const runtime::MidiEvent>{});
            float* output[]{left.data(),right.data()};engine.process(output,2,count,events);
            for(std::uint32_t frame=0;frame<count;++frame) {
                if(!std::isfinite(left[frame]) || !std::isfinite(right[frame])) return 3;
                peak=std::max({peak,std::fabs(static_cast<double>(left[frame])),std::fabs(static_cast<double>(right[frame]))});
                const double mid=(left[frame]+right[frame])*.5,s=(left[frame]-right[frame])*.5;mono+=mid*mid;side+=s*s;
                for(const auto value:{left[frame],right[frame]}) {
                    const auto* bytesView=reinterpret_cast<const unsigned char*>(&value);
                    for(std::size_t byte=0;byte<sizeof(float);++byte) {hash^=bytesView[byte];hash*=1099511628211ull;}
                }
            }
            origin+=count;
        }
        const auto end=clock();const auto milliseconds=std::chrono::duration<double,std::milli>(end-dspStart).count();
        auto item=Json::object();item.set("sampleRate",Json::number(rate));item.set("blockSize",Json::number(block));
        item.set("notePolyphony",Json::number(16));item.set("oscillatorsPerNote",Json::number(8));item.set("totalSuboscillators",Json::number(128));
        item.set("preparedBudgetBytes",Json::number(static_cast<double>(bytes)));item.set("frames",Json::number(frames));
        item.set("prepareMs",Json::number(std::chrono::duration<double,std::milli>(dspStart-began).count()));
        item.set("dspAndMeasurementMs",Json::number(milliseconds));item.set("audioSeconds",Json::number(3));
        item.set("realtimeFactor",Json::number(3000/milliseconds));item.set("sampleHash",Json::string(std::to_string(hash)));
        item.set("samplePeak",Json::number(peak));item.set("monoRms",Json::number(std::sqrt(mono/frames)));item.set("sideEnergy",Json::number(side/frames));
#if defined(_WIN32)
        PROCESS_MEMORY_COUNTERS memory{};memory.cb=sizeof(memory);
        if(GetProcessMemoryInfo(GetCurrentProcess(),&memory,sizeof(memory))) item.set("processPeakWorkingSetBytes",Json::number(static_cast<double>(memory.PeakWorkingSetSize)));
#endif
        measurements.push(std::move(item));
    }
    for(std::size_t index=0;index<measurements.asArray().size();index+=3)
        if(measurements.asArray()[index].find("sampleHash")->asString()!=measurements.asArray()[index+1].find("sampleHash")->asString() ||
            measurements.asArray()[index].find("sampleHash")->asString()!=measurements.asArray()[index+2].find("sampleHash")->asString()) return 4;
    auto report=Json::object();report.set("scope",Json::string("Release 16-note x 8 Unison maximum configuration; 2s held chord + 1s release; DSP timing includes measurements"));
    report.set("buildMode",Json::string(
#ifdef NDEBUG
        "Release"
#else
        "Debug"
#endif
    ));
    const auto* processor=std::getenv("PROCESSOR_IDENTIFIER");report.set("processor",processor?Json::string(processor):Json::null());
    report.set("measurements",std::move(measurements));report.set("auditionStatus",Json::string("unheard"));
    report.set("singleNodeScope",Json::string("One isolated stereo Unison node, 1 note x 8 suboscillators at 440 Hz for 3s; DSP timing excludes hashing, includes per-block timer overhead; budget includes state/table estimate and audio/parameter buffers; process peak is cumulative"));
    report.set("singleNodeMeasurements",std::move(singleNodeMeasurements));
    if(argc==2) {std::string error;if(!song::writeJsonAtomic(argv[1],report,error)) {std::cerr<<error;return 5;}}
    else std::cout<<report.dump()<<'\n';
    return 0;
}
