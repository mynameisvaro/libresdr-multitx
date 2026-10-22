Stand-ins for the few UHD and Boost headers that `third_party/uhd_ad9361`
includes, so the driver compiles unchanged for the MicroBlaze. Only what the
driver uses is implemented. `mutex` adds a no-op `std::recursive_mutex`: the
bare-metal libstdc++ has no threads, and neither does the firmware.
