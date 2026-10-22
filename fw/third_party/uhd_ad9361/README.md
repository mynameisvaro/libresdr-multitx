# UHD AD9361 driver

From [Ettus Research UHD](https://github.com/EttusResearch/uhd), tag `v4.6.0.0`
(commit `50fa3baa2e11ea3b30d5a7e397558e9ae76d8b00`), directory
`host/lib/usrp/common/ad9361_driver/`. Licence: GPL-3.0-or-later, the same as
this project.

This is the driver UHD uses for the B200/B210, so it already knows this board's
wiring (CMOS full port, P0 = RX, P1 = TX, 40 MHz reference on XTAL_N). The firmware
wraps it in [`src/rf.cpp`](../../src/rf.cpp).

## Changes from upstream

- The filter API (`get_filter`, `set_filter`, `get_filter_names` and their helpers)
  is wrapped in `#ifndef AD9361_NO_FILTER_API`, and the firmware defines that macro.
  It needs RTTI (`dynamic_pointer_cast`), `std::function` and `std::map`, which cost
  too much code space on the MicroBlaze. Nothing else is changed.

The UHD and Boost headers the driver includes are replaced by small stand-ins in
[`src/uhd_shim/`](../../src/uhd_shim): exceptions without `std::string`, logging
compiled out, a minimal `boost::format`, and a no-op `std::recursive_mutex` (the
firmware is single-threaded).

To update, copy the six files from a newer UHD tag and re-apply the guards (search
for `AD9361_NO_FILTER_API`).
