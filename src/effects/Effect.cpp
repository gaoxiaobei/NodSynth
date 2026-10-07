#include <nodsynth/effects/Effect.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <set>
#include <charconv>

namespace nodsynth::effects {
namespace {
const std::vector<Schema>& catalog() {
    static const std::vector<Schema> value{
        {"vst3",1,{}},
        {"delay", 1, {{"timeMs","ms",1,8000,375}, {"syncBeats","quarter-notes",0,16,0},
            {"feedback","linear",0,.95,.35}, {"pingPong","boolean",0,1,0},
            {"lowCut","Hz",20,20000,80}, {"highCut","Hz",20,20000,12000}, {"wet","linear",0,1,.25}}},
        {"reverb", 1, {{"preDelayMs","ms",0,250,20}, {"decay","seconds-RT60",.1,15,2},
            {"damping","Hz",200,20000,6000}, {"lowCut","Hz",20,20000,100},
            {"highCut","Hz",20,20000,16000}, {"wet","linear",0,1,.2}}},
        {"eq",1,{{"mode","0 bell, 1 highpass, 2 lowpass, 3 lowshelf, 4 highshelf",0,4,0,false},
            {"frequency","Hz",20,20000,1000},{"gainDb","dB",-24,24,0},{"q","ratio",.1,20,.7071067811865476},{"wet","linear",0,1,1}}},
        {"compressor",1,{{"thresholdDb","dBFS",-60,0,-18},{"ratio","ratio",1,20,4},
            {"attackMs","ms",.1,200,10},{"releaseMs","ms",5,2000,100},{"kneeDb","dB",0,24,6},
            {"makeupDb","dB",0,24,0},{"detectorHighpass","Hz (0 bypass)",0,2000,80},{"wet","linear",0,1,1}}},
        {"limiter",1,{{"ceilingDb","dBTP",-24,0,-1},{"lookaheadMs","ms",1,20,5,false},
            {"releaseMs","ms",5,2000,100}}},
        {"saturation",1,{{"driveDb","dB",0,36,6},{"outputDb","dB",-24,6,-6},{"wet","linear",0,1,1}}},
    };
    return value;
}
double safe(double value, double fallback) noexcept { return std::isfinite(value) ? value : fallback; }
class Base : public Effect {
public:
    bool initialize(double rate, std::uint32_t frames, const Config& config, std::string& error) {
        if (!std::isfinite(rate) || rate < 8000 || rate > 192000 || !frames || frames > 8192 || !validateConfig(config,error)) {
            if (error.empty()) error = "effect rate/block outside 8000..192000 Hz / 1..8192 frames";
            return false;
        }
        schema_ = findSchema(config.type); rate_ = rate; frames_ = frames;
        for (std::size_t i = 0; i < schema_->parameters.size(); ++i) {
            const auto found = config.parameters.find(schema_->parameters[i].id);
            values_[i] = found == config.parameters.end() ? schema_->parameters[i].defaultValue : found->second;
        }
        bypass_ = config.bypass; bypassMix_ = bypass_ ? 1 : 0;
        return true;
    }
    void setBypass(bool bypass) noexcept override { bypass_ = bypass; }
    std::uint32_t latencySamples() const noexcept override { return 0; }
    void event(const ParameterEvent& event) noexcept {
        if (event.parameter >= schema_->parameters.size()) return;
        const auto& parameter = schema_->parameters[event.parameter];
        if (parameter.automatable && std::isfinite(event.value)) values_[event.parameter] = std::clamp(event.value,parameter.minimum,parameter.maximum);
    }
    void output(float& l, float& r, double dryL, double dryR, double wetL, double wetR, double wet) noexcept {
        bypassMix_ += std::clamp((bypass_ ? 1.0 : 0.0) - bypassMix_, -1 / (rate_ * .005), 1 / (rate_ * .005));
        const double amount = wet * (1 - bypassMix_);
        l = static_cast<float>(dryL * (1 - amount) + wetL * amount);
        r = static_cast<float>(dryR * (1 - amount) + wetR * amount);
    }
protected:
    const Schema* schema_{nullptr};
    double rate_{48000}, bypassMix_{0};
    std::uint32_t frames_{128};
    std::array<double,16> values_{};
    bool bypass_{false};
};

struct OnePole {
    double state{0};
    double low(double input, double hz, double rate) noexcept {
        const auto coefficient = 1 - std::exp(-2 * std::numbers::pi * std::min(hz,rate*.45) / rate);
        state += coefficient * (input - state); return state;
    }
};
class Equalizer final : public Base {
public:
    bool prepare(double rate,std::uint32_t frames,const Config& config,std::string& error) override {
        if(!initialize(rate,frames,config,error)) return false;coefficients();reset();return true;
    }
    void reset() noexcept override {states_={};bypassMix_=bypass_?1:0;}
    std::uint64_t committedBytes() const noexcept override {return sizeof(*this);}
    double tailSeconds() const noexcept override {
        const double discriminant=a1_*a1_-4*a2_;
        const double radius=discriminant>=0?std::max(std::fabs((-a1_+std::sqrt(discriminant))*.5),std::fabs((-a1_-std::sqrt(discriminant))*.5)):std::sqrt(std::max(0.0,a2_));
        return radius>0 && radius<1 ? std::min(30.0,std::log(.00001)/std::log(radius)/rate_+.01):.01;
    }
    void process(const Block& block) noexcept override {
        if(!block.left || !block.right || block.frames>frames_) return;
        std::size_t cursor=0;
        for(std::uint32_t frame=0;frame<block.frames;++frame) {
            bool changed=false;
            while(cursor<block.parameters.size() && block.parameters[cursor].sampleOffset<=frame) {event(block.parameters[cursor++]);changed=true;}
            if(changed) coefficients();
            const double dry[2]{safe(block.left[frame],0),safe(block.right[frame],0)};double wet[2]{};
            for(int channel=0;channel<2;++channel) {
                auto& state=states_[channel];wet[channel]=b0_*dry[channel]+state[0];
                state[0]=b1_*dry[channel]-a1_*wet[channel]+state[1];state[1]=b2_*dry[channel]-a2_*wet[channel];
            }
            output(block.left[frame],block.right[frame],dry[0],dry[1],wet[0],wet[1],values_[4]);
        }
    }
private:
    void coefficients() noexcept {
        // RBJ Audio EQ Cookbook, normalized transposed direct form II.
        const double w=2*std::numbers::pi*std::min(values_[1],rate_*.45)/rate_,c=std::cos(w),s=std::sin(w);
        const double alpha=s/(2*values_[3]),a=std::pow(10.0,values_[2]/40),beta=2*std::sqrt(a)*alpha;
        double a0=1;
        switch(static_cast<int>(std::lround(values_[0]))) {
        case 1:b0_=(1+c)*.5;b1_=-(1+c);b2_=b0_;a0=1+alpha;a1_=-2*c;a2_=1-alpha;break;
        case 2:b0_=(1-c)*.5;b1_=1-c;b2_=b0_;a0=1+alpha;a1_=-2*c;a2_=1-alpha;break;
        case 3:
            b0_=a*((a+1)-(a-1)*c+beta);b1_=2*a*((a-1)-(a+1)*c);b2_=a*((a+1)-(a-1)*c-beta);
            a0=(a+1)+(a-1)*c+beta;a1_=-2*((a-1)+(a+1)*c);a2_=(a+1)+(a-1)*c-beta;break;
        case 4:
            b0_=a*((a+1)+(a-1)*c+beta);b1_=-2*a*((a-1)+(a+1)*c);b2_=a*((a+1)+(a-1)*c-beta);
            a0=(a+1)-(a-1)*c+beta;a1_=2*((a-1)-(a+1)*c);a2_=(a+1)-(a-1)*c-beta;break;
        default:b0_=1+alpha*a;b1_=-2*c;b2_=1-alpha*a;a0=1+alpha/a;a1_=-2*c;a2_=1-alpha/a;break;
        }
        b0_/=a0;b1_/=a0;b2_/=a0;a1_/=a0;a2_/=a0;
    }
    double b0_{1},b1_{0},b2_{0},a1_{0},a2_{0};
    std::array<std::array<double,2>,2> states_{};
};
class Delay final : public Base {
public:
    bool prepare(double rate,std::uint32_t frames,const Config& config,std::string& error) override {
        if (!initialize(rate,frames,config,error)) return false;
        for (auto& buffer : ring_) buffer.resize(static_cast<std::size_t>(rate * 8) + 2);
        reset(); return true;
    }
    void reset() noexcept override {
        for (auto& buffer : ring_) std::fill(buffer.begin(),buffer.end(),0);
        index_=0; low_={}; high_={}; currentTime_=0; bypassMix_=bypass_?1:0;
    }
    double tailSeconds() const noexcept override {
        const double seconds = values_[1] > 0 ? 8 : values_[0] / 1000;
        return seconds * (values_[2] > 0 ? 1 + std::log(.00001) / std::log(values_[2]) : 1) + .5;
    }
    std::uint64_t committedBytes() const noexcept override { return sizeof(*this) + ring_[0].capacity()*sizeof(double)*2; }
    void process(const Block& block) noexcept override {
        if (!block.left || !block.right || block.frames > frames_) return;
        std::size_t cursor=0;
        for (std::uint32_t frame=0;frame<block.frames;++frame) {
            while (cursor<block.parameters.size() && block.parameters[cursor].sampleOffset<=frame) event(block.parameters[cursor++]);
            const double bpm = block.bpm ? std::clamp(safe(block.bpm[frame],120),20.0,400.0) : 120;
            const double target = std::clamp(values_[1]>0 ? values_[1]*60/bpm : values_[0]/1000,.001,8.0)*rate_;
            if (currentTime_ == 0) currentTime_=target;
            currentTime_ += (1-std::exp(-1/(rate_*.02)))*(target-currentTime_);
            const double position = std::fmod(index_ + ring_[0].size() - currentTime_,ring_[0].size());
            const auto first = static_cast<std::size_t>(position), next=(first+1)%ring_[0].size();
            const double frac = position-first;
            const double l=safe(block.left[frame],0), r=safe(block.right[frame],0);
            double delayed[2]{};
            for (int channel=0;channel<2;++channel) delayed[channel]=ring_[channel][first]*(1-frac)+ring_[channel][next]*frac;
            for (int channel=0;channel<2;++channel) {
                const double echo=delayed[values_[3]>=.5 ? 1-channel : channel];
                const double filtered = high_[channel].low(echo,values_[5],rate_);
                const double band = filtered-low_[channel].low(filtered,values_[4],rate_);
                ring_[channel][index_] = (channel ? r : l) + band*values_[2];
            }
            index_=(index_+1)%ring_[0].size();
            output(block.left[frame],block.right[frame],l,r,delayed[0],delayed[1],values_[6]);
        }
    }
private:
    std::array<std::vector<double>,2> ring_;
    std::array<OnePole,2> low_{},high_{};
    std::size_t index_{0};
    double currentTime_{0};
};
class Compressor final : public Base {
public:
    bool prepare(double rate,std::uint32_t frames,const Config& config,std::string& error) override {
        if(!initialize(rate,frames,config,error)) return false;external_=!config.sidechain.empty();reset();return true;
    }
    void reset() noexcept override {detector_={};gainDb_=0;bypassMix_=bypass_?1:0;}
    double tailSeconds() const noexcept override {return 0;}
    std::uint64_t committedBytes() const noexcept override {return sizeof(*this);}
    void process(const Block& block) noexcept override {
        if(!block.left || !block.right || block.frames>frames_) return;
        std::size_t cursor=0;
        for(std::uint32_t frame=0;frame<block.frames;++frame) {
            while(cursor<block.parameters.size() && block.parameters[cursor].sampleOffset<=frame) event(block.parameters[cursor++]);
            const double l=safe(block.left[frame],0),r=safe(block.right[frame],0);
            const double detector[2]{external_?(block.sidechainLeft?safe(block.sidechainLeft[frame],0):0):l,
                external_?(block.sidechainRight?safe(block.sidechainRight[frame],0):0):r};
            double level=0;
            for(int channel=0;channel<2;++channel) {
                const double high=values_[6]>0?detector[channel]-detector_[channel].low(detector[channel],values_[6],rate_):detector[channel];
                level=std::max(level,std::fabs(high));
            }
            const double inputDb=20*std::log10(std::max(1e-12,level)),over=inputDb-values_[0],slope=1/values_[1]-1,knee=values_[4];
            double target=0;
            if(over>=knee*.5) target=slope*over;
            else if(knee>0 && over>-knee*.5) target=slope*(over+knee*.5)*(over+knee*.5)/(2*knee);
            const double coefficient=std::exp(-1/(rate_*(target<gainDb_?values_[2]:values_[3])/1000));
            gainDb_=coefficient*gainDb_+(1-coefficient)*target;
            const double gain=std::pow(10.0,(gainDb_+values_[5])/20);
            output(block.left[frame],block.right[frame],l,r,l*gain,r*gain,values_[7]);
        }
    }
private:
    bool external_{false};double gainDb_{0};std::array<OnePole,2> detector_{};
};

class Saturation final : public Base {
public:
    bool prepare(double rate,std::uint32_t frames,const Config& config,std::string& error) override {
        if(!initialize(rate,frames,config,error)) return false;
        factor_=config.quality=="high"?4:2;taps_=32*factor_+1;
        double sum=0;
        for(std::size_t tap=0;tap<taps_;++tap) {
            const double offset=static_cast<double>(tap)-(taps_-1)*.5,cutoff=.45/factor_;
            const double sinc=offset==0?2*cutoff:std::sin(2*std::numbers::pi*cutoff*offset)/(std::numbers::pi*offset);
            coefficients_[tap]=sinc*(.42-.5*std::cos(2*std::numbers::pi*tap/(taps_-1))+.08*std::cos(4*std::numbers::pi*tap/(taps_-1)));
            sum+=coefficients_[tap];
        }
        for(std::size_t tap=0;tap<taps_;++tap) coefficients_[tap]/=sum;
        reset();return true;
    }
    void reset() noexcept override {input_={};shaped_={};dry_={};position_=0;dryPosition_=0;bypassMix_=bypass_?1:0;}
    std::uint32_t latencySamples() const noexcept override {return 32;}
    double tailSeconds() const noexcept override {return 64/rate_;}
    std::uint64_t committedBytes() const noexcept override {return sizeof(*this);}
    void process(const Block& block) noexcept override {
        if(!block.left || !block.right || block.frames>frames_) return;
        std::size_t cursor=0;
        for(std::uint32_t frame=0;frame<block.frames;++frame) {
            while(cursor<block.parameters.size() && block.parameters[cursor].sampleOffset<=frame) event(block.parameters[cursor++]);
            const std::array<double,2> input{safe(block.left[frame],0),safe(block.right[frame],0)};
            const auto dry=dry_[dryPosition_];dry_[dryPosition_]=input;dryPosition_=(dryPosition_+1)%dry_.size();
            const double drive=std::pow(10.,values_[0]/20),gain=std::pow(10.,values_[1]/20);
            std::array<double,2> wet{};
            // Zero insertion, linear-phase interpolation, tanh, then matched antialias decimation.
            for(std::uint32_t phase=0;phase<factor_;++phase) {
                for(std::size_t channel=0;channel<2;++channel) {
                    input_[channel][position_]=phase==0?input[channel]*factor_:0;
                    shaped_[channel][position_]=std::tanh(drive*convolve(input_[channel]));
                    if(phase==0) wet[channel]=convolve(shaped_[channel])*gain;
                }
                position_=(position_+1)%taps_;
            }
            output(block.left[frame],block.right[frame],dry[0],dry[1],wet[0],wet[1],values_[2]);
        }
    }
private:
    double convolve(const std::array<double,129>& history) const noexcept {
        double sum=0;for(std::size_t tap=0;tap<taps_;++tap) sum+=coefficients_[tap]*history[(position_+taps_-tap)%taps_];return sum;
    }
    std::array<double,129> coefficients_{};
    std::array<std::array<double,129>,2> input_{},shaped_{};
    std::array<std::array<double,2>,32> dry_{};
    std::size_t position_{0},dryPosition_{0},taps_{65};std::uint32_t factor_{2};
};

class Limiter final : public Base {
public:
    bool prepare(double rate,std::uint32_t frames,const Config& config,std::string& error) override {
        if(!initialize(rate,frames,config,error)) return false;
        latency_=static_cast<std::uint32_t>(std::ceil(rate*values_[1]/1000))+25;
        audio_.resize(latency_+1);caps_.resize(latency_+1);filters_.clear();
        // Hann-windowed sinc polyphases use the calibrated libebur128 detector kernel.
        // Absolute convolution bounds protect every contributing output sample, including gain changes.
        addFilters(49,rate<96000?4:rate<192000?2:1);
        if(config.quality=="high") addFilters(97,8);
        reset();return true;
    }
    void reset() noexcept override {std::fill(audio_.begin(),audio_.end(),std::array<double,2>{});std::fill(caps_.begin(),caps_.end(),1);index_=0;gain_=1;bypassMix_=bypass_?1:0;}
    std::uint32_t latencySamples() const noexcept override {return latency_;}
    double tailSeconds() const noexcept override {return 0;}
    std::uint64_t committedBytes() const noexcept override {
        std::uint64_t bytes=sizeof(*this)+audio_.capacity()*sizeof(audio_[0])+caps_.capacity()*sizeof(double);
        for(const auto& filter:filters_) bytes+=sizeof(filter)+filter.capacity()*sizeof(double);return bytes;
    }
    void process(const Block& block) noexcept override {
        if(!block.left || !block.right || block.frames>frames_) return;
        std::size_t cursor=0;
        for(std::uint32_t frame=0;frame<block.frames;++frame) {
            while(cursor<block.parameters.size() && block.parameters[cursor].sampleOffset<=frame) event(block.parameters[cursor++]);
            audio_[index_]={safe(block.left[frame],0),safe(block.right[frame],0)};caps_[index_]=1;
            const double ceiling=std::pow(10.,values_[0]/20)*.999999;
            double bound=std::max(std::fabs(audio_[index_][0]),std::fabs(audio_[index_][1]));std::size_t history=1;
            for(const auto& filter:filters_) {
                history=std::max(history,filter.size());
                for(int channel=0;channel<2;++channel) {
                    double value=0;for(std::size_t tap=0;tap<filter.size();++tap) value+=std::fabs(filter[tap]*audio_[(index_+audio_.size()-tap)%audio_.size()][channel]);
                    bound=std::max(bound,value);
                }
            }
            const double cap=bound>ceiling?ceiling/bound:1;
            for(std::size_t tap=0;tap<history;++tap) {auto& limit=caps_[(index_+audio_.size()-tap)%audio_.size()];limit=std::min(limit,cap);}
            const auto read=(index_+1)%audio_.size();
            const double release=std::exp(-1/(rate_*values_[2]/1000));gain_=std::min(caps_[read],1-(1-gain_)*release);
            output(block.left[frame],block.right[frame],audio_[read][0],audio_[read][1],audio_[read][0]*gain_,audio_[read][1]*gain_,1);
            index_=read;
        }
    }
private:
    void addFilters(std::uint32_t taps,std::uint32_t factor) {
        if(factor==1) return;
        for(std::uint32_t phase=0;phase<factor;++phase) {
            std::vector<double> filter;
            for(std::uint32_t tap=phase;tap<taps;tap+=factor) {
                const double m=static_cast<double>(tap)-(taps-1)*.5,x=m*std::numbers::pi/factor;
                filter.push_back((m==0?1:std::sin(x)/x)*.5*(1-std::cos(2*std::numbers::pi*tap/(taps-1))));
            }filters_.push_back(std::move(filter));
        }
    }
    std::vector<std::array<double,2>> audio_;std::vector<double> caps_;std::vector<std::vector<double>> filters_;
    std::size_t index_{0};std::uint32_t latency_{0};double gain_{1};
};

class Reverb final : public Base {
public:
    bool prepare(double rate,std::uint32_t frames,const Config& config,std::string& error) override {
        if (!initialize(rate,frames,config,error)) return false;
        // Incommensurate delay lengths, orthogonal Householder feedback (Jot/Smith FDN).
        constexpr std::array<double,8> times{.0371,.0411,.0437,.0479,.0533,.0593,.0671,.0797};
        for (std::size_t i=0;i<8;++i) lines_[i].resize(static_cast<std::size_t>(times[i]*rate)|1u);
        for (auto& buffer : predelay_) buffer.resize(static_cast<std::size_t>(rate*.25)+2);
        reset(); return true;
    }
    void reset() noexcept override {
        for (auto& line:lines_) std::fill(line.begin(),line.end(),0);
        for (auto& line:predelay_) std::fill(line.begin(),line.end(),0);
        positions_={}; damping_={}; inputLow_={}; inputHigh_={}; preIndex_=0; bypassMix_=bypass_?1:0;
    }
    double tailSeconds() const noexcept override { return values_[0]/1000+values_[1]*2+.2; }
    std::uint64_t committedBytes() const noexcept override {
        std::uint64_t bytes=sizeof(*this);
        for (const auto& line:lines_) bytes+=line.capacity()*sizeof(double);
        for (const auto& line:predelay_) bytes+=line.capacity()*sizeof(double);
        return bytes;
    }
    void process(const Block& block) noexcept override {
        if (!block.left || !block.right || block.frames>frames_) return;
        std::size_t cursor=0;
        constexpr std::array<double,8> leftSigns{1,1,1,1,-1,-1,-1,-1}, rightSigns{1,-1,1,-1,1,-1,1,-1};
        constexpr double scale=.3535533905932737622;
        for (std::uint32_t frame=0;frame<block.frames;++frame) {
            while (cursor<block.parameters.size() && block.parameters[cursor].sampleOffset<=frame) event(block.parameters[cursor++]);
            const double dry[2]{safe(block.left[frame],0),safe(block.right[frame],0)};
            double input[2]{};
            const auto delay = static_cast<std::size_t>(values_[0]*rate_/1000);
            for (int c=0;c<2;++c) {
                const double filtered=inputHigh_[c].low(dry[c],values_[4],rate_);
                const double value=filtered-inputLow_[c].low(filtered,values_[3],rate_);
                predelay_[c][preIndex_]=value;
                input[c]=predelay_[c][(preIndex_+predelay_[c].size()-delay)%predelay_[c].size()];
            }
            preIndex_=(preIndex_+1)%predelay_[0].size();
            std::array<double,8> read{};
            double sum=0,l=0,r=0;
            for (std::size_t i=0;i<8;++i) {
                const double value=lines_[i][positions_[i]];
                l+=value*rightSigns[i]*scale; r+=value*leftSigns[i]*scale;
                read[i]=damping_[i].low(value,values_[2],rate_); sum+=read[i];
            }
            for (std::size_t i=0;i<8;++i) {
                const double feedback=std::exp(-std::log(1000.0)*lines_[i].size()/(rate_*values_[1]));
                lines_[i][positions_[i]]=(read[i]-sum*.25)*feedback + (input[0]*leftSigns[i]+input[1]*rightSigns[i])*scale;
                positions_[i]=(positions_[i]+1)%lines_[i].size();
            }
            output(block.left[frame],block.right[frame],dry[0],dry[1],l,r,values_[5]);
        }
    }
private:
    std::array<std::vector<double>,8> lines_;
    std::array<std::vector<double>,2> predelay_;
    std::array<std::size_t,8> positions_{};
    std::array<OnePole,8> damping_{};
    std::array<OnePole,2> inputLow_{},inputHigh_{};
    std::size_t preIndex_{0};
};
}

std::vector<Schema> schemas() { return catalog(); }
const Schema* findSchema(const std::string& type) {
    const auto& entries=catalog();
    const auto found=std::find_if(entries.begin(),entries.end(),[&](const auto& s){return s.type==type;});
    return found==entries.end()?nullptr:&*found;
}
bool validateConfig(const Config& config,std::string& error) {
    const auto* schema=findSchema(config.type);
    if (!schema || config.version!=schema->version) { error="unknown-effect: unsupported effect type/version"; return false; }
    if(config.type=="vst3") {
        if(config.pluginResource.empty() || config.quality!="standard" || !config.sidechain.empty() || config.workerTimeoutMs<100 || config.workerTimeoutMs>300000 ||
            !std::isfinite(config.declaredTailSeconds) || config.declaredTailSeconds< -1 || config.declaredTailSeconds>3600 || config.automation.size()>16) {
            error="vst3-effect-config: plugin resource, stereo main input, standard quality and bounded timeout/tail are required";return false;
        }
        const auto valid=[](const std::string& address) {
            if(!address.starts_with("vst3:")) return false;std::uint32_t id=0;
            const auto parsed=std::from_chars(address.data()+5,address.data()+address.size(),id);
            return parsed.ec==std::errc{} && parsed.ptr==address.data()+address.size() && address=="vst3:"+std::to_string(id);
        };
        for(const auto& [address,value]:config.parameters) if(!valid(address) || !std::isfinite(value) || value<0 || value>1) {error="vst3-effect-parameter: requires stable ID and normalized value";return false;}
        std::set<std::string> assigned;
        for(const auto& lane:config.automation) {
            if(!valid(lane.parameter) || !assigned.insert(lane.parameter).second || (lane.interpolation!="step" && lane.interpolation!="linear")) {error="vst3-effect-automation: invalid or duplicate lane";return false;}
            for(std::size_t index=0;index<lane.points.size();++index) if(!std::isfinite(lane.points[index].value) || lane.points[index].value<0 || lane.points[index].value>1 ||
                (index && lane.points[index].tick<=lane.points[index-1].tick)) {error="vst3-effect-automation: invalid points";return false;}
        }return true;
    }
    if(!config.pluginResource.empty() || !config.stateResource.empty() || !config.className.empty() || config.declaredTailSeconds!=-1) {error="effect-config: plugin fields apply only to VST3";return false;}
    if (config.quality!="standard" && !((config.type=="limiter" || config.type=="saturation") && config.quality=="high")) { error="effect-quality: unsupported quality mode"; return false; }
    if (!config.sidechain.empty() && config.type!="compressor") { error="unsupported-sidechain: this effect has no detector input"; return false; }
    for (const auto& [id,value]:config.parameters) {
        const auto parameter=std::find_if(schema->parameters.begin(),schema->parameters.end(),[&](const auto& p){return p.id==id;});
        if (parameter==schema->parameters.end() || !std::isfinite(value) || value<parameter->minimum || value>parameter->maximum ||
            (!parameter->automatable && value!=std::floor(value))) { error="effect-parameter: unknown or invalid parameter "+id; return false; }
    }
    std::set<std::string> automated;
    for(const auto& lane:config.automation) {
        const auto parameter=std::find_if(schema->parameters.begin(),schema->parameters.end(),[&](const auto& p){return p.id==lane.parameter;});
        if(parameter==schema->parameters.end() || !parameter->automatable || !automated.insert(lane.parameter).second ||
            (lane.interpolation!="step" && lane.interpolation!="linear")) {error="effect-automation: unknown, unsupported or duplicated target";return false;}
        for(std::size_t index=0;index<lane.points.size();++index) {
            const auto& point=lane.points[index];
            if(!std::isfinite(point.value) || point.value<parameter->minimum || point.value>parameter->maximum ||
                (index && point.tick<=lane.points[index-1].tick)) {error="effect-automation: points must be ordered and in physical range";return false;}
        }
    }
    return true;
}
std::unique_ptr<Effect> makeEffect(const std::string& type) {
    if (type=="delay") return std::make_unique<Delay>();
    if (type=="reverb") return std::make_unique<Reverb>();
    if(type=="eq") return std::make_unique<Equalizer>();
    if(type=="compressor") return std::make_unique<Compressor>();
    if(type=="limiter") return std::make_unique<Limiter>();
    if(type=="saturation") return std::make_unique<Saturation>();
    return {};
}
std::uint64_t stateBudget(const std::string& type,double rate) noexcept {
    if(!std::isfinite(rate) || rate<8000 || rate>192000) return UINT64_MAX;
    if(type=="delay") return static_cast<std::uint64_t>(rate*8+2)*sizeof(double)*2+4096;
    if(type=="reverb") return static_cast<std::uint64_t>(rate*1.5+32)*sizeof(double)+4096;
    if(type=="eq") return 4096;
    if(type=="compressor") return 4096;
    if(type=="saturation") return 8192;
    if(type=="limiter") return static_cast<std::uint64_t>(std::ceil(rate*.02)+26)*24+8192;
    if(type=="vst3") return 8ull*1024*1024;
    return UINT64_MAX;
}
}
