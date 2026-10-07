#include "public.sdk/source/main/pluginfactory.h"
#include "public.sdk/source/vst/vstsinglecomponenteffect.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include <array>
#include <cmath>
#include <cstdlib>
#include <windows.h>

namespace Steinberg::Vst {
class EffectProbe final : public SingleComponentEffect {
public:
    static FUnknown* createInstance(void*) {return static_cast<IAudioProcessor*>(new EffectProbe);}
    tresult PLUGIN_API initialize(FUnknown* context) SMTG_OVERRIDE {
        const auto result=SingleComponentEffect::initialize(context);if(result!=kResultOk) return result;
        addAudioInput(STR16("Stereo In"),SpeakerArr::kStereo);addAudioOutput(STR16("Stereo Out"),SpeakerArr::kStereo);
        parameters.addParameter(STR16("Gain"),nullptr,0,.5,ParameterInfo::kCanAutomate,7);
        parameters.addParameter(STR16("Failure probe"),nullptr,0,0,ParameterInfo::kCanAutomate,9);
        parameters.addParameter(STR16("Tempo gain"),nullptr,1,0,ParameterInfo::kCanAutomate,11);return kResultOk;
    }
    uint32 PLUGIN_API getLatencySamples() SMTG_OVERRIDE {return failure_>=.99?256:128;}
    uint32 PLUGIN_API getTailSamples() SMTG_OVERRIDE {return 0;}
    tresult PLUGIN_API canProcessSampleSize(int32 size) SMTG_OVERRIDE {return size==kSample32?kResultOk:kResultFalse;}
    tresult PLUGIN_API setActive(TBool state) SMTG_OVERRIDE {if(state) {ring_={};position_=0;}return SingleComponentEffect::setActive(state);}
    tresult PLUGIN_API getState(IBStream* stream) SMTG_OVERRIDE {
        const double state[]{gain_,tempo_};return stream?stream->write(const_cast<double*>(state),sizeof(state),nullptr):kResultFalse;
    }
    tresult PLUGIN_API setState(IBStream* stream) SMTG_OVERRIDE {
        double state[2]{};int32 read=0;if(!stream || stream->read(state,sizeof(state),&read)!=kResultOk || read!=sizeof(state) || !std::isfinite(state[0]) || !std::isfinite(state[1])) return kResultFalse;
        gain_=state[0];tempo_=state[1];failure_=0;parameters.getParameter(7)->setNormalized(gain_);parameters.getParameter(11)->setNormalized(tempo_);return kResultOk;
    }
    tresult PLUGIN_API setComponentState(IBStream* stream) SMTG_OVERRIDE {return setState(stream);}
    tresult PLUGIN_API process(ProcessData& data) SMTG_OVERRIDE {
        if(!data.inputs || !data.outputs || data.inputs[0].numChannels!=2 || data.outputs[0].numChannels!=2) return kResultFalse;
        const auto count=data.inputParameterChanges?data.inputParameterChanges->getParameterCount():0;std::array<int32,3> cursors{};
        for(int32 frame=0;frame<data.numSamples;++frame) {
            for(int32 index=0;index<count && index<3;++index) {
                auto* queue=data.inputParameterChanges->getParameterData(index);if(!queue) continue;
                auto& cursor=cursors[index];int32 offset=0;double value=0;
                while(cursor<queue->getPointCount() && queue->getPoint(cursor,offset,value)==kResultOk && offset<=frame) {
                    if(queue->getParameterId()==7) gain_=value;if(queue->getParameterId()==9) failure_=value;if(queue->getParameterId()==11) tempo_=value;++cursor;
                }
            }
            if(failure_>.7 && failure_<.9) std::abort();
            if(failure_>.4 && failure_<.6) Sleep(2000);
            if(failure_>.2 && failure_<.4) return kResultFalse;
            const double gain=2*gain_*(tempo_>=.5 && data.processContext?data.processContext->tempo/120:1);
            for(int channel=0;channel<2;++channel) {data.outputs[0].channelBuffers32[channel][frame]=ring_[position_][channel];ring_[position_][channel]=static_cast<float>(data.inputs[0].channelBuffers32[channel][frame]*gain);}
            position_=(position_+1)%ring_.size();
        }return kResultOk;
    }
    IPlugView* PLUGIN_API createView(FIDString) SMTG_OVERRIDE {return nullptr;}
private:
    double gain_{.5},tempo_{0},failure_{0};std::array<std::array<float,2>,128> ring_{};std::size_t position_{0};
};
}
BEGIN_FACTORY_DEF("NodSynth", "https://nodsynth.local", "mailto:dev@nodsynth.local")
DEF_CLASS2(INLINE_UID(0x45666650,0x726F6265,0x56335433,0x31323801),PClassInfo::kManyInstances,kVstAudioEffectClass,"EffectProbe",0,
    Steinberg::Vst::PlugType::kFx,"0.1.0",kVstVersionString,Steinberg::Vst::EffectProbe::createInstance)
END_FACTORY
