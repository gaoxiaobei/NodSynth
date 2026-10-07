#include <nodsynth/nodes/BuiltinNodes.h>
#include <nodsynth/nodes/Unison.h>
#include <nodsynth/song/AudioExport.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <iostream>
#include <numbers>

using namespace nodsynth;
namespace {
struct Probe {
    static constexpr std::uint32_t block=128;
    std::unique_ptr<runtime::DspNode> node;
    std::vector<float> input,output,parameters,gate,trigger;
    const float* triggerPointer;
    Probe(const char* type,double rate,std::initializer_list<double> values)
        :node(nodes::builtinImplementations().instantiate(model::NodeTypeId{type})),input(block),output(block),parameters(block*values.size()),gate(block),trigger(block),triggerPointer(trigger.data()) {
        std::size_t index=0;for(double value:values) std::fill_n(parameters.data()+index++*block,block,static_cast<float>(value));
        runtime::NodeBinding binding;binding.sampleRate=rate;binding.maxFrames=block;binding.voiceCount=1;
        binding.inputs={{{"audio-in"},input.data(),1,block,block,block},{{"gate"},gate.data(),1,block,block,block}};
        const auto registry=nodes::builtinRegistry();const auto* schema=registry.find(model::NodeTypeId{type});
        const auto port=std::find_if(schema->ports.begin(),schema->ports.end(),[](const auto& p){return p.direction==model::PortDirection::output;});
        binding.outputs={{port->id,output.data(),1,block,block,block}};
        binding.parameters={parameters.data(),static_cast<std::uint32_t>(values.size()),block};binding.triggers=&triggerPointer;node->bind(binding);
    }
};
std::vector<std::complex<double>> fft(const std::vector<float>& input) {
    std::vector<std::complex<double>> bins(input.begin(),input.end());const auto count=bins.size();
    for(std::size_t i=1,j=0;i<count;++i) {auto bit=count>>1;for(;j&bit;bit>>=1) j^=bit;j^=bit;if(i<j) std::swap(bins[i],bins[j]);}
    for(std::size_t length=2;length<=count;length*=2) {
        const auto step=std::polar(1.,-2*std::numbers::pi/length);
        for(std::size_t start=0;start<count;start+=length) {std::complex<double> phase{1,0};for(std::size_t offset=0;offset<length/2;++offset) {
            const auto a=bins[start+offset],b=bins[start+offset+length/2]*phase;bins[start+offset]=a+b;bins[start+offset+length/2]=a-b;phase*=step;
        }}
    }return bins;
}
persist::Json statistics(const std::vector<float>& audio) {
    using persist::Json;auto result=Json::object();double dc=0,energy=0,peak=0;
    for(float value:audio) {if(!std::isfinite(value)) throw std::runtime_error("nonfinite calibration output");dc+=value;energy+=value*value;peak=std::max(peak,std::fabs(static_cast<double>(value)));}
    result.set("peak",Json::number(peak));result.set("rms",Json::number(std::sqrt(energy/audio.size())));result.set("dc",Json::number(dc/audio.size()));return result;
}
persist::Json unisonModulationCalibration() {
    using persist::Json;auto measurements=Json::array();
    constexpr std::uint32_t count=4096,factor=8;
    for(auto rate:{44100u,48000u,96000u}) for(auto mode:{0,1,2}) {
        const double carrierBin=mode==0?1291:mode==1?431:127;
        const double modulationBin=mode==0?0:mode==1?97:431;
        const double deviationBin=mode==0?0:mode==1?80:100;
        std::vector<float> frequency(count),output(count*2),parameters(count*8);
        const std::array<float,8> defaults{1,0,0,0,0,1,123,440};
        for(std::uint32_t parameter=0;parameter<defaults.size();++parameter)
            std::fill_n(parameters.data()+parameter*count,count,defaults[parameter]);
        for(std::uint32_t sample=0;sample<count;++sample)
            frequency[sample]=static_cast<float>(rate*(carrierBin+deviationBin*std::sin(2*std::numbers::pi*modulationBin*sample/count))/count);
        auto node=nodes::makeUnisonOscillator();runtime::NodeBinding binding;
        binding.sampleRate=rate;binding.maxFrames=count;binding.voiceCount=1;
        binding.inputs={{{"frequency"},frequency.data(),1,count,count,count}};
        binding.outputs={{{"audio"},output.data(),2,count,count*2,count}};
        binding.parameters={parameters.data(),8,count};node->bind(binding);node->process(0,count);
        std::vector<float> reference(count*factor),direct(count);double phase=0;
        // Independent Fourier reference uses the same instantaneous harmonic budget, then removes out-of-band FM products.
        for(std::uint32_t sample=0;sample<count;++sample) {
            const double allowance=std::clamp(rate*.45/frequency[sample],1.,2048.);
            const auto upper=static_cast<std::uint32_t>(std::pow(2.,std::floor(std::log2(std::floor(allowance)))));
            const auto lower=upper>1?upper/2:1;
            const double blend=upper>1?std::clamp(allowance/upper-1,0.,1.):0;
            for(std::uint32_t sub=0;sub<factor;++sub) {
                double value=0;
                for(std::uint32_t harmonic=1;harmonic<=upper;++harmonic)
                    value-=2/std::numbers::pi*(harmonic<=lower?1:blend)*std::sin(2*std::numbers::pi*harmonic*phase)/harmonic;
                reference[sample*factor+sub]=static_cast<float>(value);
                if(!sub) direct[sample]=static_cast<float>(value);
                phase+=frequency[sample]/(static_cast<double>(rate)*factor);phase-=std::floor(phase);
            }
        }
        auto spectrum=fft(reference);double spectralEnergy=0,outOfBandEnergy=0;
        for(std::size_t bin=0;bin<spectrum.size();++bin) {
            const double energy=std::norm(spectrum[bin]);spectralEnergy+=energy;
            if(bin>count/2 && bin<spectrum.size()-count/2) {outOfBandEnergy+=energy;spectrum[bin]=0;}
        }
        std::vector<float> real(spectrum.size()),imaginary(spectrum.size());
        for(std::size_t bin=0;bin<spectrum.size();++bin) {real[bin]=static_cast<float>(spectrum[bin].real());imaginary[bin]=static_cast<float>(-spectrum[bin].imag());}
        const auto transformedReal=fft(real),transformedImaginary=fft(imaginary);
        double referenceEnergy=0,residualEnergy=0,lookupError=0;
        for(std::uint32_t sample=0;sample<count;++sample) {
            const auto index=sample*factor;
            const double filtered=(transformedReal[index].real()-transformedImaginary[index].imag())/spectrum.size();
            const double residual=output[sample]-filtered,lookup=output[sample]-direct[sample];
            if(!std::isfinite(residual) || !std::isfinite(lookup)) throw std::runtime_error("nonfinite Unison modulation calibration");
            referenceEnergy+=filtered*filtered;residualEnergy+=residual*residual;lookupError+=lookup*lookup;
        }
        if(std::sqrt(lookupError/count)>1e-5 || (mode==0 && residualEnergy/referenceEnergy>1e-9))
            throw std::runtime_error("Unison Fourier or static filtered reference calibration failed");
        auto item=statistics(std::vector<float>(output.begin(),output.begin()+count));
        item.set("sampleRate",Json::number(rate));item.set("mode",Json::string(mode==0?"static-high":mode==1?"fast-fm":"extreme-fm"));
        item.set("carrierHz",Json::number(rate*carrierBin/count));item.set("modulationHz",Json::number(rate*modulationBin/count));
        item.set("deviationHz",Json::number(rate*deviationBin/count));item.set("referenceFactor",Json::number(factor));
        item.set("referenceOutOfBandEnergyRatio",Json::number(outOfBandEnergy/spectralEnergy));
        item.set("filteredReferenceResidualEnergyRatio",Json::number(residualEnergy/referenceEnergy));
        item.set("fourierLookupErrorRms",Json::number(std::sqrt(lookupError/count)));
        measurements.push(std::move(item));
    }
    return measurements;
}
}
int main(int argc,char** argv) {
    using persist::Json;auto measurements=Json::array();
    try {
        for(auto rate:{44100u,48000u,96000u}) {
            for(int waveform=0;waveform<4;++waveform) for(auto bin:{19u,431u,1291u}) {
                constexpr std::uint32_t count=4096;const double frequency=static_cast<double>(rate)*bin/count;
                Probe probe("nod.oscillator",rate,{static_cast<double>(waveform),.5,frequency});std::vector<float> audio(count);
                for(std::uint32_t begin=0;begin<count*2;begin+=Probe::block) {probe.node->process(0,Probe::block);if(begin>=count) std::copy(probe.output.begin(),probe.output.end(),audio.begin()+begin-count);}
                const auto bins=fft(audio);double total=0,alias=0;
                for(std::size_t index=1;index<=count/2;++index) {const double energy=std::norm(bins[index]);total+=energy;if(index%bin || (waveform==0 && index!=bin) || (waveform==3 && index/bin%2==0)) alias+=energy;}
                auto item=statistics(audio);item.set("type",Json::string("nod.oscillator"));item.set("waveform",Json::number(waveform));item.set("sampleRate",Json::number(rate));
                item.set("frequencyHz",Json::number(frequency));item.set("unwantedEnergyRatio",Json::number(total>0?alias/total:0));measurements.push(std::move(item));
            }
            Probe noise("nod.noise",rate,{.5,1979});std::vector<float> noiseAudio(65536);
            for(std::size_t begin=0;begin<noiseAudio.size();begin+=Probe::block) {noise.node->process(0,Probe::block);std::copy(noise.output.begin(),noise.output.end(),noiseAudio.begin()+begin);}
            auto noiseItem=statistics(noiseAudio);noiseItem.set("type",Json::string("nod.noise"));noiseItem.set("sampleRate",Json::number(rate));measurements.push(std::move(noiseItem));
            Probe envelope("nod.adsr",rate,{.01,.02,.5,.04});std::vector<float> envelopeAudio(rate/4);
            const auto off=static_cast<std::uint32_t>(rate*.16);
            for(std::uint32_t begin=0;begin<envelopeAudio.size();begin+=Probe::block) {
                const auto frames=std::min<std::uint32_t>(Probe::block,envelopeAudio.size()-begin);
                for(std::uint32_t frame=0;frame<frames;++frame) envelope.gate[frame]=begin+frame<off?1:0;
                envelope.node->process(0,frames);std::copy_n(envelope.output.begin(),frames,envelopeAudio.begin()+begin);
            }
            const auto peak=std::max_element(envelopeAudio.begin(),envelopeAudio.end());
            const auto releaseEnd=std::find_if(envelopeAudio.begin()+off,envelopeAudio.end(),[](float value){return value==0;});
            auto envelopeItem=statistics(envelopeAudio);envelopeItem.set("type",Json::string("nod.adsr"));envelopeItem.set("sampleRate",Json::number(rate));
            envelopeItem.set("attackEndSample",Json::number(peak-envelopeAudio.begin()));envelopeItem.set("expectedAttackSamples",Json::number(rate*.01));
            envelopeItem.set("releaseSamples",Json::number(releaseEnd-envelopeAudio.begin()-off));envelopeItem.set("expectedReleaseSamples",Json::number(rate*.04));measurements.push(std::move(envelopeItem));
            for(const auto* type:{"nod.lowpass","nod.highpass","nod.lowpass-v2","nod.highpass-v2"}) for(double resonance:{0.,.5,.98}) for(double cutoff:{20.,1000.,20000.}) {
                Probe filter(type,rate,{cutoff,resonance});
                if(std::string(type).ends_with("-v2")) {
                    filter.parameters.resize(Probe::block*3,0);
                    runtime::NodeBinding binding;binding.sampleRate=rate;binding.maxFrames=Probe::block;binding.voiceCount=1;
                    binding.inputs={{{"audio-in"},filter.input.data(),1,Probe::block,Probe::block,Probe::block}};
                    binding.outputs={{{"audio-out"},filter.output.data(),1,Probe::block,Probe::block,Probe::block}};
                    binding.parameters={filter.parameters.data(),3,Probe::block};filter.node->bind(binding);
                }
                std::vector<float> audio(rate/2);double inputEnergy=0,outputEnergy=0;
                for(std::uint32_t begin=0;begin<audio.size();begin+=Probe::block) {
                    const auto frames=std::min<std::uint32_t>(Probe::block,audio.size()-begin);
                    for(std::uint32_t frame=0;frame<frames;++frame) filter.input[frame]=static_cast<float>(.01*std::sin(2*std::numbers::pi*std::min(cutoff,rate*.45)*(begin+frame)/rate));
                    filter.node->process(0,frames);std::copy_n(filter.output.begin(),frames,audio.begin()+begin);
                    for(std::uint32_t frame=0;frame<frames;++frame) if(begin+frame>rate/4) {inputEnergy+=filter.input[frame]*filter.input[frame];outputEnergy+=filter.output[frame]*filter.output[frame];}
                }
                auto item=statistics(audio);item.set("type",Json::string(type));item.set("sampleRate",Json::number(rate));item.set("cutoffHz",Json::number(cutoff));
                item.set("resonance",Json::number(resonance));
                item.set("cutoffResponseDb",outputEnergy>0 && inputEnergy>0?Json::number(10*std::log10(outputEnergy/inputEnergy)):Json::null());measurements.push(std::move(item));
            }
        }
        auto report=Json::object();report.set("schemaVersion",Json::number(1));report.set("scope",Json::string("Legacy v1 waveform level, coherent-bin alias/DC, deterministic noise, linear ADSR timing and filter cutoff/resonance calibration; measurements do not certify listening quality"));
        report.set("measurements",std::move(measurements));report.set("auditionStatus",Json::string("unheard"));
        report.set("unisonModulationMeasurements",unisonModulationCalibration());
        report.set("unisonModulationReference",Json::string("Independent Fourier banks with base-rate held frequency/harmonic budget; 8x sampled, FFT low-pass to base Nyquist and zero-phase reconstruction; residual includes aliases, table interpolation and modulation/bank update discontinuities; evaluation, not a claim of arbitrary FM alias suppression"));
        if(argc==2) {std::string error;if(!song::writeJsonAtomic(argv[1],report,error)) {std::cerr<<error;return 2;}}else std::cout<<report.dump()<<'\n';
    } catch(const std::exception& error) {std::cerr<<error.what();return 3;}return 0;
}
