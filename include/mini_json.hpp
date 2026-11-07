// mini_json.hpp -- 极简 JSON 解析/生成（C++17，零依赖）
// 用途：读取 Minecraft 的 <version>.json（libraries / assetIndex / mainClass /
// arguments），以及构建器生成、启动器消费的 config.json。
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace mc263 {

struct JValue {
    enum class T { Null, Bool, Number, String, Array, Object };
    T t = T::Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<JValue> a;
    std::vector<std::pair<std::string, JValue>> o; // 用 vector 避免类内不完整类型

    bool isNull() const { return t == T::Null; }
    bool isArray() const { return t == T::Array; }
    bool isObject() const { return t == T::Object; }
    bool isString() const { return t == T::String; }

    const JValue* find(const std::string& key) const {
        if (t != T::Object) return nullptr;
        for (const auto& kv : o) if (kv.first == key) return &kv.second;
        return nullptr;
    }
    std::string str(const std::string& def = "") const {
        return t == T::String ? s : def;
    }
    bool boolean(bool def = false) const { return t == T::Bool ? b : def; }
    double number(double def = 0) const { return t == T::Number ? n : def; }
    const std::vector<JValue>& array() const {
        static const std::vector<JValue> empty;
        return t == T::Array ? a : empty;
    }
    const std::vector<std::pair<std::string, JValue>>& object() const {
        static const std::vector<std::pair<std::string, JValue>> empty;
        return t == T::Object ? o : empty;
    }

    static JValue null() { return JValue{}; }
    static JValue booleanV(bool v) { JValue j; j.t = T::Bool; j.b = v; return j; }
    static JValue numberV(double v) { JValue j; j.t = T::Number; j.n = v; return j; }
    static JValue stringV(std::string v) { JValue j; j.t = T::String; j.s = std::move(v); return j; }
    static JValue arrayV() { JValue j; j.t = T::Array; return j; }
    static JValue objectV() { JValue j; j.t = T::Object; return j; }

    void push(JValue v) { if (t != T::Array) { t = T::Array; a.clear(); } a.push_back(std::move(v)); }
    void set(const std::string& key, JValue v) {
        if (t != T::Object) { t = T::Object; o.clear(); }
        for (auto& kv : o) if (kv.first == key) { kv.second = std::move(v); return; }
        o.emplace_back(key, std::move(v));
    }
};

namespace detail {
inline void appendUtf8(std::string& out, uint32_t cp) {
    if (cp <= 0x7F) out.push_back(static_cast<char>(cp));
    else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}
inline std::string escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

class Parser {
public:
    explicit Parser(const std::string& text) : s_(text) {}
    bool parse(JValue& out, std::string& err) {
        skip();
        if (!value(out, 0, err)) return false;
        skip();
        if (p_ != s_.size()) { err = "JSON 尾部有多余内容 (offset " + std::to_string(p_) + ")"; return false; }
        return true;
    }
private:
    const std::string& s_;
    size_t p_ = 0;

    void skip() { while (p_ < s_.size() && (s_[p_] == ' ' || s_[p_] == '\t' || s_[p_] == '\n' || s_[p_] == '\r')) ++p_; }
    bool fail(const std::string& msg, std::string& err) {
        err = msg + " (offset " + std::to_string(p_) + ")";
        return false;
    }
    bool value(JValue& out, int depth, std::string& err) {
        if (depth > 64) return fail("JSON 嵌套过深", err);
        skip();
        if (p_ >= s_.size()) return fail("意外的结尾", err);
        char c = s_[p_];
        if (c == '{') return object(out, depth, err);
        if (c == '[') return array(out, depth, err);
        if (c == '"') { std::string v; if (!string(v, err)) return false; out = JValue::stringV(std::move(v)); return true; }
        if (c == 't') { if (s_.compare(p_, 4, "true") == 0) { p_ += 4; out = JValue::booleanV(true); return true; } return fail("无效字面量", err); }
        if (c == 'f') { if (s_.compare(p_, 5, "false") == 0) { p_ += 5; out = JValue::booleanV(false); return true; } return fail("无效字面量", err); }
        if (c == 'n') { if (s_.compare(p_, 4, "null") == 0) { p_ += 4; out = JValue::null(); return true; } return fail("无效字面量", err); }
        if (c == '-' || (c >= '0' && c <= '9')) return number(out, err);
        return fail("无法识别的字符 '" + std::string(1, c) + "'", err);
    }
    bool object(JValue& out, int depth, std::string& err) {
        out = JValue::objectV();
        ++p_; // {
        skip();
        if (p_ < s_.size() && s_[p_] == '}') { ++p_; return true; }
        while (true) {
            skip();
            if (p_ >= s_.size() || s_[p_] != '"') return fail("object 需要字符串键", err);
            std::string key;
            if (!string(key, err)) return false;
            skip();
            if (p_ >= s_.size() || s_[p_] != ':') return fail("object 缺少 ':'", err);
            ++p_;
            JValue v;
            if (!value(v, depth + 1, err)) return false;
            out.set(key, std::move(v));
            skip();
            if (p_ < s_.size() && s_[p_] == ',') { ++p_; continue; }
            if (p_ < s_.size() && s_[p_] == '}') { ++p_; return true; }
            return fail("object 缺少 ',' 或 '}'", err);
        }
    }
    bool array(JValue& out, int depth, std::string& err) {
        out = JValue::arrayV();
        ++p_; // [
        skip();
        if (p_ < s_.size() && s_[p_] == ']') { ++p_; return true; }
        while (true) {
            JValue v;
            if (!value(v, depth + 1, err)) return false;
            out.push(std::move(v));
            skip();
            if (p_ < s_.size() && s_[p_] == ',') { ++p_; continue; }
            if (p_ < s_.size() && s_[p_] == ']') { ++p_; return true; }
            return fail("array 缺少 ',' 或 ']'", err);
        }
    }
    bool hex4(uint32_t& v, std::string& err) {
        if (p_ + 4 > s_.size()) return fail("\\u 转义不完整", err);
        v = 0;
        for (int i = 0; i < 4; ++i) {
            char c = s_[p_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<uint32_t>(c - 'A' + 10);
            else return fail("\\u 转义含非法字符", err);
        }
        return true;
    }
    bool string(std::string& out, std::string& err) {
        if (p_ >= s_.size() || s_[p_] != '"') return fail("期望字符串", err);
        ++p_;
        out.clear();
        while (p_ < s_.size()) {
            unsigned char c = static_cast<unsigned char>(s_[p_++]);
            if (c == '"') return true;
            if (c == '\\') {
                if (p_ >= s_.size()) return fail("转义不完整", err);
                char e = s_[p_++];
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
                        uint32_t cp = 0;
                        if (!hex4(cp, err)) return false;
                        if (cp >= 0xD800 && cp <= 0xDBFF && p_ + 6 <= s_.size() &&
                            s_[p_] == '\\' && s_[p_ + 1] == 'u') {
                            p_ += 2;
                            uint32_t lo = 0;
                            if (!hex4(lo, err)) return false;
                            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                                appendUtf8(out, cp);
                            } else {
                                appendUtf8(out, cp);
                                appendUtf8(out, lo);
                            }
                        } else {
                            appendUtf8(out, cp);
                        }
                        break;
                    }
                    default: return fail("未知转义 \\" + std::string(1, e), err);
                }
            } else if (c < 0x20) return fail("字符串含未转义控制字符", err);
            else out.push_back(static_cast<char>(c));
        }
        return fail("字符串未闭合", err);
    }
    bool number(JValue& out, std::string& err) {
        size_t start = p_;
        if (s_[p_] == '-') ++p_;
        while (p_ < s_.size() && s_[p_] >= '0' && s_[p_] <= '9') ++p_;
        if (p_ < s_.size() && s_[p_] == '.') {
            ++p_;
            while (p_ < s_.size() && s_[p_] >= '0' && s_[p_] <= '9') ++p_;
        }
        if (p_ < s_.size() && (s_[p_] == 'e' || s_[p_] == 'E')) {
            ++p_;
            if (p_ < s_.size() && (s_[p_] == '+' || s_[p_] == '-')) ++p_;
            while (p_ < s_.size() && s_[p_] >= '0' && s_[p_] <= '9') ++p_;
        }
        std::string tok = s_.substr(start, p_ - start);
        char* end = nullptr;
        double v = std::strtod(tok.c_str(), &end);
        if (!end || *end != '\0') return fail("无效数字 '" + tok + "'", err);
        out = JValue::numberV(v);
        return true;
    }
};

inline void dumpTo(const JValue& v, std::string& out, int indent, int level) {
    auto pad = [&](int lv) { out.append(static_cast<size_t>(lv * indent), ' '); };
    switch (v.t) {
        case JValue::T::Null: out += "null"; break;
        case JValue::T::Bool: out += v.b ? "true" : "false"; break;
        case JValue::T::Number: {
            char buf[64];
            if (std::floor(v.n) == v.n) std::snprintf(buf, sizeof(buf), "%.0f", v.n);
            else std::snprintf(buf, sizeof(buf), "%.17g", v.n);
            out += buf;
            break;
        }
        case JValue::T::String: out += "\"" + escape(v.s) + "\""; break;
        case JValue::T::Array: {
            if (v.a.empty()) { out += "[]"; break; }
            out += "[\n";
            for (size_t i = 0; i < v.a.size(); ++i) {
                pad(level + 1);
                dumpTo(v.a[i], out, indent, level + 1);
                if (i + 1 < v.a.size()) out += ",";
                out += "\n";
            }
            pad(level);
            out += "]";
            break;
        }
        case JValue::T::Object: {
            if (v.o.empty()) { out += "{}"; break; }
            out += "{\n";
            for (size_t i = 0; i < v.o.size(); ++i) {
                pad(level + 1);
                out += "\"" + escape(v.o[i].first) + "\": ";
                dumpTo(v.o[i].second, out, indent, level + 1);
                if (i + 1 < v.o.size()) out += ",";
                out += "\n";
            }
            pad(level);
            out += "}";
            break;
        }
    }
}
} // namespace detail

inline JValue jsonParse(const std::string& text, std::string& err) {
    JValue v;
    detail::Parser p(text);
    if (!p.parse(v, err)) return JValue::null();
    err.clear();
    return v;
}

inline std::string jsonDump(const JValue& v, int indent = 2) {
    std::string out;
    detail::dumpTo(v, out, indent, 0);
    out += "\n";
    return out;
}

} // namespace mc263
