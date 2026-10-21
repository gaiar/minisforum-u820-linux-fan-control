# Experimental hardware control: use at your own risk

By choosing to install, run, modify, or tune this software, **you accept
responsibility for your own hardware, configuration, testing, data, and use**.

This project is experimental software that communicates with the embedded
controller and changes fan operation. It has been tested on one specific
MINISFORUM U820 / CB9 configuration. An apparently similar machine, a different
firmware build, or a future kernel may behave differently. The compatibility
checks and operating limits are deliberate; do not bypass them.

Incorrect behavior or configuration can cause inadequate cooling, overheating,
instability, unexpected shutdowns, service interruption, data loss, or hardware
damage. Lower fan noise can come at the expense of cooling and sustained
performance. You are responsible for deciding whether those risks are
acceptable and for observing temperatures and fan feedback on your machine.

The kernel lease, sensor checks, thermal override, and automatic restoration
are precautions, not guarantees. A kernel crash, unresponsive embedded
controller, firmware interaction, or failed fan can defeat them. No independent
EC hardware timeout has been established. Existing CPU thermal protections do
not establish adequate cooling for every other component in the enclosure.

The software is provided **“as is,” without warranty of any kind**, under the
terms of the [MIT license](LICENSE), with a GPL-2.0-only alternative for the
kernel source as described in [LICENSING.md](LICENSING.md). Both licenses include
warranty disclaimers and limitations of liability. To the extent permitted by
applicable law, the authors
and contributors disclaim responsibility for losses or damage arising from
its use. This notice does not override rights that cannot legally be excluded.

This is an independent community project. It is not an official MINISFORUM,
Intel, Ubuntu, or hardware-vendor product and is not endorsed by those vendors.
Product names and trademarks belong to their respective owners. No vendor
firmware or proprietary update utility is licensed or distributed here.
