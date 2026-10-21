#!/bin/bash
# Run on Vega-1:  sudo ./vega_setup.sh
#
# Installs usbip-autoattach.service, which attaches every device the VM exports
# through the reverse tunnel (localhost:3240) and re-attaches it after the FX3
# re-enumerates.
set -euo pipefail
[ "$(id -u)" -eq 0 ] || { echo "run with sudo"; exit 1; }

modprobe vhci-hcd
echo vhci-hcd > /etc/modules-load.d/usbip.conf

cat > /usr/local/sbin/usbip-autoattach <<'EOF'
#!/bin/bash
# Attach everything exported on the tunnel endpoint that isn't attached yet.
HOST="${USBIP_HOST:-127.0.0.1}"
while true; do
    if (exec 3<>/dev/tcp/$HOST/3240) 2>/dev/null; then
        attached="$(usbip port 2>/dev/null | sed -n 's#.*usbip://[^/]*/\(.*\)$#\1#p')"
        for busid in $(usbip list -r "$HOST" 2>/dev/null | sed -n 's#^ *\([0-9][0-9.-]*\): .*#\1#p'); do
            grep -qx "$busid" <<<"$attached" && continue
            usbip attach -r "$HOST" -b "$busid" && logger -t usbip-autoattach "attached $busid from $HOST"
        done
    fi
    sleep 2
done
EOF
chmod 755 /usr/local/sbin/usbip-autoattach

cat > /etc/systemd/system/usbip-autoattach.service <<'EOF'
[Unit]
Description=Attach the remote SDR exported over the USB/IP tunnel
After=network.target

[Service]
ExecStartPre=/sbin/modprobe vhci-hcd
ExecStart=/usr/local/sbin/usbip-autoattach
Restart=always

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl enable --now usbip-autoattach.service
systemctl --no-pager --lines=0 status usbip-autoattach || true
