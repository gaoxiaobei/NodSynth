#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nodsynth::persist {
class Json {
public:
    enum class Kind { null, boolean, number, string, array, object };

    static Json null();
    static Json boolean(bool value);
    static Json number(double value);
    static Json string(std::string value);
    static Json array();
    static Json object();
    static std::optional<Json> parse(std::string_view text, std::string& error);

    [[nodiscard]] Kind kind() const noexcept { return kind_; }
    [[nodiscard]] bool isNull() const noexcept { return kind_ == Kind::null; }
    [[nodiscard]] bool asBool(bool fallback = false) const noexcept;
    [[nodiscard]] double asNumber(double fallback = 0.0) const noexcept;
    [[nodiscard]] const std::string& asString() const;
    [[nodiscard]] const std::vector<Json>& asArray() const;
    [[nodiscard]] const Json* find(std::string_view key) const noexcept;
    Json& set(std::string key, Json value);
    Json& push(Json value);
    [[nodiscard]] const std::vector<std::pair<std::string, Json>>& items() const noexcept { return items_; }
    [[nodiscard]] std::string dump(int indent = 2) const;

private:
    Kind kind_{Kind::null};
    bool boolean_{false};
    double number_{0.0};
    std::string text_;
    std::vector<Json> values_;
    std::vector<std::pair<std::string, Json>> items_;
};
} // namespace nodsynth::persist
