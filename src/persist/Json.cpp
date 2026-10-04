#include <nodsynth/persist/Json.h>

#include <cmath>
#include <sstream>

namespace nodsynth::persist {
namespace {
class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    std::optional<Json> parse(std::string& error) {
        skip();
        auto value = parseValue(error);
        if (!value) return std::nullopt;
        skip();
        if (index_ != text_.size()) {
            error = "unexpected trailing JSON";
            return std::nullopt;
        }
        return value;
    }

private:
    std::optional<Json> parseValue(std::string& error) {
        skip();
        if (index_ >= text_.size()) {
            error = "unexpected end of JSON";
            return std::nullopt;
        }
        const char character = text_[index_];
        if (character == '{') return parseObject(error);
        if (character == '[') return parseArray(error);
        if (character == '"') return parseString(error);
        if (character == 't' || character == 'f') return parseBool(error);
        if (character == 'n') return parseNull(error);
        if (character == '-' || (character >= '0' && character <= '9')) return parseNumber(error);
        error = "invalid JSON value";
        return std::nullopt;
    }

    std::optional<Json> parseObject(std::string& error) {
        ++index_;
        auto object = Json::object();
        skip();
        if (consume('}')) return object;
        while (index_ < text_.size()) {
            auto key = parseString(error);
            if (!key) return std::nullopt;
            skip();
            if (!consume(':')) {
                error = "JSON object is missing a colon";
                return std::nullopt;
            }
            auto value = parseValue(error);
            if (!value) return std::nullopt;
            object.set(key->asString(), std::move(*value));
            skip();
            if (consume('}')) return object;
            if (!consume(',')) {
                error = "JSON object is missing a comma";
                return std::nullopt;
            }
            skip();
        }
        error = "unterminated JSON object";
        return std::nullopt;
    }

    std::optional<Json> parseArray(std::string& error) {
        ++index_;
        auto array = Json::array();
        skip();
        if (consume(']')) return array;
        while (index_ < text_.size()) {
            auto value = parseValue(error);
            if (!value) return std::nullopt;
            array.push(std::move(*value));
            skip();
            if (consume(']')) return array;
            if (!consume(',')) {
                error = "JSON array is missing a comma";
                return std::nullopt;
            }
            skip();
        }
        error = "unterminated JSON array";
        return std::nullopt;
    }

    std::optional<Json> parseString(std::string& error) {
        if (!consume('"')) {
            error = "JSON string expected";
            return std::nullopt;
        }
        std::string value;
        while (index_ < text_.size()) {
            const char character = text_[index_++];
            if (character == '"') return Json::string(std::move(value));
            if (character == '\\') {
                if (index_ >= text_.size()) break;
                const char escaped = text_[index_++];
                switch (escaped) {
                    case '"':
                    case '\\':
                    case '/': value.push_back(escaped); break;
                    case 'b': value.push_back('\b'); break;
                    case 'f': value.push_back('\f'); break;
                    case 'n': value.push_back('\n'); break;
                    case 'r': value.push_back('\r'); break;
                    case 't': value.push_back('\t'); break;
                    case 'u':
                        if (index_ + 4 > text_.size()) {
                            error = "short unicode escape";
                            return std::nullopt;
                        }
                        value.append(text_.substr(index_ - 2, 6));
                        index_ += 4;
                        break;
                    default: error = "invalid JSON escape"; return std::nullopt;
                }
            } else value.push_back(character);
        }
        error = "unterminated JSON string";
        return std::nullopt;
    }

    std::optional<Json> parseBool(std::string& error) {
        if (text_.substr(index_, 4) == "true") {
            index_ += 4;
            return Json::boolean(true);
        }
        if (text_.substr(index_, 5) == "false") {
            index_ += 5;
            return Json::boolean(false);
        }
        error = "invalid JSON boolean";
        return std::nullopt;
    }

    std::optional<Json> parseNull(std::string& error) {
        if (text_.substr(index_, 4) != "null") {
            error = "invalid JSON null";
            return std::nullopt;
        }
        index_ += 4;
        return Json::null();
    }

    std::optional<Json> parseNumber(std::string& error) {
        const auto start = index_;
        if (consume('-')) {}
        if (index_ >= text_.size() || text_[index_] < '0' || text_[index_] > '9') {
            error = "invalid JSON number";
            return std::nullopt;
        }
        while (index_ < text_.size() && text_[index_] >= '0' && text_[index_] <= '9') ++index_;
        if (consume('.')) {
            if (index_ >= text_.size() || text_[index_] < '0' || text_[index_] > '9') {
                error = "invalid JSON number";
                return std::nullopt;
            }
            while (index_ < text_.size() && text_[index_] >= '0' && text_[index_] <= '9') ++index_;
        }
        if (index_ < text_.size() && (text_[index_] == 'e' || text_[index_] == 'E')) {
            ++index_;
            if (index_ < text_.size() && (text_[index_] == '+' || text_[index_] == '-')) ++index_;
            if (index_ >= text_.size() || text_[index_] < '0' || text_[index_] > '9') {
                error = "invalid JSON exponent";
                return std::nullopt;
            }
            while (index_ < text_.size() && text_[index_] >= '0' && text_[index_] <= '9') ++index_;
        }
        try {
            const auto value = std::stod(std::string{text_.substr(start, index_ - start)});
            if (!std::isfinite(value)) {
                error = "non-finite JSON number";
                return std::nullopt;
            }
            return Json::number(value);
        } catch (const std::exception&) {
            error = "JSON number is out of range";
            return std::nullopt;
        }
    }

    void skip() {
        while (index_ < text_.size() && (text_[index_] == ' ' || text_[index_] == '\n' || text_[index_] == '\r' ||
                                          text_[index_] == '\t')) {
            ++index_;
        }
    }
    bool consume(char character) {
        if (index_ < text_.size() && text_[index_] == character) {
            ++index_;
            return true;
        }
        return false;
    }

    std::string_view text_;
    std::size_t index_{0};
};

void dumpString(std::ostream& out, std::string_view text) {
    out << '"';
    for (const unsigned char character : text) {
        switch (character) {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\b': out << "\\b"; break;
            case '\f': out << "\\f"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (character < 0x20) {
                    constexpr char hex[] = "0123456789abcdef";
                    out << "\\u00" << hex[character >> 4] << hex[character & 0xf];
                } else out << static_cast<char>(character);
        }
    }
    out << '"';
}

void dump(std::ostream& out, const Json& json, int indent, int depth) {
    const auto newline = [&] {
        if (indent >= 0) {
            out << '\n';
            for (int count = 0; count < indent * (depth + 1); ++count) out << ' ';
        }
    };
    switch (json.kind()) {
        case Json::Kind::null: out << "null"; break;
        case Json::Kind::boolean: out << (json.asBool() ? "true" : "false"); break;
        case Json::Kind::number: {
            const double value = json.asNumber();
            if (value == static_cast<double>(static_cast<long long>(value)) && std::fabs(value) < 1.0e15) {
                out << static_cast<long long>(value);
            } else out << value;
            break;
        }
        case Json::Kind::string: dumpString(out, json.asString()); break;
        case Json::Kind::array: {
            out << '[';
            const auto& values = json.asArray();
            for (std::size_t index = 0; index < values.size(); ++index) {
                if (index != 0) out << ',';
                newline();
                dump(out, values[index], indent, depth + 1);
            }
            if (!values.empty() && indent >= 0) {
                out << '\n';
                for (int count = 0; count < indent * depth; ++count) out << ' ';
            }
            out << ']';
            break;
        }
        case Json::Kind::object: {
            out << '{';
            const auto& items = json.items();
            for (std::size_t index = 0; index < items.size(); ++index) {
                if (index != 0) out << ',';
                newline();
                dumpString(out, items[index].first);
                out << (indent >= 0 ? ": " : ":");
                dump(out, items[index].second, indent, depth + 1);
            }
            if (!items.empty() && indent >= 0) {
                out << '\n';
                for (int count = 0; count < indent * depth; ++count) out << ' ';
            }
            out << '}';
            break;
        }
    }
}
} // namespace

Json Json::null() { return {}; }
Json Json::boolean(bool value) {
    Json json;
    json.kind_ = Kind::boolean;
    json.boolean_ = value;
    return json;
}
Json Json::number(double value) {
    Json json;
    json.kind_ = Kind::number;
    json.number_ = value;
    return json;
}
Json Json::string(std::string value) {
    Json json;
    json.kind_ = Kind::string;
    json.text_ = std::move(value);
    return json;
}
Json Json::array() {
    Json json;
    json.kind_ = Kind::array;
    return json;
}
Json Json::object() {
    Json json;
    json.kind_ = Kind::object;
    return json;
}

std::optional<Json> Json::parse(std::string_view text, std::string& error) { return Parser{text}.parse(error); }

bool Json::asBool(bool fallback) const noexcept { return kind_ == Kind::boolean ? boolean_ : fallback; }
double Json::asNumber(double fallback) const noexcept { return kind_ == Kind::number ? number_ : fallback; }
const std::string& Json::asString() const {
    static const std::string empty;
    return kind_ == Kind::string ? text_ : empty;
}
const std::vector<Json>& Json::asArray() const {
    static const std::vector<Json> empty;
    return kind_ == Kind::array ? values_ : empty;
}
const Json* Json::find(std::string_view key) const noexcept {
    if (kind_ != Kind::object) return nullptr;
    for (const auto& [name, value] : items_) {
        if (name == key) return &value;
    }
    return nullptr;
}
Json& Json::set(std::string key, Json value) {
    kind_ = Kind::object;
    for (auto& [name, existing] : items_) {
        if (name == key) {
            existing = std::move(value);
            return existing;
        }
    }
    items_.emplace_back(std::move(key), std::move(value));
    return items_.back().second;
}
Json& Json::push(Json value) {
    kind_ = Kind::array;
    values_.push_back(std::move(value));
    return values_.back();
}
std::string Json::dump(int indent) const {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out.precision(17);
    persist::dump(out, *this, indent, 0);
    return out.str();
}
} // namespace nodsynth::persist
