#!/bin/bash
# Run on the Ubuntu Server VM that has the LibreSDR B220 passed through to it:
#   sudo ./vm_setup.sh [VEGA_HOST] [VEGA_USER]
#
# Sets up:
#   - usbip tools + usbip-host module (loaded at boot)
#   - usbipd.service          : USB/IP server on port 3240
#   - usbip-autobind.service  : keeps the SDR exported (the FX3 re-enumerates
#                               every time UHD loads its firmware)
#   - usbip-tunnel.service    : reverse SSH tunnel, Vega-1 localhost:3240 -> VM:3240
#                               (encrypted; nothing is opened to the network)
set -euo pipefail

VEGA_HOST="${1:-192.168.192.71}"
VEGA_USER="${2:-vega-1}"
CONF_DIR=/etc/usbip-tunnel

[ "$(id -u)" -eq 0 ] || { echo "run with sudo"; exit 1; }

echo "== Installing usbip tools"
# (a broken third-party repository must not stop the setup)
apt-get update -qq || echo "== apt-get update reported errors, continuing"
apt-get install -y -qq linux-tools-common "linux-tools-$(uname -r)" linux-tools-generic usbutils openssh-client
if ! modprobe usbip-host 2>/dev/null; then
    apt-get install -y -qq "linux-modules-extra-$(uname -r)"
    modprobe usbip-host
fi
echo usbip-host > /etc/modules-load.d/usbip.conf

echo "== Installing services"
cat > /usr/local/sbin/usbip-autobind <<'EOF'
#!/bin/bash
# Bind any Ettus/FX3 device to usbip-host so it stays exported after re-enumeration.
IDS="${USBIP_IDS:-2500:0020 2500:0021 2500:0022 3923:7813 3923:7814 04b4:00f3}"
while true; do
    for d in /sys/bus/usb/devices/*; do
        [ -f "$d/idVendor" ] || continue
        id="$(cat "$d/idVendor"):$(cat "$d/idProduct")"
        case " $IDS " in *" $id "*) ;; *) continue ;; esac
        drv=""
        [ -e "$d/driver" ] && drv="$(basename "$(readlink -f "$d/driver")")"
        if [ "$drv" != usbip-host ]; then
            busid="$(basename "$d")"
            usbip bind -b "$busid" && logger -t usbip-autobind "bound $busid ($id)"
        fi
    done
    sleep 1
done
EOF
chmod 755 /usr/local/sbin/usbip-autobind

cat > /etc/systemd/system/usbipd.service <<'EOF'
[Unit]
Description=USB/IP server
After=network.target

[Service]
ExecStartPre=/sbin/modprobe usbip-host
ExecStart=/usr/bin/usbipd -4
Restart=always

[Install]
WantedBy=multi-user.target
EOF

cat > /etc/systemd/system/usbip-autobind.service <<'EOF'
[Unit]
Description=Keep the SDR bound to usbip-host
After=usbipd.service

[Service]
ExecStart=/usr/local/sbin/usbip-autobind
Restart=always

[Install]
WantedBy=multi-user.target
EOF

echo "== Setting up the SSH tunnel key"
mkdir -p "$CONF_DIR"
chmod 700 "$CONF_DIR"
[ -f "$CONF_DIR/id_ed25519" ] || ssh-keygen -q -t ed25519 -N "" -C "usbip-tunnel@$(hostname)" -f "$CONF_DIR/id_ed25519"
SSH_OPTS=(-o UserKnownHostsFile="$CONF_DIR/known_hosts" -o StrictHostKeyChecking=accept-new)

echo "Installing the tunnel key on $VEGA_USER@$VEGA_HOST (asks for the Vega-1 password once)."
echo "The key can only forward Vega-1's localhost:3240; it gets no shell."
PUB="$(cat "$CONF_DIR/id_ed25519.pub")"
ssh "${SSH_OPTS[@]}" "$VEGA_USER@$VEGA_HOST" \
    "grep -qF '$PUB' ~/.ssh/authorized_keys 2>/dev/null || echo 'restrict,port-forwarding,permitlisten=\"localhost:3240\" $PUB' >> ~/.ssh/authorized_keys"

cat > /etc/systemd/system/usbip-tunnel.service <<EOF
[Unit]
Description=Reverse SSH tunnel: $VEGA_HOST localhost:3240 -> local usbipd
After=network-online.target usbipd.service
Wants=network-online.target

[Service]
ExecStart=/usr/bin/ssh -N -i $CONF_DIR/id_ed25519 -o UserKnownHostsFile=$CONF_DIR/known_hosts \\
    -o ExitOnForwardFailure=yes -o ServerAliveInterval=15 -o ServerAliveCountMax=3 -o BatchMode=yes \\
    -R localhost:3240:localhost:3240 $VEGA_USER@$VEGA_HOST
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF

# usbipd listens on all interfaces; only the tunnel (loopback) should reach it.
if command -v ufw >/dev/null && ufw status | grep -q "Status: active"; then
    ufw deny 3240/tcp
fi

systemctl daemon-reload
systemctl enable --now usbipd.service usbip-autobind.service usbip-tunnel.service

echo
echo "== Done. Status:"
systemctl --no-pager --lines=0 status usbipd usbip-autobind usbip-tunnel || true
echo
echo "Exported devices (the SDR should be listed once it's passed through to this VM):"
usbip list -l
