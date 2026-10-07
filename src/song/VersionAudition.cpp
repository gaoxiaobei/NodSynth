#include <nodsynth/song/Workflow.h>
#include <nodsynth/song/AudioExport.h>
#include <nodsynth/runtime/WavFile.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>

namespace nodsynth::song {
namespace {
using persist::Json;
std::string pathText(const std::filesystem::path& path) {
    const auto text=std::filesystem::absolute(path).u8string();
    return {reinterpret_cast<const char*>(text.data()),text.size()};
}
bool barNumber(std::string_view text,std::uint32_t& number) {
    const auto [end,error]=std::from_chars(text.data(),text.data()+text.size(),number);
    return error==std::errc{} && end==text.data()+text.size() && number>0;
}
}

Json renderVersions(const std::filesystem::path& a,const std::filesystem::path& b,
    const std::filesystem::path& output,SongRenderOptions options,const std::string& bars,const std::string& section) {
    auto report=Json::object();
    const auto reject=[&](std::string error) {
        report.set("status",Json::string("rejected"));report.set("message",Json::string(std::move(error)));return report;
    };
    if (options.format!="float32" || !options.previewOutput.empty()) return reject("version audition requires float32 masters and manages its own previews");
    if (!bars.empty() && !section.empty()) return reject("select either bars or section");
    std::string error;
    const std::array paths{a,b};std::array<SongDocument,2> songs;
    std::array<SongRenderOptions,2> settings{options,options};
    std::array<std::string,2> sourceHashes;
    for (std::size_t i=0;i<2;++i) {
        auto song=loadSong(paths[i],error);if (!song) return reject(error);
        songs[i]=std::move(*song);sourceHashes[i]=hashFile(paths[i]);
        settings[i].baseDirectory=songs[i].baseDirectory;settings[i].songHash=sourceHashes[i];
        if (!section.empty()) {
            const auto found=std::find_if(songs[i].sections.begin(),songs[i].sections.end(),[&](const auto& s){return s.id==section;});
            if (found==songs[i].sections.end()) return reject("section missing in version "+std::to_string(i+1));
            settings[i].previewStartTick=found->startTick;settings[i].previewEndTick=found->endTick;
        } else if (!bars.empty()) {
            const auto colon=bars.find(':');std::uint32_t fromBar=0,toBar=0,from=0,to=0;
            if (colon==std::string::npos || !barNumber(std::string_view(bars).substr(0,colon),fromBar) ||
                !barNumber(std::string_view(bars).substr(colon+1),toBar) || !barsToTicks(songs[i],fromBar,toBar,from,to,error))
                return reject(error.empty()?"bars must be increasing START:END":error);
            settings[i].previewStartTick=from;settings[i].previewEndTick=to;
        }
        if (!validateRenderReady(songs[i],songs[i].baseDirectory).ok) return reject("version is not render-ready: "+pathText(paths[i]));
    }
    std::error_code failure;
    if (!std::filesystem::create_directory(output,failure)) return reject("output must be a new directory with an existing parent");
    const std::array names{std::string("A"),std::string("B")};
    std::array<SongRenderReport,2> renders;std::array<AudioAnalysis,2> analyses;
    double target=-20;
    for (std::size_t i=0;i<2;++i) {
        if (settings[i].cacheDirectory.empty()) settings[i].cacheDirectory=output/"cache";
        const auto raw=output/(names[i]+"-master.wav");
        renders[i]=renderSong(songs[i],settings[i],raw);
        if (!renders[i].ok) return reject(renders[i].message);
        if (!writeJsonAtomic(output/(names[i]+"-render.json"),songReportJson(renders[i]),error)) return reject(error);
        if (renders[i].tailTruncated) return reject("version tail is truncated; increase tail budget");
        analyses[i]=analyzeWav(raw);
        if (!analyses[i].ok || !analyses[i].loudnessLufs || !analyses[i].truePeakDbtp)
            return reject("version audio has no measurable loudness/true peak");
        target=std::min(target,*analyses[i].loudnessLufs-1.5-*analyses[i].truePeakDbtp);
    }
    auto entries=Json::array();
    for (std::size_t i=0;i<2;++i) {
        if (hashFile(paths[i])!=sourceHashes[i]) return reject("source version changed during rendering");
        const auto raw=output/(names[i]+"-master.wav"),matched=output/(names[i]+".wav");
        runtime::WavData audio;if (!runtime::readWav(raw,audio,error)) return reject(error);
        const auto original=audio.interleaved;double gainDb=target-*analyses[i].loudnessLufs;AudioAnalysis measured;
        for (unsigned attempt=0;attempt<8;++attempt) {
            const auto gain=std::pow(10.,gainDb/20.);
            for (std::size_t n=0;n<original.size();++n) audio.interleaved[n]=static_cast<float>(original[n]*gain);
            if (!runtime::writeWav(matched,audio,error)) return reject(error);
            measured=analyzeWav(matched);
            if (!measured.ok || !measured.loudnessLufs || !measured.truePeakDbtp) return reject("matched version analysis failed");
            if (std::abs(*measured.loudnessLufs-target)<.05) break;
            gainDb+=target-*measured.loudnessLufs;
        }
        if (std::abs(*measured.loudnessLufs-target)>.05 || *measured.truePeakDbtp> -1) return reject("loudness matching/headroom failed");
        auto entry=Json::object();entry.set("label",Json::string(names[i]));entry.set("song",Json::string(pathText(paths[i])));
        entry.set("sourceHash",Json::string(sourceHashes[i]));entry.set("revision",Json::number(static_cast<double>(songs[i].revision)));
        entry.set("audio",Json::string(pathText(matched)));entry.set("master",Json::string(pathText(raw)));
        entry.set("fileHash",Json::string(hashFile(matched)));entry.set("gainDb",Json::number(gainDb));
        entry.set("render",songReportJson(renders[i]));entry.set("analysis",analysisJson(measured));
        entry.set("auditionStatus",Json::string("unheard"));entries.push(std::move(entry));
    }
    report.set("status",Json::string("ok"));report.set("versions",std::move(entries));
    report.set("targetLufs",Json::number(target));report.set("diff",semanticDiff(songs[0],songs[1],false,false));
    report.set("orderPolicy",Json::string("A/B identifies versions, not a blind randomization"));
    if (!writeJsonAtomic(output/"manifest.json",report,error)) return reject(error);
    return report;
}
} // namespace nodsynth::song
