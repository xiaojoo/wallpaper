#pragma once
// Engine/Core/Json.hpp - small JSON reader/writer (config files and IPC payloads).
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sw {

class Json {
public:
    enum class Kind { Null, Bool, Number, String, Array, Object };
    using Member = std::pair<std::string, Json>;

    Json() = default;
    static Json Null() { return Json(); }
    static Json Of(bool v);
    static Json Of(double v);
    static Json Of(long long v);
    static Json Of(int v) { return Of((long long)v); }
    static Json Of(std::string v);
    static Json Of(std::string_view v) { return Of(std::string(v)); }
    // Without this one, `Of("apply")` picks Of(bool): a literal decays to a pointer and pointer-to-bool
    // is a standard conversion, which outranks the user-defined conversion to std::string(_view). Every
    // call site that reads like a string then silently writes `true` - which is how the tray's apply
    // command, config.json's _comment and the imported manifests' renderer/quality ended up.
    static Json Of(const char* v) { return Of(std::string(v)); }
    static Json Object();
    static Json Array();

    Kind kind() const { return kind_; }
    bool isNull() const { return kind_ == Kind::Null; }
    bool isBool() const { return kind_ == Kind::Bool; }
    bool isNumber() const { return kind_ == Kind::Number; }
    bool isString() const { return kind_ == Kind::String; }
    bool isArray() const { return kind_ == Kind::Array; }
    bool isObject() const { return kind_ == Kind::Object; }

    bool asBool(bool def = false) const;
    double asNumber(double def = 0.0) const;
    int asInt(int def = 0) const;
    long long asInt64(long long def = 0) const;
    const std::string& asString(const std::string& def = Empty()) const;

    // Read a field with a default; safe on any kind and on a missing key.
    std::string strOr(std::string_view key, std::string def = {}) const;
    int intOr(std::string_view key, int def = 0) const;
    long long i64Or(std::string_view key, long long def = 0) const;
    double numOr(std::string_view key, double def = 0.0) const;
    bool boolOr(std::string_view key, bool def = false) const;
    const Json& at(std::string_view key) const; // a shared Null when absent

    // Container access. Find/member helpers are safe on any kind (return nullptr / defaults).
    const Json* find(std::string_view key) const;
    Json* find(std::string_view key);
    bool has(std::string_view key) const { return find(key) != nullptr; }
    void set(std::string key, Json value);
    void push(Json value); // array append
    bool erase(std::string_view key);

    const std::vector<Member>& members() const { return obj_; }
    const std::vector<Json>& items() const { return arr_; }
    std::vector<Json>& items() { return arr_; }
    size_t size() const { return kind_ == Kind::Array ? arr_.size() : kind_ == Kind::Object ? obj_.size() : 0; }

    // Returns the members of the object stored under key, or an empty vector.
    std::vector<Member> membersOf(std::string_view key) const;

    static std::optional<Json> Parse(std::string_view text, std::string& error);
    static std::optional<Json> ParseOrLog(std::string_view text, const char* where);
    std::string dump(int indent = 0) const;

    static const std::string& Empty();

private:
    Kind kind_ = Kind::Null;
    bool bool_ = false;
    double num_ = 0.0;
    std::string str_;
    std::vector<Json> arr_;
    std::vector<Member> obj_;
    void DumpTo(std::string& out, int indent, int depth) const;
};

// UTF-8 <-> UTF-16 conversion for filesystem and Win32 calls.
std::wstring ToWide(std::string_view utf8);
std::string ToUtf8(std::wstring_view wide);

std::string ReadFileUtf8(const std::wstring& path, bool& ok);
bool WriteFileUtf8(const std::wstring& path, std::string_view text);

} // namespace sw
