// AD9361 glue: UHD's ad9361_device_t on the B220, behind a C API (rf.h).
#include <cstdio>
#include <exception>
#include <memory>

#include "ad9361_device.h"

extern "C" {
#include "ad9361_spi.h"
#include "axi_ad9361.h"
#include "rf.h"
}

using uhd::usrp::ad9361_device_t;

// Replaces libstdc++'s default, which demangles type names (~50 KB of code).
namespace __gnu_cxx {
void __verbose_terminate_handler()
{
    std::printf("fatal: unhandled C++ exception\n");
    for (;;)
        ;
}
} // namespace __gnu_cxx

namespace {

// Register access through the AXI Quad SPI.
class b220_io : public uhd::usrp::ad9361_io
{
public:
    uint8_t peek8(uint32_t reg) override { return ad9361_spi_read(reg); }
    void poke8(uint32_t reg, uint8_t val) override { ad9361_spi_write(reg, val); }
};

// Board description. The data port and clocking are wired like a B210 (UHD's
// b200_ad9361_client_t), but only the AD9361's A ports have baluns on the B220
// mini, so every band edge is 0 and port A is used at all frequencies (as UHD
// does for the B205mini).
class b220_params : public uhd::usrp::ad9361_params
{
public:
    double get_band_edge(uhd::usrp::frequency_band_t) override
    {
        return 0;
    }
    uhd::usrp::clocking_mode_t get_clocking_mode() override
    {
        return uhd::usrp::clocking_mode_t::AD9361_XTAL_N_CLK_PATH;   // 40 MHz TCXO
    }
    uhd::usrp::digital_interface_mode_t get_digital_interface_mode() override
    {
        return uhd::usrp::AD9361_DDR_FDD_LVCMOS;   // full port: P0 = RX, P1 = TX
    }
    uhd::usrp::digital_interface_delays_t get_digital_interface_timing() override
    {
        // UHD's B210 values (registers 0x006/0x007); not yet tuned for axi_ad9361
        uhd::usrp::digital_interface_delays_t d;
        d.rx_clk_delay  = 0;
        d.rx_data_delay = 0xF;
        d.tx_clk_delay  = 0;
        d.tx_data_delay = 0xF;
        return d;
    }
};

ad9361_device_t* dev;

ad9361_device_t::direction_t to_dir(rf_dir d)
{
    return d == RF_TX ? ad9361_device_t::TX : ad9361_device_t::RX;
}

// Run a driver call; turn its exceptions into -1 plus a message.
template <typename F> int guarded(const char* what, F f)
{
    if (!dev) {
        std::printf("rf: not initialised (run 'rf init')\n");
        return -1;
    }
    try {
        f();
        return 0;
    } catch (const std::exception& e) {
        std::printf("rf %s: %s\n", what, e.what());
        return -1;
    }
}

// Calibrations (rate changes, TX quadrature, big retunes) switch the RX DC and
// I/Q tracking off; UHD's ad936x_manager turns them back on, so do the same.
void rx_tracking_on()
{
    dev->set_dc_offset_auto(ad9361_device_t::RX, true);
    dev->set_iq_balance_auto(ad9361_device_t::RX, true);
}

void set(double* out, double v)
{
    if (out)
        *out = v;
}

} // namespace

extern "C" int rf_init(void)
{
    ad9361_hw_reset();
    if (!dev)
        dev = new ad9361_device_t(std::make_shared<b220_params>(), std::make_shared<b220_io>());
    int ret = guarded("init", [] {
        dev->initialize();
        dev->set_clock_rate(30.72e6);
        dev->set_active_chains(true, false, true, false);
        rx_tracking_on();
    });
    if (ret) {
        delete dev;
        dev = nullptr;
    } else {
        axi_ad9361_init();   // the AD9361 now clocks the data port
    }
    return ret;
}

extern "C" int rf_ready(void)
{
    return dev != nullptr;
}

extern "C" int rf_set_rate(double hz, double* actual)
{
    return guarded("rate", [=] {
        set(actual, dev->set_clock_rate(hz));
        dev->set_active_chains(true, false, true, false);
        rx_tracking_on();
    });
}

extern "C" int rf_get_rate(double* hz)
{
    return guarded("rate", [=] { set(hz, dev->get_clock_rate()); });
}

extern "C" int rf_tune(rf_dir dir, double hz, double* actual)
{
    return guarded("tune", [=] {
        set(actual, dev->tune(to_dir(dir), hz));
        rx_tracking_on();
    });
}

extern "C" int rf_get_freq(rf_dir dir, double* hz)
{
    return guarded("freq", [=] { set(hz, dev->get_freq(to_dir(dir))); });
}

extern "C" int rf_set_gain(rf_dir dir, double db, double* actual)
{
    return guarded("gain", [=] {
        set(actual, dev->set_gain(to_dir(dir), ad9361_device_t::CHAIN_1, db));
    });
}

extern "C" int rf_set_bw(rf_dir dir, double hz, double* actual)
{
    return guarded("bw", [=] { set(actual, dev->set_bw_filter(to_dir(dir), hz)); });
}

extern "C" int rf_rssi(double* db)
{
    return guarded("rssi", [=] { set(db, dev->get_rssi(ad9361_device_t::CHAIN_1)); });
}

extern "C" int rf_temperature(double* deg_c)
{
    return guarded("temp", [=] { set(deg_c, dev->get_average_temperature()); });
}

extern "C" int rf_loopback(int on)
{
    return guarded("loopback", [=] { dev->data_port_loopback(on != 0); });
}
