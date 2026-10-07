#include <nodsynth/nodes/GlobalEffect.h>
#include <nodsynth/effects/Effect.h>
#include <array>
#include <algorithm>
#include <stdexcept>

namespace nodsynth::nodes {
namespace {
class GlobalEffect final : public runtime::DspNode {
public:
    explicit GlobalEffect(std::string type):type_(std::move(type)) {}
    void bind(const runtime::NodeBinding& binding) override {
        for(const auto& input:binding.inputs) if(input.port.value=="audio-in") input_=input;
        for(const auto& output:binding.outputs) if(output.port.value=="audio-out") output_=output;
        parameters_=binding.parameters;
        effect_=effects::makeEffect(type_);effects::Config config;config.type=type_;std::string error;
        if(!effect_ || !effect_->prepare(binding.sampleRate,binding.maxFrames,config,error)) throw std::runtime_error(error);
        events_.resize(static_cast<std::size_t>(binding.maxFrames)*parameters_.count);
        tempo_.resize(binding.maxFrames,120);
        last_.fill(-1);reset();
    }
    void reset() override {if(effect_) effect_->reset();last_.fill(-1);}
    void process(std::uint32_t,std::uint32_t frames) override {
        std::size_t count=0;
        for(std::uint32_t frame=0;frame<frames;++frame) {
            *output_.at(0,0,frame)=input_.data?*input_.at(0,0,frame):0;
            *output_.at(0,1,frame)=input_.data?*input_.at(0,1,frame):0;
            if(type_=="delay" && parameters_.count>7) tempo_[frame]=parameters_.at(7,frame);
            for(std::uint32_t parameter=0;parameter<std::min(parameters_.count,type_=="delay"?7u:6u);++parameter) {
                const auto value=parameters_.at(parameter,frame);
                if(value!=last_[parameter]) {events_[count++]={frame,parameter,value};last_[parameter]=value;}
            }
        }
        effect_->process({output_.at(0,0,0),output_.at(0,1,0),frames,nullptr,nullptr,tempo_.data(),{events_.data(),count}});
    }
private:
    std::string type_;
    runtime::BufferView input_{},output_{};runtime::ParamView parameters_{};
    std::unique_ptr<effects::Effect> effect_;
    std::vector<effects::ParameterEvent> events_;
    std::vector<double> tempo_;
    std::array<double,8> last_{};
};
}
std::unique_ptr<runtime::DspNode> makeGlobalEffect(std::string type) {return std::make_unique<GlobalEffect>(std::move(type));}
std::uint64_t globalEffectStateBytes(const std::string& type,double rate,std::uint32_t frames) noexcept {
    return effects::stateBudget(type,rate)+static_cast<std::uint64_t>(frames)*(8*sizeof(effects::ParameterEvent)+sizeof(double))+1024;
}
}
