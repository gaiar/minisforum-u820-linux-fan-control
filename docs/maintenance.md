# Operation and recovery

## Installed target

The reference target is an Ubuntu U820; the research workstation is separate.
The reference machine used Secure Boot disabled. Live-tested kernel:
6.8.0-138-generic. The driver is installed through
DKMS as `u820-ec/0.2.0`; daemon code and its isolated Python environment are in
`/opt/u820-fan`. The systemd unit is `u820-fan.service`.

Configuration: `/etc/u820-fan.json`. Change it locally, then restart the service.
Invalid curves, duties and timing parameters fail before fan control begins.
Use `cat /run/u820-fan/state.json` for the last sample and decision; timestamp
freshness matters. `u820_fan.py check` reads actual CPU and fan feedback.

```sh
sudo systemctl stop u820-fan
sudo /opt/u820-fan/venv/bin/python /opt/u820-fan/u820_fan.py check
sudo systemctl start u820-fan
journalctl -u u820-fan -n 30
```

Stopping releases control immediately. An abrupt process death leaves at most
the remaining three-second lease plus the watchdog's next 500 ms tick and EC
transaction latency. The kernel retries failed automatic restoration and
prevents module unload until it succeeds. The service does not automatically
restart after a failure, avoiding repeated acquisition after a hardware fault.

The driver also checks the EC temperature/state, control-mode readback,
secondary output and tachometer while leased. Userspace independently reads
all five coretemp inputs. Existing CPU hardware throttling and thermald remain
active. A stalled kernel or EC can defeat the host-side watchdog; no independent
hardware timeout is known. Do not write to vendor EC pointers with another tool.

## Reboot and kernel updates

`/etc/modules-load.d/u820-fan.conf` loads the module; its options are in
`/etc/modprobe.d/u820-fan.conf`. DKMS builds for installed kernels when matching
headers are present. A missing module means firmware retains cooling. The
service checks for the loaded module, and identity mismatches stop driver load.
Do not force a module with a wrong vermagic or bypass the identity check.

A post-install reboot is deliberately not performed on this running server.
Boot persistence is configured, but must be checked on the next normal reboot:
`dkms status`, `systemctl status u820-fan`, `u820_fan.py check`, and application
health. New Secure Boot enrollment/enablement also requires reviewing the DKMS
signing key; no key was enrolled during this work.

Turbo was re-enabled at the owner's request (`intel_pstate/no_turbo=0`). The
existing 70% performance-cap service is disabled. The existing 85°C TCC target
(100°C nominal minus a 15°C offset) is unchanged. Fan speed cannot guarantee
unthrottled performance when heat transfer or airflow is the limiting factor.

## Remove persistent fan control

```sh
sudo systemctl disable --now u820-fan
sudo rm /etc/modules-load.d/u820-fan.conf /etc/modprobe.d/u820-fan.conf
sudo modprobe -r u820_ec_probe
sudo dkms remove -m u820-ec -v 0.2.0 --all
```

Preserve the configuration and research evidence if further investigation is
planned. Firmware automatic cooling does not depend on this daemon.

On the reference server, `/etc/systemd/system/cpu-performance.service.d/u820-turbo.conf`
is the intended Turbo persistence drop-in for the pre-existing performance
service. Its `ExecStartPost` writes `no_turbo=0` after that service sets its
existing governor/performance limits. It does not change TCC, PL1/PL2 or voltage.
The general installer does not assume this site-specific service exists;
`deploy/u820-turbo.conf` supplies the optional drop-in.
