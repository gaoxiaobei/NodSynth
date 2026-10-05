#include <nodsynth/song/SongDocument.h>

#include <system_error>

namespace nodsynth::song {
std::filesystem::path resolveResourcePath(const std::filesystem::path& base, const std::string& stored) {
    const auto path = std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(stored.data()), stored.size()));
    return (path.is_absolute() ? path : base / path).lexically_normal();
}

persist::Json resourceDiagnostics(const SongDocument& song, const std::filesystem::path& base) {
    auto diagnostics = persist::Json::array();
    for (const auto& resource : song.resources) {
        const auto path = std::filesystem::absolute(resolveResourcePath(base, resource.path));
        std::error_code ec;
        const bool exists = std::filesystem::exists(path, ec);
        const auto actual = hashFile(path);
        std::string code;
        if (!exists && !ec) code = "missing-resource";
        else if (actual.empty()) code = "unreadable-resource";
        else if (!resource.hash.empty() && actual != resource.hash) code = "resource-hash-mismatch";
        else {
            const auto canonicalBase = std::filesystem::weakly_canonical(std::filesystem::absolute(base.empty() ? "." : base), ec);
            const auto canonicalPath = std::filesystem::weakly_canonical(path, ec);
            const auto relative = canonicalPath.lexically_relative(canonicalBase);
            if (resolveResourcePath({}, resource.path).is_absolute() || relative.empty() || *relative.begin() == "..")
                code = "non-portable";
        }
        if (code.empty()) continue;
        const auto utf8 = path.u8string();
        auto item = persist::Json::object();
        item.set("code", persist::Json::string(code));
        item.set("resourceId", persist::Json::string(resource.id));
        item.set("path", persist::Json::string(resource.path));
        item.set("pathBase", persist::Json::string("song-directory"));
        item.set("resolvedPath", persist::Json::string({reinterpret_cast<const char*>(utf8.data()), utf8.size()}));
        item.set("expectedHash", persist::Json::string(resource.hash));
        item.set("actualHash", actual.empty() ? persist::Json::null() : persist::Json::string(actual));
        diagnostics.push(std::move(item));
    }
    return diagnostics;
}
} // namespace nodsynth::song
