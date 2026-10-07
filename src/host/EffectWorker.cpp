#include <nodsynth/host/EffectWorker.h>
#include <nodsynth/host/Vst3Host.h>
#include "EffectProtocol.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <cstdlib>
#include <windows.h>
#include <psapi.h>

namespace nodsynth::host {
namespace {
std::string utf8(const std::filesystem::path& path) {const auto text=path.u8string();return {reinterpret_cast<const char*>(text.data()),text.size()};}
std::filesystem::path fromUtf8(const char* text) {return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(text)));}
std::wstring wide(const std::string& text) {const auto size=MultiByteToWideChar(CP_UTF8,0,text.data(),static_cast<int>(text.size()),nullptr,0);std::wstring result(size,0);MultiByteToWideChar(CP_UTF8,0,text.data(),static_cast<int>(text.size()),result.data(),size);return result;}
bool copyText(char* destination,std::size_t capacity,const std::string& text) {if(text.size()>=capacity) return false;std::memcpy(destination,text.c_str(),text.size()+1);return true;}

class WorkerEffect final : public effects::Effect {
public:
    explicit WorkerEffect(EffectWorkerOptions options):options_(std::move(options)) {}
    ~WorkerEffect() override {close();}
    bool prepare(double rate,std::uint32_t frames,const effects::Config& config,std::string& error) override {
        close();error_.clear();schema_={"vst3",1,{}};
        if(!std::isfinite(rate) || rate<8000 || rate>192000 || !frames || frames>protocol::maxFrames || config.automation.size()>16 ||
            options_.executable.empty() || options_.plugin.empty() || options_.timeoutMs<100 || options_.timeoutMs>300000) {error="vst3-effect: invalid preparation options";return false;}
        static std::atomic<std::uint32_t> serial{0};
        const auto name="Local\\NodEffect-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64())+"-"+std::to_string(serial.fetch_add(1));
        mapping_=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(protocol::Shared),wide(name).c_str());
        if(!mapping_ || GetLastError()==ERROR_ALREADY_EXISTS) {error="vst3-effect: could not create isolated worker mapping";close();return false;}
        shared_=static_cast<protocol::Shared*>(MapViewOfFile(mapping_,FILE_MAP_ALL_ACCESS,0,0,sizeof(protocol::Shared)));
        request_=CreateEventW(nullptr,FALSE,FALSE,wide(name+"-request").c_str());response_=CreateEventW(nullptr,FALSE,FALSE,wide(name+"-response").c_str());
        if(!shared_ || !request_ || !response_) {error="vst3-effect: could not prepare worker transport";close();return false;}
        new(shared_) protocol::Shared;
        shared_->rate=rate;shared_->capacity=frames;shared_->parentPid=GetCurrentProcessId();
        shared_->inspectionOnly=options_.inspectionOnly;
        if(!copyText(shared_->plugin,sizeof(shared_->plugin),utf8(options_.plugin)) || !copyText(shared_->state,sizeof(shared_->state),utf8(options_.state)) ||
            !copyText(shared_->className,sizeof(shared_->className),options_.className)) {error="vst3-effect: path or class exceeds transport budget";close();return false;}
        auto command=L"\""+options_.executable.wstring()+L"\" --effect-worker "+wide(name);
        STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION process{};
        if(!CreateProcessW(options_.executable.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process)) {
            error="vst3-effect: worker could not start";close();return false;
        }
        process_=process.hProcess;CloseHandle(process.hThread);
        if(!exchange(protocol::prepare)) {error=error_;close();return false;}
        if(options_.declaredTailSeconds>=0) shared_->tailSeconds=std::isfinite(shared_->tailSeconds)?std::max(shared_->tailSeconds,options_.declaredTailSeconds):options_.declaredTailSeconds;
        if(shared_->parameterCount>protocol::maxParameters || shared_->latency>static_cast<std::uint32_t>(rate*10) || (!options_.inspectionOnly && !std::isfinite(shared_->tailSeconds))) {error="vst3-effect: parameter, latency or infinite-tail budget exceeded";close();return false;}
        for(std::uint32_t index=0;index<shared_->parameterCount;++index) {
            const auto& p=shared_->parameters[index];
            const auto id="vst3:"+std::to_string(p.id);
            if(!std::isfinite(p.value) || p.value<0 || p.value>1 || std::any_of(schema_.parameters.begin(),schema_.parameters.end(),[&](const auto& item){return item.id==id;})) {
                error="vst3-effect-parameter: duplicate ID or invalid default";close();return false;
            }
            schema_.parameters.push_back({id,"normalized",0,1,p.value,p.automatable,p.title});
        }
        capacity_=frames;rate_=rate;latency_=shared_->latency;bypass_=config.bypass;bypassMix_=bypass_?1:0;
        dry_.resize((static_cast<std::size_t>(latency_)+1)*2);std::fill(dry_.begin(),dry_.end(),0);position_=0;
        initial_.clear();
        for(const auto& [name,value]:config.parameters) {
            const auto p=std::find_if(schema_.parameters.begin(),schema_.parameters.end(),[&](const auto& parameter){return parameter.id==name;});
            if(p==schema_.parameters.end() || !p->automatable || !std::isfinite(value) || value<0 || value>1) {error="vst3-effect-parameter: unknown, unavailable or invalid "+name;close();return false;}
            initial_.push_back({0,static_cast<std::uint32_t>(p-schema_.parameters.begin()),value});
        }
        for(const auto& lane:config.automation) {
            const auto p=std::find_if(schema_.parameters.begin(),schema_.parameters.end(),[&](const auto& parameter){return parameter.id==lane.parameter;});
            if(p==schema_.parameters.end() || !p->automatable) {error="vst3-effect-parameter: unknown or unavailable "+lane.parameter;close();return false;}
        }
        first_=true;return true;
    }
    void reset() noexcept override {
        std::fill(dry_.begin(),dry_.end(),0);position_=0;first_=true;bypassMix_=bypass_?1:0;
        if(shared_ && process_ && error_.empty()) exchange(protocol::reset);
    }
    void setBypass(bool value) noexcept override {bypass_=value;}
    std::uint32_t latencySamples() const noexcept override {return latency_;}
    double tailSeconds() const noexcept override {return shared_?shared_->tailSeconds:0;}
    const effects::Schema* parameterSchema() const noexcept override {return &schema_;}
    const char* errorReason() const noexcept override {return error_.empty()?nullptr:error_.c_str();}
    std::uint64_t workerMemoryBytes() const noexcept override {
        PROCESS_MEMORY_COUNTERS_EX memory{};memory.cb=sizeof(memory);
        return process_ && GetProcessMemoryInfo(process_,reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),sizeof(memory))?memory.PrivateUsage:0;
    }
    std::uint64_t committedBytes() const noexcept override {return sizeof(*this)+sizeof(protocol::Shared)+dry_.capacity()*sizeof(float)+initial_.capacity()*sizeof(effects::ParameterEvent);}
    void process(const effects::Block& block) noexcept override {
        if(!block.left || !block.right || !block.frames || block.frames>capacity_) return;
        if(options_.inspectionOnly) fail("vst3-inspection: processing is disabled in inspection mode");
        if(error_.empty() && shared_) {
            shared_->frames=block.frames;shared_->eventCount=0;
            if(first_) for(const auto& point:initial_) shared_->events[shared_->eventCount++]=point;
            if(block.parameters.size()+shared_->eventCount>protocol::maxEvents) fail("vst3-effect: event transport budget exceeded");
            else {
                for(const auto& point:block.parameters) shared_->events[shared_->eventCount++]=point;
                std::copy_n(block.left,block.frames,shared_->inputLeft);std::copy_n(block.right,block.frames,shared_->inputRight);
                for(std::uint32_t frame=0;frame<block.frames;++frame) shared_->bpm[frame]=block.bpm?block.bpm[frame]:120;
                exchange(protocol::process);first_=false;
            }
        }
        if(!error_.empty()) {std::fill_n(block.left,block.frames,std::numeric_limits<float>::quiet_NaN());std::fill_n(block.right,block.frames,std::numeric_limits<float>::quiet_NaN());return;}
        for(std::uint32_t frame=0;frame<block.frames;++frame) {
            float l=block.left[frame],r=block.right[frame];
            if(latency_) {const auto dl=dry_[position_],dr=dry_[position_+1];dry_[position_]=l;dry_[position_+1]=r;position_=(position_+2)%(latency_*2);l=dl;r=dr;}
            bypassMix_+=std::clamp((bypass_?1.:0.)-bypassMix_,-1/(rate_*.005),1/(rate_*.005));
            block.left[frame]=static_cast<float>(l*bypassMix_+shared_->outputLeft[frame]*(1-bypassMix_));
            block.right[frame]=static_cast<float>(r*bypassMix_+shared_->outputRight[frame]*(1-bypassMix_));
        }
    }
    PluginInspection inspection() const {
        PluginInspection result;result.parameters=schema_.parameters;result.tailSeconds=tailSeconds();result.workerPrivateBytes=workerMemoryBytes();
        result.info={shared_->pluginName,shared_->vendor,shared_->pluginVersion,shared_->subCategories,static_cast<std::int32_t>(latency_)};
        for(std::uint32_t index=0;index<shared_->classCount && index<128;++index) result.classes.emplace_back(shared_->classes[index]);return result;
    }
private:
    void fail(const char* message) noexcept {try {error_=message;} catch(...) {} }
    bool exchange(protocol::Command command) noexcept {
        shared_->command=command;shared_->ok=false;
        if(!SetEvent(request_)) {fail("vst3-effect: worker request failed");return false;}
        HANDLE handles[]{response_,process_};const auto wait=WaitForMultipleObjects(2,handles,FALSE,options_.timeoutMs);
        if(wait!=WAIT_OBJECT_0) {fail(wait==WAIT_TIMEOUT?"vst3-effect-timeout: worker exceeded its processing deadline":"vst3-effect-crash: worker exited before responding");return false;}
        if(!shared_->ok) {fail(shared_->error[0]?shared_->error:"vst3-effect: worker rejected operation");return false;}return true;
    }
    void close() noexcept {
        if(process_) {
            if(shared_ && request_ && WaitForSingleObject(process_,0)==WAIT_TIMEOUT) {shared_->command=protocol::stop;SetEvent(request_);}
            if(WaitForSingleObject(process_,250)==WAIT_TIMEOUT) {TerminateProcess(process_,1);WaitForSingleObject(process_,1000);}CloseHandle(process_);process_=nullptr;
        }
        if(shared_) {UnmapViewOfFile(shared_);shared_=nullptr;}
        for(auto* handle:{&request_,&response_,&mapping_}) if(*handle) {CloseHandle(*handle);*handle=nullptr;}
    }
    EffectWorkerOptions options_;HANDLE mapping_{nullptr},request_{nullptr},response_{nullptr},process_{nullptr};protocol::Shared* shared_{nullptr};
    effects::Schema schema_;std::string error_;std::vector<float> dry_;std::vector<effects::ParameterEvent> initial_;
    std::size_t position_{0};std::uint32_t capacity_{0},latency_{0};double rate_{48000},bypassMix_{0};bool bypass_{false},first_{true};
};
}

std::unique_ptr<effects::Effect> makeWorkerEffect(EffectWorkerOptions options) {return std::make_unique<WorkerEffect>(std::move(options));}
bool inspectPlugin(EffectWorkerOptions options,PluginInspection& result,std::string& error) {
    WorkerEffect effect(std::move(options));effects::Config config;config.type="vst3";
    if(!effect.prepare(48000,128,config,error)) return false;result=effect.inspection();return true;
}

int runEffectWorker(const std::string& mappingName) {
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX);
    _set_abort_behavior(0,_WRITE_ABORT_MSG|_CALL_REPORTFAULT);
    const auto mapping=OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,wide(mappingName).c_str());if(!mapping) return 2;
    auto* shared=static_cast<protocol::Shared*>(MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(protocol::Shared)));
    const auto request=OpenEventW(SYNCHRONIZE,FALSE,wide(mappingName+"-request").c_str());const auto response=OpenEventW(EVENT_MODIFY_STATE,FALSE,wide(mappingName+"-response").c_str());
    if(!shared || !request || !response || shared->protocolVersion!=protocol::version) return 2;
    const auto parent=OpenProcess(SYNCHRONIZE,FALSE,shared->parentPid);if(!parent) return 2;
    Vst3Plugin plugin;std::string error;std::vector<char> initialState;std::vector<Vst3ParamPoint> points;points.reserve(protocol::maxEvents);
    std::vector<Vst3Midi> midi;
    std::vector<Vst3ParamPoint> blockPoints;blockPoints.reserve(protocol::maxEvents);
    HANDLE handles[]{request,parent};bool running=true;
    while(running && WaitForMultipleObjects(2,handles,FALSE,INFINITE)==WAIT_OBJECT_0) {
        error.clear();bool ok=false;
        if(shared->command==protocol::stop) break;
        if(shared->command==protocol::prepare) {
            ok=plugin.open(fromUtf8(shared->plugin),error,shared->className);
            if(ok && shared->state[0]) {
                std::ifstream input(fromUtf8(shared->state),std::ios::binary|std::ios::ate);
                if(!input || input.tellg()<0 || input.tellg()>16*1024*1024) {error="vst3-effect-state: missing or oversized state";ok=false;}
                else {initialState.resize(static_cast<std::size_t>(input.tellg()));input.seekg(0);input.read(initialState.data(),initialState.size());ok=static_cast<bool>(input) && plugin.restoreState(initialState,error);}
            }
            if(ok) ok=plugin.activate(shared->rate,shared->capacity,error);
            if(ok && !shared->inspectionOnly && (!plugin.hasAudioInput() || plugin.info().subCategories.find("Instrument")!=std::string::npos)) {error="vst3-effect-input: effect requires a stereo audio input and a non-instrument class";ok=false;}
            if(ok) {
                const auto parameters=plugin.parameters();
                if(parameters.size()>protocol::maxParameters) {error="vst3-effect: plugin has too many parameters";ok=false;}
                else {
                    shared->parameterCount=static_cast<std::uint32_t>(parameters.size());
                    for(std::size_t index=0;index<parameters.size();++index) {const auto& p=parameters[index];auto& target=shared->parameters[index];target.id=p.id;target.automatable=p.canAutomate;target.value=p.value;copyText(target.title,sizeof(target.title),p.title.substr(0,127));}
                    shared->latency=static_cast<std::uint32_t>(std::max(0,plugin.info().latencySamples));shared->tailSeconds=plugin.tailSeconds();
                    const auto classes=plugin.classes();shared->classCount=static_cast<std::uint32_t>(std::min<std::size_t>(classes.size(),128));
                    for(std::uint32_t index=0;index<shared->classCount;++index) copyText(shared->classes[index],256,classes[index].substr(0,255));
                    copyText(shared->pluginName,256,plugin.info().name.substr(0,255));copyText(shared->vendor,256,plugin.info().vendor.substr(0,255));
                    copyText(shared->pluginVersion,256,plugin.info().version.substr(0,255));copyText(shared->subCategories,256,plugin.info().subCategories.substr(0,255));
                    if(initialState.empty()) ok=plugin.saveState(initialState,error);
                }
            }
        } else if(shared->command==protocol::reset) {
            ok=plugin.restart(initialState,shared->rate,shared->capacity,error);
        } else if(shared->command==protocol::process && shared->frames<=shared->capacity && shared->eventCount<=protocol::maxEvents) {
            points.clear();ok=true;
            for(std::uint32_t index=0;index<shared->eventCount;++index) {
                const auto& event=shared->events[index];
                if(event.parameter>=shared->parameterCount || !shared->parameters[event.parameter].automatable || event.sampleOffset>=shared->frames || !std::isfinite(event.value) || event.value<0 || event.value>1) {error="vst3-effect-parameter: invalid parameter event";ok=false;break;}
                points.push_back({event.sampleOffset,shared->parameters[event.parameter].id,event.value});
            }
            if(ok) {
                for(std::uint32_t begin=0;begin<shared->frames && ok;) {
                    auto end=begin+1;while(end<shared->frames && shared->bpm[end]==shared->bpm[begin]) ++end;
                    blockPoints.clear();for(auto point:points) if(point.sampleOffset>=begin && point.sampleOffset<end) {point.sampleOffset-=begin;blockPoints.push_back(point);}
                    plugin.setTempo(shared->bpm[begin]);
                    ok=plugin.process(shared->outputLeft+begin,shared->outputRight+begin,end-begin,midi,error,shared->inputLeft+begin,shared->inputRight+begin,&blockPoints);begin=end;
                }
                if(ok && static_cast<std::uint32_t>(std::max(0,plugin.currentLatency()))!=shared->latency) {error="vst3-effect-latency: plugin changed latency during rendering";ok=false;}
            }
        } else error="vst3-effect: invalid worker command";
        shared->ok=ok;copyText(shared->error,sizeof(shared->error),error.substr(0,1023));SetEvent(response);
    }
    plugin.close();CloseHandle(parent);CloseHandle(request);CloseHandle(response);UnmapViewOfFile(shared);CloseHandle(mapping);return 0;
}
}
