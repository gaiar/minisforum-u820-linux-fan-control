#!/bin/sh
# Read existing Linux interfaces only; do not probe buses or change cooling.
set -u

section() { printf '\n== %s ==\n' "$1"; }
read_attribute() {
    [ -f "$1" ] || return 0
    printf '%s=' "$1"
    cat "$1" 2>/dev/null || printf '(unreadable)\n'
}

section 'Capture time and system'
date -u '+%Y-%m-%dT%H:%M:%SZ'
hostname
uname -a
cat /etc/os-release
id
uptime
section 'DMI identity (no serial numbers)'
for field in sys_vendor product_name product_version board_vendor board_name \
    board_version bios_vendor bios_version bios_date; do
    read_attribute "/sys/class/dmi/id/$field"
done

section 'Existing hwmon interfaces (raw kernel units)'
for device in /sys/class/hwmon/hwmon*; do
    [ -d "$device" ] || continue
    printf '\n%s -> %s\n' "$device" "$(readlink -f "$device")"
    for attribute in "$device"/name "$device"/temp* "$device"/fan* \
        "$device"/pwm* "$device"/in[0-9]* "$device"/curr* \
        "$device"/power* "$device"/energy* "$device"/humidity*; do
        read_attribute "$attribute"
    done
done

section 'Thermal zones and cooling devices'
for device in /sys/class/thermal/thermal_zone* /sys/class/thermal/cooling_device*; do
    [ -d "$device" ] || continue
    for attribute in "$device"/type "$device"/temp "$device"/policy \
        "$device"/cur_state "$device"/max_state "$device"/trip_point*; do
        read_attribute "$attribute"
    done
done

section 'Powercap interfaces'
for device in /sys/class/powercap/intel-rapl:*; do
    [ -d "$device" ] || continue
    read_attribute "$device/name"
    read_attribute "$device/max_energy_range_uj"
    if sudo -n true 2>/dev/null; then
        printf '%s=' "$device/energy_uj"
        sudo -n cat "$device/energy_uj" || true
    else
        read_attribute "$device/energy_uj"
    fi
done
section 'Loaded drivers'
lsmod
section 'PCI identifiers and bound drivers'
lspci -nnk
section 'ACPI devices'
for device in /sys/bus/acpi/devices/*; do
    [ -d "$device" ] || continue
    printf '%s ' "${device##*/}"
    cat "$device/path" 2>/dev/null || printf '\n'
done
section 'Kernel cooling and controller messages'
if sudo -n true 2>/dev/null; then
    sudo -n dmesg | grep -iE 'embedded|EC:|hwmon|thermal|fan|wmi|resource conflict|IT87|nct67' || true
    section 'ACPI-reserved I/O resources'
    sudo -n cat /proc/ioports
fi
section 'Available investigation tools'
for tool in sensors acpidump iasl dmidecode gcc make dkms; do
    command -v "$tool" || true
done
section 'Plex service state'
systemctl is-active plexmediaserver || true
if sudo -n true 2>/dev/null && command -v docker >/dev/null 2>&1; then
    sudo -n docker ps --filter name=plex --format '{{.Names}}: {{.Status}}'
fi
section 'Collection complete; no control writes performed'
