#pragma once
// Minimal JSON library for CDP protocol
#include <string>
#include <vector>
#include <map>
#include <sstream>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cstdint>

namespace json {

enum class Type { Null, Bool, Number, String, Array, Object };

class Value {
public:
    using ObjectType = std::map<std::string, Value>;
    using ArrayType = std::vector<Value>;

    Value() : type_(Type::Null), bool_val_(false), num_val_(0) {}
    Value(std::nullptr_t) : type_(Type::Null), bool_val_(false), num_val_(0) {}
    Value(bool b) : type_(Type::Bool), bool_val_(b), num_val_(0) {}
    Value(int n) : type_(Type::Number), bool_val_(false), num_val_(static_cast<double>(n)) {}
    Value(unsigned int n) : type_(Type::Number), bool_val_(false), num_val_(static_cast<double>(n)) {}
    Value(int64_t n) : type_(Type::Number), bool_val_(false), num_val_(static_cast<double>(n)) {}
    Value(double n) : type_(Type::Number), bool_val_(false), num_val_(n) {}
    Value(const char* s) : type_(Type::String), bool_val_(false), num_val_(0), str_val_(s) {}
    Value(const std::string& s) : type_(Type::String), bool_val_(false), num_val_(0), str_val_(s) {}
    Value(std::string&& s) : type_(Type::String), bool_val_(false), num_val_(0), str_val_(std::move(s)) {}

    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::Null; }
    bool is_bool() const { return type_ == Type::Bool; }
    bool is_number() const { return type_ == Type::Number; }
    bool is_string() const { return type_ == Type::String; }
    bool is_array() const { return type_ == Type::Array; }
    bool is_object() const { return type_ == Type::Object; }

    bool get_bool() const { return bool_val_; }
    double get_number() const { return num_val_; }
    int get_int() const { return static_cast<int>(num_val_); }
    int64_t get_int64() const { return static_cast<int64_t>(num_val_); }
    const std::string& get_string() const { return str_val_; }
    const ArrayType& get_array() const { return arr_val_; }
    ArrayType& get_array() { return arr_val_; }
    const ObjectType& get_object() const { return obj_val_; }
    ObjectType& get_object() { return obj_val_; }

    bool has(const std::string& key) const {
        return type_ == Type::Object && obj_val_.count(key) > 0;
    }

    const Value& operator[](const std::string& key) const {
        static const Value null_value;
        if (type_ != Type::Object) return null_value;
        auto it = obj_val_.find(key);
        return it != obj_val_.end() ? it->second : null_value;
    }

    Value& operator[](const std::string& key) {
        if (type_ != Type::Object) {
            type_ = Type::Object;
            obj_val_.clear();
        }
        return obj_val_[key];
    }

    const Value& operator[](size_t index) const {
        static const Value null_value;
        if (type_ != Type::Array || index >= arr_val_.size()) return null_value;
        return arr_val_[index];
    }

    size_t size() const {
        if (type_ == Type::Array) return arr_val_.size();
        if (type_ == Type::Object) return obj_val_.size();
        return 0;
    }

    static Value object() { Value v; v.type_ = Type::Object; return v; }
    static Value array() { Value v; v.type_ = Type::Array; return v; }

    Value& set(const std::string& key, const Value& val) {
        if (type_ != Type::Object) { type_ = Type::Object; obj_val_.clear(); }
        obj_val_[key] = val;
        return *this;
    }

    Value& push(const Value& val) {
        if (type_ != Type::Array) { type_ = Type::Array; arr_val_.clear(); }
        arr_val_.push_back(val);
        return *this;
    }

    std::string serialize() const {
        std::ostringstream ss;
        serialize_to(ss);
        return ss.str();
    }

    static Value parse(const std::string& str) {
        size_t pos = 0;
        return parse_value(str, pos);
    }

private:
    Type type_;
    bool bool_val_;
    double num_val_;
    std::string str_val_;
    ArrayType arr_val_;
    ObjectType obj_val_;

    void serialize_to(std::ostringstream& ss) const {
        switch (type_) {
        case Type::Null:
            ss << "null";
            break;
        case Type::Bool:
            ss << (bool_val_ ? "true" : "false");
            break;
        case Type::Number: {
            int64_t iv = static_cast<int64_t>(num_val_);
            if (num_val_ == static_cast<double>(iv) && num_val_ >= -1e15 && num_val_ <= 1e15) {
                ss << iv;
            } else {
                char buf[64];
                snprintf(buf, sizeof(buf), "%.17g", num_val_);
                ss << buf;
            }
            break;
        }
        case Type::String:
            ss << '"';
            for (unsigned char c : str_val_) {
                switch (c) {
                case '"':  ss << "\\\""; break;
                case '\\': ss << "\\\\"; break;
                case '\b': ss << "\\b"; break;
                case '\f': ss << "\\f"; break;
                case '\n': ss << "\\n"; break;
                case '\r': ss << "\\r"; break;
                case '\t': ss << "\\t"; break;
                default:
                    if (c < 0x20) {
                        char buf[8];
                        snprintf(buf, sizeof(buf), "\\u%04x", c);
                        ss << buf;
                    } else {
                        ss << static_cast<char>(c);
                    }
                }
            }
            ss << '"';
            break;
        case Type::Array:
            ss << '[';
            for (size_t i = 0; i < arr_val_.size(); i++) {
                if (i > 0) ss << ',';
                arr_val_[i].serialize_to(ss);
            }
            ss << ']';
            break;
        case Type::Object:
            ss << '{';
            { bool first = true;
              for (const auto& kv : obj_val_) {
                  if (!first) ss << ',';
                  first = false;
                  Value(kv.first).serialize_to(ss);
                  ss << ':';
                  kv.second.serialize_to(ss);
              }
            }
            ss << '}';
            break;
        }
    }

    static void skip_ws(const std::string& s, size_t& p) {
        while (p < s.size() && (s[p]==' '||s[p]=='\t'||s[p]=='\n'||s[p]=='\r')) p++;
    }

    static Value parse_value(const std::string& s, size_t& p) {
        skip_ws(s, p);
        if (p >= s.size()) return Value();
        switch (s[p]) {
        case '"': return parse_string(s, p);
        case '{': return parse_object(s, p);
        case '[': return parse_array(s, p);
        case 't': case 'f': return parse_bool(s, p);
        case 'n': return parse_null(s, p);
        default:  return parse_number(s, p);
        }
    }

    static Value parse_string(const std::string& s, size_t& p) {
        p++; // skip "
        std::string r;
        while (p < s.size() && s[p] != '"') {
            if (s[p] == '\\') {
                p++;
                if (p >= s.size()) break;
                switch (s[p]) {
                case '"':  r += '"'; break;
                case '\\': r += '\\'; break;
                case '/':  r += '/'; break;
                case 'b':  r += '\b'; break;
                case 'f':  r += '\f'; break;
                case 'n':  r += '\n'; break;
                case 'r':  r += '\r'; break;
                case 't':  r += '\t'; break;
                case 'u':
                    if (p + 4 < s.size()) {
                        unsigned cp = (unsigned)std::stoul(s.substr(p+1, 4), nullptr, 16);
                        if (cp < 0x80) {
                            r += (char)cp;
                        } else if (cp < 0x800) {
                            r += (char)(0xC0 | (cp >> 6));
                            r += (char)(0x80 | (cp & 0x3F));
                        } else {
                            r += (char)(0xE0 | (cp >> 12));
                            r += (char)(0x80 | ((cp >> 6) & 0x3F));
                            r += (char)(0x80 | (cp & 0x3F));
                        }
                        p += 4;
                    }
                    break;
                default: r += s[p];
                }
            } else {
                r += s[p];
            }
            p++;
        }
        if (p < s.size()) p++; // skip closing "
        return Value(std::move(r));
    }

    static Value parse_number(const std::string& s, size_t& p) {
        size_t start = p;
        if (p < s.size() && s[p] == '-') p++;
        while (p < s.size() && s[p] >= '0' && s[p] <= '9') p++;
        if (p < s.size() && s[p] == '.') {
            p++;
            while (p < s.size() && s[p] >= '0' && s[p] <= '9') p++;
        }
        if (p < s.size() && (s[p] == 'e' || s[p] == 'E')) {
            p++;
            if (p < s.size() && (s[p] == '+' || s[p] == '-')) p++;
            while (p < s.size() && s[p] >= '0' && s[p] <= '9') p++;
        }
        return Value(std::stod(s.substr(start, p - start)));
    }

    static Value parse_bool(const std::string& s, size_t& p) {
        if (s.compare(p, 4, "true") == 0) { p += 4; return Value(true); }
        if (s.compare(p, 5, "false") == 0) { p += 5; return Value(false); }
        return Value();
    }

    static Value parse_null(const std::string& s, size_t& p) {
        if (s.compare(p, 4, "null") == 0) { p += 4; }
        return Value();
    }

    static Value parse_object(const std::string& s, size_t& p) {
        p++; // skip {
        Value obj = Value::object();
        skip_ws(s, p);
        if (p < s.size() && s[p] == '}') { p++; return obj; }
        while (p < s.size()) {
            skip_ws(s, p);
            if (p >= s.size() || s[p] != '"') break;
            Value key = parse_string(s, p);
            skip_ws(s, p);
            if (p < s.size() && s[p] == ':') p++;
            Value val = parse_value(s, p);
            obj.set(key.get_string(), val);
            skip_ws(s, p);
            if (p < s.size() && s[p] == ',') { p++; } else break;
        }
        skip_ws(s, p);
        if (p < s.size() && s[p] == '}') p++;
        return obj;
    }

    static Value parse_array(const std::string& s, size_t& p) {
        p++; // skip [
        Value arr = Value::array();
        skip_ws(s, p);
        if (p < s.size() && s[p] == ']') { p++; return arr; }
        while (p < s.size()) {
            arr.push(parse_value(s, p));
            skip_ws(s, p);
            if (p < s.size() && s[p] == ',') { p++; } else break;
        }
        skip_ws(s, p);
        if (p < s.size() && s[p] == ']') p++;
        return arr;
    }
};

} // namespace json
