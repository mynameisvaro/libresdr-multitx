# remote_usb/

Scripts for using the board when it's plugged into a different machine than the
one running Vivado. The setup is explained in [docs/remote-usb.md](../docs/remote-usb.md).

| Script | Runs on | |
|---|---|---|
| [`vega_setup.sh`](vega_setup.sh) | dev machine | loads `vhci-hcd` and installs `usbip-autoattach.service` |
| [`vm_setup.sh`](vm_setup.sh) | board host | installs `usbipd`, the auto-bind service and the reverse SSH tunnel |
| [`vm_load_fpga.sh`](vm_load_fpga.sh) | board host | loads FX3 firmware + a bitstream locally, then re-shares the board |
