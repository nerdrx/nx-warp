// Tiny append-only JSON writer. No dependencies: the probe must be a single
// self-contained exe that can be dropped on a bare Windows box.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace nxwarp::win {

class Json {
public:
    Json& begin_object(const char* key = nullptr)
    {
        sep(key);
        out_ += '{';
        stack_.push_back(false);
        return *this;
    }
    Json& end_object()
    {
        out_ += '}';
        stack_.pop_back();
        return *this;
    }
    Json& begin_array(const char* key = nullptr)
    {
        sep(key);
        out_ += '[';
        stack_.push_back(false);
        return *this;
    }
    Json& end_array()
    {
        out_ += ']';
        stack_.pop_back();
        return *this;
    }

    Json& str(const char* key, const std::string& v)
    {
        sep(key);
        quote(v);
        return *this;
    }
    Json& str(const std::string& v) { return str(nullptr, v); }

    Json& num(const char* key, double v)
    {
        sep(key);
        char buf[64];
        if (v == static_cast<double>(static_cast<long long>(v)) && v < 9e15 && v > -9e15)
            std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(v));
        else
            std::snprintf(buf, sizeof buf, "%.4f", v);
        out_ += buf;
        return *this;
    }
    Json& num(const char* key, unsigned long long v)
    {
        sep(key);
        char buf[32];
        std::snprintf(buf, sizeof buf, "%llu", v);
        out_ += buf;
        return *this;
    }
    Json& boolean(const char* key, bool v)
    {
        sep(key);
        out_ += v ? "true" : "false";
        return *this;
    }
    Json& null(const char* key)
    {
        sep(key);
        out_ += "null";
        return *this;
    }

    Json& strings(const char* key, const std::vector<std::string>& v)
    {
        begin_array(key);
        for (const auto& s : v)
            str(s);
        return end_array();
    }

    const std::string& text() const { return out_; }

private:
    void sep(const char* key)
    {
        if (!stack_.empty()) {
            if (stack_.back())
                out_ += ',';
            stack_.back() = true;
        }
        if (key) {
            quote(key);
            out_ += ':';
        }
    }
    void quote(const std::string& s)
    {
        out_ += '"';
        for (char c : s) {
            switch (c) {
            case '"': out_ += "\\\""; break;
            case '\\': out_ += "\\\\"; break;
            case '\n': out_ += "\\n"; break;
            case '\r': out_ += "\\r"; break;
            case '\t': out_ += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out_ += buf;
                } else {
                    out_ += c;
                }
            }
        }
        out_ += '"';
    }

    std::string out_;
    std::vector<bool> stack_;
};

} // namespace nxwarp::win
