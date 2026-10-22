// AD9361 glue: UHD's ad9361_device_t on the B220, behind a C API (rf.h).
#include <cstdio>
#include <exception>
#include <memory>

#include "ad9361_device.h"

extern "C" {
#include "ad9361_spi.h"
#include "timer.h"
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

// UHD's synthesiser tables (third_party/uhd_ad9361/ad9361_synth_lut.h)
extern double vco_index[53];
extern int synth_cal_lut[53][12];

// Fast retune between LO slices: the register writes of UHD's _tune_helper()
// and _setup_synth(), without the ENSM trip through ALERT and with the PLL lock
// bit polled instead of a fixed 2 ms wait (the TX PLL locks in ~110 us on this
// board; the lock bit reads 0 right after the integer word is written, so it is
// safe to poll at once). Port A only (all band edges are 0). The RX synthesiser
// registers sit 0x40 below the TX ones. UHD's own frequency bookkeeping is not
// updated: use rf_tune() again before relying on rf_get_freq().
// trace: optional record of the lock register, one sample every ~10 us.
extern "C" int rf_fast_tune(rf_dir d, double hz, double* actual, uint32_t* lock_us,
                            uint8_t* trace, int* ntrace)
{
    const double fref = 80e6;
    const int modulus = 8388593;
    const uint16_t b = d == RF_TX ? 0x40 : 0;     // TX registers = RX + 0x40
    int i, vcodiv = 2;
    double vcorate = 0;

    if (!dev)
        return -1;
    for (i = 0; i <= 6; i++) {
        vcodiv  = 2 << i;
        vcorate = hz * vcodiv;
        if (vcorate >= 6e9 && vcorate <= 12e9)
            break;
    }
    if (i == 7)
        return -1;
    int nint  = static_cast<int>(vcorate / fref);
    int nfrac = static_cast<int>(((vcorate / fref) - nint) * modulus);
    double actual_vcorate = fref * (nint + (double)nfrac / modulus);

    int vi = 0;
    for (int k = 0; k < 53; k++) {
        vi = k;
        if (actual_vcorate > vco_index[k])
            break;
    }
    const int* lut = synth_cal_lut[vi];
    ad9361_spi_write(b + 0x23a, 0x40 | lut[0]);
    ad9361_spi_write(b + 0x239, 0xC0 | lut[1]);
    ad9361_spi_write(b + 0x242, lut[2] | (lut[3] << 3));
    ad9361_spi_write(b + 0x238, lut[4] << 3);
    ad9361_spi_write(b + 0x245, 0x00);
    ad9361_spi_write(b + 0x251, lut[5]);
    ad9361_spi_write(b + 0x250, 0x70);
    ad9361_spi_write(b + 0x23b, 0x80 | lut[6]);
    ad9361_spi_write(b + 0x23e, lut[8] | (lut[7] << 4));
    ad9361_spi_write(b + 0x23f, lut[10] | (lut[9] << 4));
    ad9361_spi_write(b + 0x240, lut[11]);
    ad9361_spi_write(b + 0x233, nfrac & 0xFF);
    ad9361_spi_write(b + 0x234, (nfrac >> 8) & 0xFF);
    ad9361_spi_write(b + 0x235, (nfrac >> 16) & 0xFF);
    ad9361_spi_write(b + 0x232, (nint >> 8) & 0xFF);
    uint64_t t0 = timer_ticks();
    ad9361_spi_write(b + 0x231, nint & 0xFF);
    uint8_t divs = ad9361_spi_read(0x005);
    divs = d == RF_TX ? (divs & 0x0F) | (i << 4) : (divs & 0xF0) | i;
    ad9361_spi_write(0x005, divs);

    int locked = 0, n = 0;
    for (;; n++) {
        uint8_t v = ad9361_spi_read(b + 0x247);
        if (trace && n < *ntrace)
            trace[n] = v;
        if ((v & 0x02) && !(trace && n < *ntrace)) {
            locked = 1;
            break;
        }
        if (timer_ticks() - t0 > TIMER_US(3000))
            break;
        if (trace)
            delay_us(10);
    }
    if (trace)
        *ntrace = n < *ntrace ? n : *ntrace;
    if (lock_us)
        *lock_us = (uint32_t)((timer_ticks() - t0) / (CPU_HZ / 1000000u));
    set(actual, actual_vcorate / vcodiv);
    return locked || trace ? 0 : -1;
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
