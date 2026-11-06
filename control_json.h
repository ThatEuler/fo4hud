#pragma once
#include <json-c/json.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>

namespace fo4control {
struct JsonDelete { void operator()(json_object* p) const { if (p) json_object_put(p); } };
using Json = std::unique_ptr<json_object, JsonDelete>;
inline Json object() { return Json(json_object_new_object()); }
inline json_object* member(json_object* o, const char* key) {
    json_object* v = nullptr;
    if (!o || !json_object_is_type(o, json_type_object) || !json_object_object_get_ex(o, key, &v)) return nullptr;
    return v;
}
inline void set(json_object* o, const char* key, json_object* v) { json_object_object_add(o, key, v); }
inline void set(json_object* o, const char* key, const std::string& v) { set(o,key,json_object_new_string_len(v.data(),v.size())); }
inline void set(json_object* o, const char* key, const char* v) { set(o,key,std::string(v)); }
inline void set(json_object* o, const char* key, bool v) { set(o,key,json_object_new_boolean(v)); }
inline void set(json_object* o, const char* key, int64_t v) { set(o,key,json_object_new_int64(v)); }
inline void set(json_object* o, const char* key, double v) { set(o,key,std::isfinite(v)?json_object_new_double(v):nullptr); }
inline std::string string(json_object* o, const char* key, const std::string& fallback = {}) {
    auto* v = member(o,key);
    if (!v) return fallback;
    if (!json_object_is_type(v,json_type_string)) throw std::runtime_error(std::string(key)+" must be a string");
    return std::string(json_object_get_string(v),json_object_get_string_len(v));
}
inline int64_t integer(json_object* o, const char* key, int64_t fallback) {
    auto* v=member(o,key); if (!v) return fallback;
    if (!json_object_is_type(v,json_type_int)) throw std::runtime_error(std::string(key)+" must be an integer");
    return json_object_get_int64(v);
}
inline double number(json_object* o, const char* key, double fallback) {
    auto* v=member(o,key); if (!v) return fallback;
    if (!json_object_is_type(v,json_type_double) && !json_object_is_type(v,json_type_int)) throw std::runtime_error(std::string(key)+" must be numeric");
    double n=json_object_get_double(v); if (!std::isfinite(n)) throw std::runtime_error("nonfinite number"); return n;
}
inline bool boolean(json_object* o, const char* key, bool fallback=false) {
    auto* v=member(o,key); if (!v) return fallback;
    if (!json_object_is_type(v,json_type_boolean)) throw std::runtime_error(std::string(key)+" must be boolean");
    return json_object_get_boolean(v);
}
inline std::string hex(uintptr_t p) { char b[32]; std::snprintf(b,sizeof(b),"0x%lx",(unsigned long)p); return b; }
inline uintptr_t address(json_object* o, const char* key) {
    auto s=string(o,key); if (s.empty() || s.size()>18 || s[0]=='-') throw std::runtime_error(std::string("invalid ")+key);
    size_t end=0; unsigned long long n=0;
    try { n=std::stoull(s,&end,0); } catch (...) { throw std::runtime_error(std::string("invalid ")+key); }
    if (end!=s.size() || !n) throw std::runtime_error(std::string("invalid ")+key);
    return uintptr_t(n);
}
inline Json parse(const std::string& s) {
    std::unique_ptr<json_tokener, decltype(&json_tokener_free)> t(json_tokener_new(),json_tokener_free);
    json_tokener_set_flags(t.get(),JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    Json j(json_tokener_parse_ex(t.get(),s.c_str(),s.size()+1));
    if (json_tokener_get_error(t.get())!=json_tokener_success || !j || !json_object_is_type(j.get(),json_type_object)) throw std::runtime_error("invalid JSON object");
    return j;
}
inline std::string dump(json_object* j) { return json_object_to_json_string_ext(j,JSON_C_TO_STRING_PLAIN); }
inline Json failure(const std::string& message) { auto j=object();set(j.get(),"error",message);return j; }
}
