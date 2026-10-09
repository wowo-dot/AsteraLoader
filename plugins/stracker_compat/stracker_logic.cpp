#include "stracker_logic.hpp"

#include <windows.h>

#include <algorithm>
#include <map>
#include <utility>

namespace al::stracker {
namespace {
enum class Kind { string, boolean, number, null, object, array };

struct Value {
    Kind kind = Kind::null;
    std::string text;
    bool flag = false;
};

// A small strict JSON reader (RFC 8259, no comments), enough to read the
// top-level members of loader-config.json. Nested objects and arrays are
// checked and skipped.
class Reader {
public:
    explicit Reader(std::string_view text) : s_(text) {}

    bool top_object(std::vector<std::pair<std::string, Value>>& members) {
        if (s_.substr(0, 3) == "\xEF\xBB\xBF") i_ = 3;  // UTF-8 BOM, as nlohmann accepts
        ws();
        if (i_ >= s_.size() || s_[i_] != '{') return false;
        return object(&members, 0);
    }

private:
    static constexpr int kMaxDepth = 64;

    bool object(std::vector<std::pair<std::string, Value>>* members, int depth) {
        if (depth > kMaxDepth) return false;
        ++i_;
        ws();
        if (eat('}')) return true;
        for (;;) {
            ws();
            std::string key;
            if (!string(key)) return false;
            ws();
            if (!eat(':')) return false;
            ws();
            Value value;
            if (!this->value(value, depth + 1)) return false;
            if (members) members->emplace_back(std::move(key), std::move(value));
            ws();
            if (eat(',')) continue;
            return eat('}');
        }
    }

    bool array(int depth) {
        if (depth > kMaxDepth) return false;
        ++i_;
        ws();
        if (eat(']')) return true;
        for (;;) {
            ws();
            Value ignored;
            if (!value(ignored, depth + 1)) return false;
            ws();
            if (eat(',')) continue;
            return eat(']');
        }
    }

    bool value(Value& out, int depth) {
        if (i_ >= s_.size()) return false;
        switch (s_[i_]) {
        case '{': out.kind = Kind::object; return object(nullptr, depth);
        case '[': out.kind = Kind::array; return array(depth);
        case '"': out.kind = Kind::string; return string(out.text);
        default: break;
        }
        if (literal("true")) { out.kind = Kind::boolean; out.flag = true; return true; }
        if (literal("false")) { out.kind = Kind::boolean; out.flag = false; return true; }
        if (literal("null")) { out.kind = Kind::null; return true; }
        out.kind = Kind::number;
        return number();
    }

    bool string(std::string& out) {
        if (!eat('"')) return false;
        while (i_ < s_.size()) {
            const auto c = static_cast<unsigned char>(s_[i_++]);
            if (c == '"') return true;
            if (c < 0x20) return false;
            if (c != '\\') {
                out.push_back(static_cast<char>(c));
                continue;
            }
            if (i_ >= s_.size()) return false;
            switch (s_[i_++]) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                unsigned code = 0;
                if (!hex4(code)) return false;
                if (code >= 0xD800 && code <= 0xDBFF) {
                    unsigned low = 0;
                    if (!eat('\\') || !eat('u') || !hex4(low) || low < 0xDC00 || low > 0xDFFF) return false;
                    code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                } else if (code >= 0xDC00 && code <= 0xDFFF) {
                    return false;
                }
                append_utf8(out, code);
                break;
            }
            default: return false;
            }
        }
        return false;
    }

    bool number() {
        eat('-');
        if (!eat('0')) {
            if (!digit()) return false;
            while (digit()) {}
        }
        if (eat('.')) {
            if (!digit()) return false;
            while (digit()) {}
        }
        if (eat('e') || eat('E')) {
            if (!eat('+')) eat('-');
            if (!digit()) return false;
            while (digit()) {}
        }
        return true;
    }

    bool hex4(unsigned& code) {
        if (s_.size() - i_ < 4) return false;
        for (int k = 0; k < 4; ++k) {
            const char c = s_[i_++];
            code <<= 4;
            if (c >= '0' && c <= '9') code |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') code |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') code |= static_cast<unsigned>(c - 'A' + 10);
            else return false;
        }
        return true;
    }

    static void append_utf8(std::string& out, unsigned code) {
        if (code < 0x80) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else if (code < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (code >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }

    bool digit() {
        if (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') {
            ++i_;
            return true;
        }
        return false;
    }
    bool eat(char c) {
        if (i_ < s_.size() && s_[i_] == c) {
            ++i_;
            return true;
        }
        return false;
    }
    bool literal(std::string_view word) {
        if (s_.substr(i_, word.size()) != word) return false;
        i_ += word.size();
        return true;
    }
    void ws() {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r')) ++i_;
    }

    std::string_view s_;
    std::size_t i_ = 0;
};
}  // namespace

bool parse_config(std::string_view text, Config& out, std::vector<std::string>& problems) {
    out = {};
    std::vector<std::pair<std::string, Value>> members;
    if (!Reader(text).top_object(members)) return false;
    std::map<std::string, Value> last;  // nlohmann keeps the last of repeated keys
    for (auto& [key, value] : members) last[key] = std::move(value);

    const auto flag = [&](const char* key, bool& field) {
        const auto found = last.find(key);
        if (found == last.end()) return;
        if (found->second.kind == Kind::boolean) {
            field = found->second.flag;
        } else {
            problems.push_back(std::string(key) + " is not true or false; using " + (field ? "true" : "false"));
        }
    };
    flag("logfile", out.logfile);
    flag("logcmd", out.logcmd);
    flag("enablePluginLoader", out.enable_plugin_loader);

    if (const auto found = last.find("logLevel"); found != last.end()) {
        static const std::pair<const char*, int> kLevels[] = {{"DEBUG", 0}, {"INFO", 1}, {"WARNING", 2}, {"ERROR", 3}};
        bool known = false;
        if (found->second.kind == Kind::string) {
            for (const auto& [name, level] : kLevels) {
                if (found->second.text == name) {
                    out.log_level = level;
                    known = true;
                }
            }
        }
        if (!known) problems.push_back("logLevel is not \"DEBUG\", \"INFO\", \"WARNING\" or \"ERROR\"; using \"ERROR\"");
    }
    return true;
}

std::vector<std::wstring> plugin_files(std::vector<DirectoryEntry> entries) {
    std::vector<std::wstring> files;
    for (auto& entry : entries) {
        if (entry.directory) continue;
        // std::filesystem's extension(): from the last '.', but a name that
        // starts with its only dot (".dll") has none.
        const auto dot = entry.name.rfind(L'.');
        if (dot == std::wstring::npos || dot == 0 || entry.name.compare(dot, std::wstring::npos, L".dll") != 0) {
            continue;
        }
        files.push_back(std::move(entry.name));
    }
    std::stable_sort(files.begin(), files.end(), [](const std::wstring& a, const std::wstring& b) {
        return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()),
                                    TRUE) == CSTR_LESS_THAN;
    });
    return files;
}

}  // namespace al::stracker
