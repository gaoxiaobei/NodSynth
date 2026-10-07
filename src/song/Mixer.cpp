#include <nodsynth/song/Mixer.h>

#include <algorithm>
#include <cmath>
#include <set>
#include <limits>
#if defined(NOD_HAS_EFFECT_WORKER)
#include <nodsynth/host/EffectWorker.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace nodsynth::song {
using persist::Json;
Json effectsJson(const std::vector<effects::Config>& configs) {
    auto array=Json::array();
    for(const auto& config:configs) {
        auto item=Json::object(); item.set("id",Json::string(config.id)); item.set("type",Json::string(config.type));
        item.set("version",Json::number(config.version)); item.set("bypass",Json::boolean(config.bypass));
        item.set("quality",Json::string(config.quality)); item.set("sidechain",Json::string(config.sidechain));
        if(config.type=="vst3") {
            item.set("pluginResource",Json::string(config.pluginResource));item.set("stateResource",Json::string(config.stateResource));item.set("className",Json::string(config.className));
            item.set("workerTimeoutMs",Json::number(config.workerTimeoutMs));item.set("declaredTailSeconds",Json::number(config.declaredTailSeconds));
        }
        auto parameters=Json::object(); for(const auto& [name,value]:config.parameters) parameters.set(name,Json::number(value));
        item.set("parameters",std::move(parameters));
        auto automation=Json::array();
        for(const auto& lane:config.automation) {
            auto entry=Json::object();entry.set("parameter",Json::string(lane.parameter));entry.set("interpolation",Json::string(lane.interpolation));
            auto points=Json::array();for(const auto& point:lane.points) {auto p=Json::object();p.set("tick",Json::number(point.tick));p.set("value",Json::number(point.value));points.push(std::move(p));}
            entry.set("points",std::move(points));automation.push(std::move(entry));
        }
        item.set("automation",std::move(automation));array.push(std::move(item));
    }
    return array;
}
bool parseEffects(const Json& json,std::vector<effects::Config>& configs,std::string& error) {
    if(json.kind()!=Json::Kind::array || json.asArray().size()>32) { error="inserts must be an array of at most 32 effects"; return false; }
    std::vector<effects::Config> parsed;
    for(const auto& item:json.asArray()) {
        effects::Config config;
        if(!item.find("id") || item.find("id")->asString().empty() || !item.find("type")) { error="effect requires id and type"; return false; }
        config.id=item.find("id")->asString(); config.type=item.find("type")->asString();
        if(const auto* version=item.find("version")) {
            if(version->kind()!=Json::Kind::number || version->asNumber()!=1) { error="unsupported effect version"; return false; }
            config.version=1;
        }
        if(const auto* bypass=item.find("bypass")) { if(bypass->kind()!=Json::Kind::boolean) {error="bypass must be boolean";return false;} config.bypass=bypass->asBool(); }
        if(const auto* quality=item.find("quality")) config.quality=quality->asString();
        if(const auto* sidechain=item.find("sidechain")) config.sidechain=sidechain->asString();
        for(const auto* name:{"pluginResource","stateResource","className"}) if(const auto* field=item.find(name)) {
            if(field->kind()!=Json::Kind::string) {error="plugin fields must be strings";return false;}
            (std::string(name)=="pluginResource"?config.pluginResource:std::string(name)=="stateResource"?config.stateResource:config.className)=field->asString();
        }
        if(const auto* timeout=item.find("workerTimeoutMs")) {
            if(timeout->kind()!=Json::Kind::number || !std::isfinite(timeout->asNumber()) || timeout->asNumber()<100 || timeout->asNumber()>300000 || timeout->asNumber()!=std::floor(timeout->asNumber())) {error="invalid worker timeout";return false;}
            config.workerTimeoutMs=static_cast<std::uint32_t>(timeout->asNumber());
        }
        if(const auto* tail=item.find("declaredTailSeconds")) {if(tail->kind()!=Json::Kind::number) {error="invalid declared effect tail";return false;}config.declaredTailSeconds=tail->asNumber();}
        if(const auto* parameters=item.find("parameters")) {
            if(parameters->kind()!=Json::Kind::object) {error="effect parameters must be an object";return false;}
            for(const auto& [name,value]:parameters->items()) {
                if(value.kind()!=Json::Kind::number || !std::isfinite(value.asNumber())) {error="effect parameters must be finite numbers";return false;}
                config.parameters[name]=value.asNumber();
            }
        }
        if(!effects::validateConfig(config,error)) return false;
        if(const auto* automation=item.find("automation")) {
            if(automation->kind()!=Json::Kind::array || automation->asArray().size()>16) {error="effect automation must contain at most 16 lanes";return false;}
            for(const auto& entry:automation->asArray()) {
                effects::Config::Lane lane;
                if(!entry.find("parameter") || !entry.find("points") || entry.find("points")->kind()!=Json::Kind::array) {error="effect lane requires parameter and points";return false;}
                lane.parameter=entry.find("parameter")->asString();if(const auto* interpolation=entry.find("interpolation")) lane.interpolation=interpolation->asString();
                for(const auto& point:entry.find("points")->asArray()) {
                    const auto* tick=point.find("tick");const auto* value=point.find("value");
                    if(!tick || tick->kind()!=Json::Kind::number || !std::isfinite(tick->asNumber()) || tick->asNumber()<0 || tick->asNumber()>UINT32_MAX || tick->asNumber()!=std::floor(tick->asNumber()) ||
                        !value || value->kind()!=Json::Kind::number) {error="effect automation point is invalid";return false;}
                    lane.points.push_back({static_cast<std::uint32_t>(tick->asNumber()),value->asNumber()});
                }
                config.automation.push_back(std::move(lane));
            }
            if(!effects::validateConfig(config,error)) return false;
        }
        parsed.push_back(std::move(config));
    }
    configs=std::move(parsed); return true;
}
Json sendsJson(const std::vector<Send>& sends) {
    auto array=Json::array();
    for(const auto& send:sends) {
        auto item=Json::object(); item.set("target",Json::string(send.target)); item.set("gain",Json::number(send.gain));
        item.set("position",Json::string(send.preFader?"pre-fader":"post-fader")); array.push(std::move(item));
    }
    return array;
}
bool parseSends(const Json& json,std::vector<Send>& sends,std::string& error) {
    if(json.kind()!=Json::Kind::array || json.asArray().size()>32) {error="sends must be an array of at most 32 entries";return false;}
    std::vector<Send> parsed;
    for(const auto& item:json.asArray()) {
        Send send;
        if(!item.find("target") || item.find("target")->asString().empty()) {error="send requires target";return false;}
        send.target=item.find("target")->asString();
        if(const auto* gain=item.find("gain")) {
            if(gain->kind()!=Json::Kind::number) {error="send gain must be numeric";return false;} send.gain=gain->asNumber();
        }
        if(const auto* position=item.find("position")) {
            if(position->asString()!="pre-fader" && position->asString()!="post-fader") {error="send position must be pre-fader or post-fader";return false;}
            send.preFader=position->asString()=="pre-fader";
        }
        parsed.push_back(std::move(send));
    }
    sends=std::move(parsed);return true;
}
Json routingJson(const SongDocument& song) {
    auto result=Json::object(),buses=Json::array();
    for(const auto& bus:song.buses) {
        auto item=Json::object();item.set("id",Json::string(bus.id));item.set("name",Json::string(bus.name));
        item.set("output",Json::string(bus.output));item.set("gain",Json::number(bus.gain));item.set("pan",Json::number(bus.pan));
        item.set("return",Json::boolean(bus.isReturn));item.set("mute",Json::boolean(bus.mute));
        item.set("inserts",effectsJson(bus.inserts));item.set("sends",sendsJson(bus.sends));buses.push(std::move(item));
    }
    result.set("buses",std::move(buses));result.set("masterInserts",effectsJson(song.masterInserts));return result;
}
bool parseRouting(const Json& json,SongDocument& song,std::string& error) {
    if(const auto* inserts=json.find("masterInserts")) if(!parseEffects(*inserts,song.masterInserts,error)) return false;
    if(const auto* buses=json.find("buses")) {
        if(buses->kind()!=Json::Kind::array || buses->asArray().size()>64) {error="buses must be an array of at most 64 entries";return false;}
        for(const auto& item:buses->asArray()) {
            Bus bus;
            if(!item.find("id")) {error="bus requires id";return false;} bus.id=item.find("id")->asString();
            if(const auto* name=item.find("name")) bus.name=name->asString();
            if(const auto* output=item.find("output")) bus.output=output->asString();
            if(const auto* gain=item.find("gain")) {if(gain->kind()!=Json::Kind::number) {error="bus gain must be numeric";return false;}bus.gain=gain->asNumber();}
            if(const auto* pan=item.find("pan")) {if(pan->kind()!=Json::Kind::number) {error="bus pan must be numeric";return false;}bus.pan=pan->asNumber();}
            if(const auto* value=item.find("return")) {if(value->kind()!=Json::Kind::boolean) {error="return must be boolean";return false;}bus.isReturn=value->asBool();}
            if(const auto* value=item.find("mute")) {if(value->kind()!=Json::Kind::boolean) {error="bus mute must be boolean";return false;}bus.mute=value->asBool();}
            if(const auto* inserts=item.find("inserts")) if(!parseEffects(*inserts,bus.inserts,error)) return false;
            if(const auto* sends=item.find("sends")) if(!parseSends(*sends,bus.sends,error)) return false;
            song.buses.push_back(std::move(bus));
        }
    }
    return true;
}
bool hasRouting(const SongDocument& song) noexcept {
    if(!song.buses.empty() || !song.masterInserts.empty()) return true;
    return std::any_of(song.tracks.begin(),song.tracks.end(),[](const auto& t){return t.output!="master" || !t.inserts.empty() || !t.sends.empty();});
}

namespace {
bool topology(const SongDocument& song,std::vector<std::size_t>& order,std::string& error) {
    std::map<std::string,std::size_t> nodes;
    std::set<std::string> targets{"master"};
    for(const auto& bus:song.buses) targets.insert(bus.id);
    const auto add=[&](const std::string& id) {
        if(id.empty() || nodes.contains(id)) {error="route-id: routing ids must be nonempty and unique";return false;}
        nodes[id]=nodes.size();return true;
    };
    for(const auto& track:song.tracks) if(!add(track.id)) return false;
    for(const auto& bus:song.buses) if(!add(bus.id)) return false;
    if(!add("master")) return false;
    std::vector<std::vector<std::size_t>> edges(nodes.size());std::vector<std::size_t> indegree(nodes.size());
    auto edge=[&](std::size_t source,const std::string& target) {
        if(!targets.contains(target)) {error="route-target: output/send must target a bus, return or master";return false;}
        edges[source].push_back(nodes[target]);++indegree[nodes[target]];return true;
    };
    auto connections=[&](const std::string& id,const std::string& output,const std::vector<Send>& sends,const std::vector<effects::Config>& inserts) {
        const auto source=nodes[id];
        if(!output.empty() && !edge(source,output)) return false;
        for(const auto& send:sends) {
            if(!std::isfinite(send.gain) || send.gain<0 || send.gain>4) {error="route-gain: send gain must be 0..4";return false;}
            if(!edge(source,send.target)) return false;
        }
        std::set<std::string> effectIds;
        for(const auto& config:inserts) {
            if(config.id.empty() || !effectIds.insert(config.id).second || !effects::validateConfig(config,error)) {
                if(error.empty()) error="effect-id: insert ids must be nonempty and unique within a chain";return false;
            }
            if(config.type=="vst3") for(const auto& [id,kind]:std::vector<std::pair<std::string,std::string>>{{config.pluginResource,"vst3-plugin"},{config.stateResource,"plugin-state"}}) {
                if(id.empty()) continue;
                const auto resource=std::find_if(song.resources.begin(),song.resources.end(),[&](const auto& r){return r.id==id;});
                if(resource==song.resources.end() || resource->kind!=kind || resource->hash.empty()) {error="vst3-effect-resource: missing, wrong-kind or unhashed dependency "+id;return false;}
            }
            if(!config.sidechain.empty()) {
                const auto detector=nodes.find(config.sidechain);
                if(detector==nodes.end()) {error="sidechain-source: detector source was not found";return false;}
                edges[detector->second].push_back(source);++indegree[source];
            }
        }
        return true;
    };
    for(const auto& track:song.tracks) if(!connections(track.id,track.output,track.sends,track.inserts)) return false;
    for(const auto& bus:song.buses) {
        if(!std::isfinite(bus.gain) || bus.gain<0 || bus.gain>4 || !std::isfinite(bus.pan) || std::fabs(bus.pan)>1) {error="bus-mix: invalid bus gain/pan";return false;}
        if(!connections(bus.id,bus.output,bus.sends,bus.inserts)) return false;
    }
    if(!connections("master","",{},song.masterInserts)) return false;
    order.clear();
    while(order.size()<nodes.size()) {
        auto ready=indegree.size();
        for(std::size_t index=0;index<indegree.size();++index) if(indegree[index]==0) {ready=index;break;}
        if(ready==indegree.size()) {error="route-cycle: feedback routes are unsupported";return false;}
        indegree[ready]=SIZE_MAX;order.push_back(ready);
        for(const auto target:edges[ready]) --indegree[target];
    }
    return true;
}
}
bool validateRouting(const SongDocument& song,std::string& error) {
    std::vector<std::size_t> order;return topology(song,order,error);
}

bool SongMixer::prepare(const SongDocument& song,std::uint32_t rate,std::uint32_t frames,std::string& error,const std::filesystem::path& baseDirectory) {
    if(rate<8000 || rate>192000 || !frames || frames>8192 || song.tracks.size()+song.buses.size()>256) {error="mixer rate, block or node count exceeds supported budget";return false;}
    if(!topology(song,order_,error)) return false;
    tracks_=song.tracks.size();buses_=song.buses.size();master_=tracks_+buses_;capacity_=frames;
    nodes_.clear();nodes_.resize(master_+1);edges_.clear();bpm_.resize(frames);tempo_.clear();
    for(const auto& point:song.tempo) {
        const auto sample=sampleAtTick(song,point.tick,rate);if(!sample) {error="tempo sample overflow";return false;}
        tempo_.push_back({*sample,60000000.0/point.microsecondsPerQuarter});
    }
    std::map<std::string,std::size_t> targets;targets["master"]=master_;
    std::map<std::string,std::size_t> sources;sources["master"]=master_;
    for(std::size_t i=0;i<tracks_;++i) sources[song.tracks[i].id]=i;
    for(std::size_t i=0;i<buses_;++i) targets[song.buses[i].id]=tracks_+i;
    for(std::size_t i=0;i<buses_;++i) sources[song.buses[i].id]=tracks_+i;
    bytes_=sizeof(*this);tail_=0;latency_=0;
    auto configure=[&](std::size_t index,const std::string& output,const std::vector<Send>& sends,const std::vector<effects::Config>& configs) {
        auto& node=nodes_[index];node.left.resize(frames);node.right.resize(frames);node.post.resize(frames*2);
        bytes_+=sizeof(Node)+frames*sizeof(float)*4;
        for(const auto& config:configs) {
            const auto budget=effects::stateBudget(config.type,rate);
            if(budget>512ull*1024*1024 || bytes_>512ull*1024*1024-budget) {error="mixer-budget: effects exceed 512 MiB preparation budget";return false;}
            auto effect=effects::makeEffect(config.type);
            if(config.type=="vst3") {
#if defined(NOD_HAS_EFFECT_WORKER)
                host::EffectWorkerOptions worker;worker.className=config.className;worker.timeoutMs=config.workerTimeoutMs;worker.declaredTailSeconds=config.declaredTailSeconds;
                std::wstring executable(32768,0);const auto length=GetModuleFileNameW(nullptr,executable.data(),static_cast<DWORD>(executable.size()));executable.resize(length);
                worker.executable=std::filesystem::path(executable).parent_path()/"nod_vst3_worker.exe";
                if(!std::filesystem::exists(worker.executable)) worker.executable=NOD_DEFAULT_EFFECT_WORKER;
                for(const auto& [id,path]:std::vector<std::pair<std::string,std::filesystem::path*>>{{config.pluginResource,&worker.plugin},{config.stateResource,&worker.state}}) {
                    if(id.empty()) continue;
                    const auto resource=std::find_if(song.resources.begin(),song.resources.end(),[&](const auto& r){return r.id==id;});
                    *path=std::filesystem::absolute(resolveResourcePath(baseDirectory.empty()?song.baseDirectory:baseDirectory,resource->path));
                    if(hashFile(*path)!=resource->hash) {error="vst3-effect-resource: changed or unreadable dependency "+id;return false;}
                }
                effect=host::makeWorkerEffect(std::move(worker));
#else
                error="vst3-effect-unavailable: this build has no isolated VST3 host";return false;
#endif
            }
            if(!effect || !effect->prepare(rate,frames,config,error)) return false;
            const auto* effectSchema=effect->parameterSchema()?effect->parameterSchema():effects::findSchema(config.type);
            if(config.type=="delay" && config.parameters.contains("syncBeats") && config.parameters.at("syncBeats")>0)
                for(const auto& [sample,bpm]:tempo_) if(config.parameters.at("syncBeats")*60/bpm>8) {error="delay-time: tempo-sync exceeds 8-second buffer";return false;}
            bytes_+=effect->committedBytes();
            auto maximum=[&](const std::string& name) {
                double value=0;for(const auto& p:effectSchema->parameters) if(p.id==name) value=p.defaultValue;
                if(config.parameters.contains(name)) value=config.parameters.at(name);
                for(const auto& lane:config.automation) if(lane.parameter==name) for(const auto& point:lane.points) value=std::max(value,point.value);
                return value;
            };
            if(config.type=="reverb") tail_+=maximum("preDelayMs")/1000+maximum("decay")*2+.2;
            else if(config.type=="delay") {
                double delay=maximum("timeMs")/1000;
                for(const auto& [sample,bpm]:tempo_) delay=std::max(delay,maximum("syncBeats")*60/bpm);
                if(delay>8) {error="delay-time: automated tempo-sync exceeds 8-second buffer";return false;}
                const double feedback=maximum("feedback");tail_+=delay*(feedback>0?1+std::log(.00001)/std::log(feedback):1)+.5;
            } else if(config.type=="eq" && std::any_of(config.automation.begin(),config.automation.end(),[](const auto& lane){return lane.parameter!="wet";})) tail_+=30;
            else tail_+=effect->tailSeconds();
            Node::Insert insert;insert.effect=std::move(effect);
            if(!config.sidechain.empty()) {
                insert.detectorSource=sources.at(config.sidechain);insert.detectorLeft.resize(frames);insert.detectorRight.resize(frames);bytes_+=frames*sizeof(float)*2;
            }
            const auto* schema=effectSchema;
            for(const auto& lane:config.automation) {
                const auto p=std::find_if(schema->parameters.begin(),schema->parameters.end(),[&](const auto& parameter){return parameter.id==lane.parameter;});
                if(p==schema->parameters.end() || !p->automatable) {error="effect-automation: target is unavailable";return false;}
                PreparedLane prepared;prepared.index=static_cast<std::uint32_t>(p-schema->parameters.begin());prepared.linear=lane.interpolation=="linear";
                prepared.base=config.parameters.contains(p->id)?config.parameters.at(p->id):p->defaultValue;
                for(const auto& point:lane.points) {
                    const auto sample=sampleAtTick(song,point.tick,rate);
                    if(!sample || (!prepared.points.empty() && *sample<=prepared.points.back().sample)) {error="effect automation points collide or exceed sample range";return false;}
                    prepared.points.push_back({*sample,point.value});
                }
                bytes_+=sizeof(PreparedLane)+prepared.points.capacity()*sizeof(PreparedLane::Point);insert.lanes.push_back(std::move(prepared));
            }
            insert.events.resize(static_cast<std::size_t>(frames)*insert.lanes.size());bytes_+=insert.events.capacity()*sizeof(effects::ParameterEvent);
            node.inserts.push_back(std::move(insert));
        }
        auto connect=[&](const std::string& target,double gain,bool pre) {
            node.edges.push_back(edges_.size());Edge edge;edge.source=index;edge.target=targets.at(target);edge.gain=gain;edge.preFader=pre;edges_.push_back(std::move(edge));
        };
        if(!output.empty()) connect(output,1,false);
        for(const auto& send:sends) connect(send.target,send.gain,send.preFader);
        return true;
    };
    for(std::size_t i=0;i<tracks_;++i) {
        const auto& track=song.tracks[i];if(!configure(i,track.output,track.sends,track.inserts)) return false;
        PreparedTrackMix mix;if(!prepareTrackMix(song,track,rate,mix,error)) return false;nodes_[i].trackMix=std::move(mix);
    }
    for(std::size_t i=0;i<buses_;++i) {
        const auto& bus=song.buses[i];auto& node=nodes_[tracks_+i];node.gain=bus.gain;node.pan=bus.pan;node.mute=bus.mute;
        if(!configure(tracks_+i,bus.output,bus.sends,bus.inserts)) return false;
    }
    if(!configure(master_,"",{},song.masterInserts)) return false;
    const auto compensate=[&](std::vector<float>& buffer,std::uint32_t delay) {
        const auto bytes=static_cast<std::uint64_t>(delay)*sizeof(float)*2;
        if(bytes>512ull*1024*1024 || bytes_>512ull*1024*1024-bytes) {
            error="mixer-budget: latency compensation exceeds 512 MiB";return false;
        }
        buffer.resize(static_cast<std::size_t>(delay)*2);bytes_+=bytes;return true;
    };
    for(auto index:order_) {
        auto& node=nodes_[index];std::uint32_t inputLatency=0;
        for(const auto& edge:edges_) if(edge.target==index) inputLatency=std::max(inputLatency,nodes_[edge.source].outputLatency);
        for(const auto& insert:node.inserts) if(insert.detectorSource) inputLatency=std::max(inputLatency,nodes_[*insert.detectorSource].outputLatency);
        for(auto& edge:edges_) if(edge.target==index) {
            const auto delay=inputLatency-nodes_[edge.source].outputLatency;if(!compensate(edge.compensation,delay)) return false;
        }
        node.outputLatency=inputLatency;
        if(index<tracks_ && !compensate(node.inputCompensation,inputLatency)) return false;
        for(auto& insert:node.inserts) {
            if(insert.detectorSource) {
                const auto delay=node.outputLatency-nodes_[*insert.detectorSource].outputLatency;
                if(!compensate(insert.detectorCompensation,delay)) return false;
            }
            const auto latency=insert.effect->latencySamples();
            if(latency>std::numeric_limits<std::uint32_t>::max()-node.outputLatency) {error="mixer-budget: cumulative latency overflow";return false;}
            node.outputLatency+=latency;
        }
    }
    latency_=nodes_[master_].outputLatency;
    if(bytes_>512ull*1024*1024) {error="mixer-budget: prepared mixer exceeds 512 MiB";return false;}
    reset();if(const auto* reason=errorReason()) {error=reason;return false;}return true;
}
persist::Json SongMixer::effectReport(const SongDocument& song,bool frozen) const {
    auto report=Json::array();
    const auto add=[&](std::size_t index,const std::string& target,const auto& configs) {
        for(std::size_t i=0;i<configs.size();++i) {
            const auto& config=configs[i];const auto& effect=*nodes_[index].inserts[i].effect;
            auto item=Json::object();item.set("target",Json::string(target));item.set("id",Json::string(config.id));item.set("type",Json::string(config.type));
            item.set("version",Json::number(config.version));item.set("latencySamples",Json::number(effect.latencySamples()));
            item.set("quality",Json::string(config.quality));
            item.set("oversamplingFactor",Json::number(config.type=="saturation"?(config.quality=="high"?4:2):1));
            if(config.type=="limiter") item.set("truePeakDetection",Json::string(config.quality=="high"?"libebur128 rate-dependent kernel plus 8x supplemental detection":"libebur128 rate-dependent kernel"));
            item.set("tailSeconds",Json::number(effect.tailSeconds()));item.set("parentCommittedBytes",Json::number(effect.committedBytes()));
            if(config.type=="vst3") {
                item.set("workerPrivateBytes",Json::number(effect.workerMemoryBytes()));
                item.set("frozen",Json::boolean(frozen));item.set("className",Json::string(config.className));
                for(const auto& [id,key]:std::vector<std::pair<std::string,std::string>>{{config.pluginResource,"pluginHash"},{config.stateResource,"stateHash"}})
                    for(const auto& resource:song.resources) if(resource.id==id) item.set(key,Json::string(resource.hash));
            }
            report.push(std::move(item));
        }
    };
    for(std::size_t i=0;i<tracks_;++i) add(i,song.tracks[i].id,song.tracks[i].inserts);
    for(std::size_t i=0;i<buses_;++i) add(tracks_+i,song.buses[i].id,song.buses[i].inserts);
    add(master_,"master",song.masterInserts);return report;
}
void SongMixer::reset() noexcept {
    for(auto& node:nodes_) {
        std::fill(node.left.begin(),node.left.end(),0);std::fill(node.right.begin(),node.right.end(),0);std::fill(node.post.begin(),node.post.end(),0);
        std::fill(node.inputCompensation.begin(),node.inputCompensation.end(),0);node.inputPosition=0;
        for(auto& insert:node.inserts) {insert.effect->reset();std::fill(insert.detectorCompensation.begin(),insert.detectorCompensation.end(),0);insert.detectorPosition=0;}
    }
    for(auto& edge:edges_) {std::fill(edge.compensation.begin(),edge.compensation.end(),0);edge.position=0;}
}
void SongMixer::setTrackInput(std::size_t index,const float* left,const float* right,std::uint32_t frames) noexcept {
    if(index>=tracks_ || frames>capacity_) return;
    std::copy_n(left,frames,nodes_[index].left.begin());std::copy_n(right,frames,nodes_[index].right.begin());
}
void SongMixer::process(std::int64_t origin,std::uint32_t frames,float* master) noexcept {
    if(frames>capacity_ || !master) return;
    for(std::size_t index=tracks_;index<nodes_.size();++index) {std::fill_n(nodes_[index].left.begin(),frames,0);std::fill_n(nodes_[index].right.begin(),frames,0);}
    for(std::uint32_t frame=0;frame<frames;++frame) {
        double tempo=120;for(const auto& [sample,bpm]:tempo_) {if(sample>origin+frame) break;tempo=bpm;}bpm_[frame]=tempo;
    }
    for(auto index:order_) {
        auto& node=nodes_[index];
        if(!node.inputCompensation.empty()) for(std::uint32_t frame=0;frame<frames;++frame) {
            const auto position=node.inputPosition;const auto l=node.inputCompensation[position],r=node.inputCompensation[position+1];
            node.inputCompensation[position]=node.left[frame];node.inputCompensation[position+1]=node.right[frame];
            node.left[frame]=l;node.right[frame]=r;node.inputPosition=(position+2)%node.inputCompensation.size();
        }
        std::uint32_t inputLatency=node.outputLatency;
        for(const auto& insert:node.inserts) inputLatency-=insert.effect->latencySamples();
        for(auto& insert:node.inserts) {
            std::size_t count=0;
            for(std::uint32_t frame=0;frame<frames;++frame) for(const auto& lane:insert.lanes)
                insert.events[count++]={frame,lane.index,parameterAt(lane,origin+frame-inputLatency)};
            for(std::uint32_t frame=0;frame<frames;++frame) {
                double tempo=120;for(const auto& [sample,bpm]:tempo_) {if(sample>origin+frame-inputLatency) break;tempo=bpm;}bpm_[frame]=tempo;
            }
            if(insert.detectorSource) for(std::uint32_t frame=0;frame<frames;++frame) {
                const auto& source=nodes_[*insert.detectorSource];float l=source.post[frame*2],r=source.post[frame*2+1];
                if(!insert.detectorCompensation.empty()) {
                    const auto position=insert.detectorPosition;const auto dl=insert.detectorCompensation[position],dr=insert.detectorCompensation[position+1];
                    insert.detectorCompensation[position]=l;insert.detectorCompensation[position+1]=r;insert.detectorPosition=(position+2)%insert.detectorCompensation.size();l=dl;r=dr;
                }
                insert.detectorLeft[frame]=l;insert.detectorRight[frame]=r;
            }
            insert.effect->process({node.left.data(),node.right.data(),frames,insert.detectorSource?insert.detectorLeft.data():nullptr,
                insert.detectorSource?insert.detectorRight.data():nullptr,bpm_.data(),{insert.events.data(),count}});
            inputLatency+=insert.effect->latencySamples();
        }
        for(std::uint32_t frame=0;frame<frames;++frame) {
            float gl=static_cast<float>(node.gain*(node.pan>0?1-node.pan:1)),gr=static_cast<float>(node.gain*(node.pan<0?1+node.pan:1));
            if(node.trackMix) node.trackMix->gainsAt(origin+frame-node.outputLatency,gl,gr);
            if(node.mute) gl=gr=0;
            node.post[frame*2]=node.left[frame]*gl;node.post[frame*2+1]=node.right[frame]*gr;
        }
        for(const auto edgeIndex:node.edges) {
            auto& edge=edges_[edgeIndex];auto& target=nodes_[edge.target];
            for(std::uint32_t frame=0;frame<frames;++frame) {
                float l=edge.preFader?node.left[frame]:node.post[frame*2],r=edge.preFader?node.right[frame]:node.post[frame*2+1];
                if(!edge.compensation.empty()) {
                    const auto position=edge.position;const auto dl=edge.compensation[position],dr=edge.compensation[position+1];
                    edge.compensation[position]=l;edge.compensation[position+1]=r;edge.position=(position+2)%edge.compensation.size();l=dl;r=dr;
                }
                target.left[frame]+=static_cast<float>(l*edge.gain);target.right[frame]+=static_cast<float>(r*edge.gain);
            }
        }
    }
    std::copy_n(nodes_[master_].post.begin(),frames*2,master);
}
const float* SongMixer::trackOutput(std::size_t index) const noexcept {return index<tracks_?nodes_[index].post.data():nullptr;}
const float* SongMixer::busOutput(std::size_t index) const noexcept {return index<buses_?nodes_[tracks_+index].post.data():nullptr;}
const char* SongMixer::errorReason() const noexcept {for(const auto& node:nodes_) for(const auto& insert:node.inserts) if(const auto* reason=insert.effect->errorReason()) return reason;return nullptr;}
}
