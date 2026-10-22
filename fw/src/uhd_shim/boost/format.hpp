// Minimal boost::format: each % fills the next printf-style conversion. The
// result is a fixed-size buffer (no std::string), long enough for the driver's
// error messages.
#pragma once
#include <cstdio>
#include <cstring>

namespace boost {
class format
{
public:
    explicit format(const char* fmt) : _rest(fmt) { _out[0] = 0; }

    template <typename T> format& operator%(const T& value)
    {
        const char* pct = std::strchr(_rest, '%');
        const char* end = pct ? std::strpbrk(pct + 1, "diouxXeEfgGcsp") : nullptr;
        if (!end)
            return *this;
        char spec[16];
        size_t n = end - pct + 1 < sizeof(spec) ? end - pct + 1 : sizeof(spec) - 1;
        std::memcpy(spec, pct, n);
        spec[n] = 0;
        append(_rest, pct - _rest);
        char buf[48];
        std::snprintf(buf, sizeof(buf), spec, value);
        append(buf, std::strlen(buf));
        _rest = end + 1;
        return *this;
    }

    const char* c_str()
    {
        append(_rest, std::strlen(_rest));
        _rest = "";
        return _out;
    }

private:
    void append(const char* s, size_t n)
    {
        size_t len = std::strlen(_out);
        if (n > sizeof(_out) - 1 - len)
            n = sizeof(_out) - 1 - len;
        std::memcpy(_out + len, s, n);
        _out[len + n] = 0;
    }

    char _out[120];
    const char* _rest;
};

inline const char* str(format& f) { return f.c_str(); }
} // namespace boost
