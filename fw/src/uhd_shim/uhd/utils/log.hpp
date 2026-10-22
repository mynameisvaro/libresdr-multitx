// Stand-in for UHD's logging: the driver's debug/trace messages are dropped.
#pragma once

namespace uhd { namespace shim {
struct null_log
{
    template <typename T> null_log& operator<<(const T&) { return *this; }
};
}} // namespace uhd::shim

#define UHD_LOG_TRACE(component, message) do { } while (0)
#define UHD_LOG_DEBUG(component, message) do { } while (0)
#define UHD_LOGGER_DEBUG(component) uhd::shim::null_log()
