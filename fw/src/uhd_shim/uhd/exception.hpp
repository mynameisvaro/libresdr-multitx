// Stand-in for UHD's exception header (fw/third_party/uhd_ad9361 only).
// The message lives in a fixed buffer: no std::string, which would pull ~50 KB
// of libstdc++ into the firmware.
#pragma once
#include <cstring>
#include <exception>

namespace uhd {
struct runtime_error : std::exception
{
    explicit runtime_error(const char* msg)
    {
        std::strncpy(_msg, msg, sizeof(_msg) - 1);
        _msg[sizeof(_msg) - 1] = 0;
    }
    const char* what() const noexcept override { return _msg; }

private:
    char _msg[120];
};
} // namespace uhd

#define UHD_THROW_INVALID_CODE_PATH() throw uhd::runtime_error("invalid code path")
#define UHD_UNUSED(x) x __attribute__((unused))
