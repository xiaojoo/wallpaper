#include "Engine/Core/Json.hpp"

#include <windows.h>

#include <cmath>
#include <format>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace sw {

const std::string& Json::Empty() {
    static const std::string e;
    return e;
}

Json Json::Of(bool v) { Json j; j.kind_ = Kind::Bool; j.bool_ = v; return j; }
Json Json::Of(double v) { Json j; j.kind_ = Kind::Number; j.num_ = v; return j; }
Json Json::Of(long long v) { Json j; j.kind_ = Kind::Number; j.num_ = static_cast<double>(v); return j; }
Json Json::Of(std::string v) { Json j; j.kind_ = Kind::String; j.str_ = std::move(v); return j; }
Json Json::Object() { Json j; j.kind_ = Kind::Object; return j; }
Json Json::Array() { Json j; j.kind_ = Kind::Array; return j; }

bool Json::asBool(bool def) const {
    if (kind_ == Kind::Bool) return bool_;
    if (kind_ == Kind::Number) return num_ != 0.0;
    return def;
}
double Json::asNumber(double def) const { return kind_ == Kind::Number ? num_ : def; }
int Json::asInt(int def) const { return kind_ == Kind::Number ? static_cast<int>(std::llround(num_)) : def; }
long long Json::asInt64(long long def) const { return kind_ == Kind::Number ? static_cast<long long>(num_) : def; }
const std::string& Json::asString(const std::string& def) const { return kind_ == Kind::String ? str_ : def; }

std::string Json::strOr(std::string_view key, std::string def) const {
    const Json* j = find(key);
    return (j && j->isString()) ? j->str_ : ((j && j->isNumber()) ? std::format("{:g}", j->num_) : def);
}
int Json::intOr(std::string_view key, int def) const {
    const Json* j = find(key);
    return j ? j->asInt(def) : def;
}
long long Json::i64Or(std::string_view key, long long def) const {
    const Json* j = find(key);
    return j ? j->asInt64(def) : def;
}
double Json::numOr(std::string_view key, double def) const {
    const Json* j = find(key);
    return j ? j->asNumber(def) : def;
}
bool Json::boolOr(std::string_view key, bool def) const {
    const Json* j = find(key);
    return j ? j->asBool(def) : def;
}
const Json& Json::at(std::string_view key) const {
    static const Json null;
    const Json* j = find(key);
    return j ? *j : null;
}

const Json* Json::find(std::string_view key) const {
    if (kind_ != Kind::Object) return nullptr;
    for (auto& m : obj_)
        if (m.first == key) return &m.second;
    return nullptr;
}
Json* Json::find(std::string_view key) {
    if (kind_ != Kind::Object) return nullptr;
    for (auto& m : obj_)
        if (m.first == key) return &m.second;
    return nullptr;
}

void Json::set(std::string key, Json value) {
    if (kind_ != Kind::Object) { kind_ = Kind::Object; arr_.clear(); }
    for (auto& m : obj_)
        if (m.first == key) { m.second = std::move(value); return; }
    obj_.emplace_back(std::move(key), std::move(value));
}

void Json::push(Json value) {
    if (kind_ != Kind::Array) { kind_ = Kind::Array; obj_.clear(); }
    arr_.push_back(std::move(value));
}

bool Json::erase(std::string_view key) {
    if (kind_ != Kind::Object) return false;
    for (auto it = obj_.begin(); it != obj_.end(); ++it)
        if (it->first == key) { obj_.erase(it); return true; }
    return false;
}

std::vector<Json::Member> Json::membersOf(std::string_view key) const {
    const Json* j = find(key);
    if (j && j->kind_ == Kind::Object) return j->obj_;
    return {};
}

// ---------------------------------------------------------------- parsing

namespace {

struct Parser {
    std::string_view s;
    size_t i = 0;
    std::string err;
    int depth = 0;

    bool fail(const std::string& m, size_t at) {
        if (err.empty()) {
            std::ostringstream o;
            o << m << " at offset " << at;
            err = o.str();
        }
        return false;
    }
    void SkipWs() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
    }
    bool Eof() { SkipWs(); return i >= s.size(); }
    char Peek() { return i < s.size() ? s[i] : '\0'; }
    bool Lit(std::string_view lit) {
        if (s.substr(i, lit.size()) != lit) return false;
        i += lit.size();
        return true;
    }

    bool ParseValue(Json& out);

    bool ParseString(std::string& out) {
        if (Peek() != '"') return fail("expected string", i);
        ++i;
        out.clear();
        while (i < s.size()) {
            char c = s[i++];
            if (c == '"') return true;
            if (c != '\\') {
                if ((unsigned char)c < 0x20) return fail("control char in string", i - 1);
                out.push_back(c);
                continue;
            }
            if (i >= s.size()) return fail("bad escape", i);
            char e = s[i++];
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    if (i + 4 > s.size()) return fail("bad \\u", i);
                    auto hex = [&](int k) -> int {
                        char h = s[i + k];
                        if (h >= '0' && h <= '9') return h - '0';
                        if (h >= 'a' && h <= 'f') return h - 'a' + 10;
                        if (h >= 'A' && h <= 'F') return h - 'A' + 10;
                        return -1;
                    };
                    int cp = 0;
                    for (int k = 0; k < 4; ++k) {
                        int v = hex(k);
                        if (v < 0) return fail("bad \\u digit", i);
                        cp = cp * 16 + v;
                    }
                    i += 4;
                    if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 <= s.size() && s[i] == '\\' && s[i + 1] == 'u') {
                        int lo = 0;
                        bool ok = true;
                        for (int k = 0; k < 4; ++k) {
                            int v = hex(2 + k);
                            if (v < 0) { ok = false; break; }
                            lo = lo * 16 + v;
                        }
                        if (ok && lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            i += 6;
                        }
                    }
                    AppendUtf8(out, (unsigned)cp);
                    break;
                }
                default: return fail("unknown escape", i - 1);
            }
        }
        return fail("unterminated string", i);
    }

    static void AppendUtf8(std::string& out, unsigned cp) {
        if (cp < 0x80) out.push_back((char)cp);
        else if (cp < 0x800) {
            out.push_back((char)(0xC0 | (cp >> 6)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back((char)(0xE0 | (cp >> 12)));
            out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        } else {
            out.push_back((char)(0xF0 | (cp >> 18)));
            out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        }
    }

    bool ParseNumber(double& out) {
        size_t start = i;
        if (Peek() == '-' || Peek() == '+') ++i;
        while (i < s.size() && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.' || s[i] == 'e' || s[i] == 'E' ||
                                s[i] == '-' || s[i] == '+'))
            ++i;
        if (i == start) return fail("bad number", i);
        std::string token(s.substr(start, i - start));
        char* end = nullptr;
        out = std::strtod(token.c_str(), &end);
        if (end == token.c_str()) return fail("bad number", start);
        return true;
    }

    bool ParseArray(Json& out) {
        ++i; // [
        out = Json::Array();
        SkipWs();
        if (Peek() == ']') { ++i; return true; }
        while (true) {
            Json v;
            if (!ParseValue(v)) return false;
            out.push(std::move(v));
            SkipWs();
            char c = Peek();
            if (c == ',') { ++i; continue; }
            if (c == ']') { ++i; return true; }
            return fail("expected , or ] in array", i);
        }
    }

    bool ParseObject(Json& out) {
        ++i; // {
        out = Json::Object();
        SkipWs();
        if (Peek() == '}') { ++i; return true; }
        while (true) {
            SkipWs();
            std::string key;
            if (!ParseString(key)) return false;
            SkipWs();
            if (Peek() != ':') return fail("expected : in object", i);
            ++i;
            Json v;
            if (!ParseValue(v)) return false;
            out.set(std::move(key), std::move(v));
            SkipWs();
            char c = Peek();
            if (c == ',') { ++i; continue; }
            if (c == '}') { ++i; return true; }
            return fail("expected , or } in object", i);
        }
    }
};

bool Parser::ParseValue(Json& out) {
    if (depth > 64) return fail("nesting too deep", i);
    SkipWs();
    char c = Peek();
    if (c == '"') {
        std::string str;
        if (!ParseString(str)) return false;
        out = Json::Of(std::move(str));
        return true;
    }
    if (c == '{') return ++depth, ParseObject(out), --depth, true;
    if (c == '[') return ++depth, ParseArray(out), --depth, true;
    if (c == 't') { if (!Lit("true")) return fail("bad literal", i); out = Json::Of(true); return true; }
    if (c == 'f') { if (!Lit("false")) return fail("bad literal", i); out = Json::Of(false); return true; }
    if (c == 'n') { if (!Lit("null")) return fail("bad literal", i); out = Json::Null(); return true; }
    if (c == '-' || c == '+' || (c >= '0' && c <= '9')) {
        double d = 0;
        if (!ParseNumber(d)) return false;
        out = Json::Of(d);
        return true;
    }
    return fail("unexpected character", i);
}

void EscapeToString(std::string& out, std::string_view in) {
    out.push_back('"');
    for (char c : in) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if ((unsigned char)c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else out.push_back(c);
        }
    }
    out.push_back('"');
}

void Indent(std::string& out, int n) { for (int i = 0; i < n; ++i) out += "  "; }

} // namespace

std::optional<Json> Json::Parse(std::string_view text, std::string& error) {
    error.clear();
    // tolerate a UTF-8 BOM
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB &&
        (unsigned char)text[2] == 0xBF)
        text.remove_prefix(3);
    Parser p;
    p.s = text;
    Json out;
    if (!p.ParseValue(out)) { error = p.err; return std::nullopt; }
    p.SkipWs();
    if (p.i != p.s.size()) { error = "trailing characters at offset " + std::to_string(p.i); return std::nullopt; }
    return out;
}

std::optional<Json> Json::ParseOrLog(std::string_view text, const char* where) {
    std::string err;
    auto j = Parse(text, err);
    if (!j) {
        std::ostringstream o;
        o << "json parse failed for " << (where ? where : "?") << ": " << err;
        // Logging is pulled in by callers; keep Json free of Log.hpp by printing.
        OutputDebugStringA(o.str().c_str());
    }
    return j;
}

void Json::DumpTo(std::string& out, int indent, int depth) const {
    const bool pretty = indent > 0;
    auto nl = [&](int d) {
        if (pretty) { out.push_back('\n'); Indent(out, d); }
    };
    switch (kind_) {
        case Kind::Null: out += "null"; break;
        case Kind::Bool: out += bool_ ? "true" : "false"; break;
        case Kind::Number: {
            if (num_ == std::floor(num_) && std::fabs(num_) < 1e15) {
                out += std::to_string((long long)num_);
            } else {
                char buf[40];
                std::snprintf(buf, sizeof(buf), "%.6f", num_);
                out += buf;
            }
            break;
        }
        case Kind::String: EscapeToString(out, str_); break;
        case Kind::Array: {
            if (arr_.empty()) { out += "[]"; break; }
            out.push_back('[');
            for (size_t k = 0; k < arr_.size(); ++k) {
                if (k) out.push_back(',');
                nl(depth + 1);
                arr_[k].DumpTo(out, indent, depth + 1);
            }
            nl(depth);
            out.push_back(']');
            break;
        }
        case Kind::Object: {
            if (obj_.empty()) { out += "{}"; break; }
            out.push_back('{');
            for (size_t k = 0; k < obj_.size(); ++k) {
                if (k) out.push_back(',');
                nl(depth + 1);
                EscapeToString(out, obj_[k].first);
                out += pretty ? ": " : ":";
                obj_[k].second.DumpTo(out, indent, depth + 1);
            }
            nl(depth);
            out.push_back('}');
            break;
        }
    }
}

std::string Json::dump(int indent) const {
    std::string out;
    DumpTo(out, indent, 0);
    return out;
}

// ---------------------------------------------------------------- strings & files

std::wstring ToWide(std::string_view utf8) {
    if (utf8.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), w.data(), n);
    return w;
}

std::string ToUtf8(std::wstring_view wide) {
    if (wide.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::string ReadFileUtf8(const std::wstring& path, bool& ok) {
    ok = false;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart > (1LL << 24)) { CloseHandle(h); return {}; }
    std::string data((size_t)size.QuadPart, '\0');
    DWORD got = 0;
    BOOL r = ReadFile(h, data.data(), (DWORD)data.size(), &got, nullptr);
    CloseHandle(h);
    if (!r || got != data.size()) return {};
    ok = true;
    return data;
}

bool WriteFileUtf8(const std::wstring& path, std::string_view text) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    BOOL ok = WriteFile(h, text.data(), (DWORD)text.size(), &wrote, nullptr);
    CloseHandle(h);
    return ok && wrote == text.size();
}

} // namespace sw
