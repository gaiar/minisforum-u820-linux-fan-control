# Install Minisforum U820 fan control on Ubuntu Server

Read the [hardware-risk disclaimer](../DISCLAIMER.md). Installation changes
kernel-module and boot-service configuration; starting the service changes fan
control. It does not flash firmware or change CPU power/thermal limits.

## 1. Confirm the machine

The only live-validated target is U820 / CB9, i5-8279U, BIOS CB9_FV102,
EC 1.05 built 2021/01/20 **11:28:11**, on Ubuntu 22.04.5 x86-64 with
kernel 6.8.0-138-generic. Inspect the non-serial DMI identifiers:

```sh
for field in product_name board_name bios_version; do
    printf '%s: ' "$field"
    cat "/sys/class/dmi/id/$field"
done
uname -r
```

Expected values are `U820`, `CB9`, and `CB9_FV102`. The driver separately checks
the EC identity and protocol. A version label of 1.05 alone is insufficient;
another known image has a different build time. Unsupported identity causes
installation/module loading to fail. Do not flash a BIOS to match this project
or remove the guard as an installation shortcut.

## 2. Prepare dependencies and Secure Boot

```sh
sudo apt-get update
sudo apt-get install git dkms build-essential linux-headers-$(uname -r) python3-venv
```

Keep the headers metapackage matching your Ubuntu kernel track installed so
future kernels receive matching headers. For the tested Ubuntu 22.04 HWE track
this is `linux-headers-generic-hwe-22.04`; other tracks use different packages.
Do not switch kernel tracks simply to install this driver.

Secure Boot must allow the locally built module. The reference machine used
Secure Boot disabled. If you keep it enabled, arrange module signing and
enrollment of a trusted key before loading the module; this installer does not
enroll a key. See Ubuntu's [Secure Boot documentation](https://wiki.ubuntu.com/UEFI/SecureBoot).
`mokutil --sb-state` shows the state when `mokutil` is installed. A rejected key
or lockdown error is not fixed by bypassing hardware identity checks.

## 3. Build and install

```sh
git clone https://github.com/gaiar/minisforum-u820-linux-fan-control.git
cd minisforum-u820-linux-fan-control
less scripts/install-u820.sh
cat deploy/u820-fan.json
sudo sh scripts/install-u820.sh
```

The installer:

1. Checks the DMI model, board, BIOS, prerequisites, and running-kernel headers.
2. Copies source to `/usr/src/u820-ec-0.2.0` and builds/installs it through DKMS.
3. Creates `/opt/u820-fan/venv` with no third-party runtime dependencies.
4. Installs the Python tools, systemd unit, module autoload options, and default
   JSON profile. Existing `/etc/u820-fan.json` is preserved.
5. Loads the driver and checks temperature/RPM feedback without acquiring a
   manual fan lease. Enables the daemon for the next boot.

It does not immediately start or restart an existing daemon. If you are only
evaluating and do not want it to start at the next boot, disable it with
`sudo systemctl disable u820-fan`.

## 4. Check and start

```sh
sudo /opt/u820-fan/venv/bin/python /opt/u820-fan/u820_fan.py check --config /etc/u820-fan.json
sudo systemctl start u820-fan
systemctl status u820-fan --no-pager
cat /run/u820-fan/state.json
journalctl -u u820-fan -n 30 --no-pager
```

The `check` command validates the profile and reads feedback; it does not write
a fan duty. Observe plausible RPM, all CPU temperatures, and the service state
before trying ordinary workloads. Keep other component temperatures and the
machine's airflow in mind; the daemon's control input is CPU temperature.
See [tuning and rollback](configuration.md) before changing the curve.

Stop custom control at any time:

```sh
sudo systemctl stop u820-fan
```

Normal stop releases the lease and restores firmware automatic cooling. To
prevent startup after reboot, use `sudo systemctl disable --now u820-fan`.

## 5. Kernel updates and reboot

`AUTOINSTALL=yes` tells DKMS to attempt builds for new kernels. The existing
running module continues to serve the current kernel until reboot. Missing
headers or kernel API changes can prevent the new build; inspect upgrade errors
and `dkms status` before rebooting. The service requires the module to be loaded.
If it is absent at boot, custom control is skipped and firmware cooling is used.

At the next planned reboot, verify:

```sh
uname -r
dkms status
systemctl status u820-fan --no-pager
cat /run/u820-fan/state.json
sudo /opt/u820-fan/venv/bin/python /opt/u820-fan/u820_fan.py check --config /etc/u820-fan.json
```

DKMS should show `installed` for the running kernel. The reference installation
has not yet undergone a post-install reboot test; these instructions describe
configured behavior, not a claim that every future kernel has been tested.

## Updating the project

Review changes before installing. Stop the daemon and unload the old module
before replacing it so the next start uses the rebuilt driver:

```sh
sudo systemctl stop u820-fan
sudo modprobe -r u820_ec_probe
git pull --ff-only
sudo sh scripts/install-u820.sh
sudo /opt/u820-fan/venv/bin/python /opt/u820-fan/u820_fan.py check --config /etc/u820-fan.json
sudo systemctl start u820-fan
```

If unload fails, inspect the journal and driver state; do not force it. The
driver can pin itself while automatic restoration is unresolved. When a future
release changes the DKMS version, follow its migration instructions and retain
the previous working source until validation is complete.

## CPU Turbo and complete removal

The general installer does not change Turbo, governors, performance caps, or
power limits. The `deploy/u820-turbo.conf` example belongs to a separate,
pre-existing `cpu-performance.service` on the reference system; do not install
it blindly. Fan control works independently of that service.

See [maintenance](maintenance.md) for file locations, failure recovery and
complete removal. Do not run two EC writers or fan controllers simultaneously.
