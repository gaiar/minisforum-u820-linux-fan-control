#!/bin/sh
# Install the validated U820-specific driver and daemon; never flash firmware.
set -eu

if [ "$(id -u)" -ne 0 ]; then
    echo "Run as root on the Ubuntu U820." >&2
    exit 1
fi
for pair in product_name:U820 board_name:CB9 bios_version:CB9_FV102; do
    field=${pair%%:*}
    expected=${pair#*:}
    if [ "$(cat "/sys/class/dmi/id/$field")" != "$expected" ]; then
        echo "Unsupported machine or BIOS: $field" >&2
        exit 1
    fi
done
project_root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
for command in dkms make python3 modprobe systemctl; do
    command -v "$command" >/dev/null
done
test -d "/lib/modules/$(uname -r)/build"
echo "Building and installing the kernel driver with DKMS…"
install -d /usr/src/u820-ec-0.2.0/kernel
install -m 0644 "$project_root/dkms.conf" /usr/src/u820-ec-0.2.0/
install -m 0644 "$project_root/kernel/Makefile" \
    "$project_root/kernel/u820_ec_probe.c" "$project_root/kernel/u820_control.h" \
    /usr/src/u820-ec-0.2.0/kernel/
if ! dkms status -m u820-ec -v 0.2.0 | grep -q .; then
    dkms add -m u820-ec -v 0.2.0
fi
dkms build --force -m u820-ec -v 0.2.0 -k "$(uname -r)"
dkms install --force -m u820-ec -v 0.2.0 -k "$(uname -r)"
echo "Installing the isolated Python runtime and service…"
install -d /opt/u820-fan /etc/modprobe.d /etc/modules-load.d
python3 -m venv --without-pip /opt/u820-fan/venv
install -m 0644 "$project_root/u820_monitor.py" "$project_root/u820_fan.py" /opt/u820-fan/
install -m 0644 "$project_root/deploy/u820-fan.service" /etc/systemd/system/
install -m 0644 "$project_root/deploy/u820-fan.conf" /etc/modprobe.d/u820-fan.conf
install -m 0644 "$project_root/deploy/u820-fan.modules" /etc/modules-load.d/u820-fan.conf
if [ ! -e /etc/u820-fan.json ]; then
    install -m 0644 "$project_root/deploy/u820-fan.json" /etc/u820-fan.json
fi
modprobe u820_ec_probe
/opt/u820-fan/venv/bin/python /opt/u820-fan/u820_fan.py check
systemctl daemon-reload
systemctl enable u820-fan.service
echo "Installation complete. Start with: systemctl start u820-fan.service"
echo "Stop and restore firmware cooling with: systemctl stop u820-fan.service"
