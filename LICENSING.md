# Licensing

**MIT is the project's default license.** Unless a file states otherwise,
original source code, scripts, configuration and documentation in this project
are provided under the [MIT license](LICENSE).

## Kernel source

These original files are offered under **GPL-2.0-only OR MIT**:

- `kernel/u820_ec_probe.c`
- `kernel/u820_control.h`

You may choose either license for these source files. Their SPDX identifiers
state that choice; the module declares `MODULE_LICENSE("Dual MIT/GPL")`, a
recognized Linux module license tag. The alternative GPL text is preserved in
[LICENSES/GPL-2.0-only.txt](LICENSES/GPL-2.0-only.txt).

This arrangement keeps the MIT option for reuse of our original source and a
GPL-compatible path for Linux kernel integration. It does not relicense Linux,
its headers, or other third-party material. Distributing a built module or a
combined work still requires complying with the licenses of the material it
incorporates. The module tag is not a replacement for source license notices.

See the Linux kernel's [licensing rules](https://docs.kernel.org/process/license-rules.html)
for dual-license SPDX expressions and the `Dual MIT/GPL` module tag.

## Dependencies, history and warranty

Third-party dependencies retain their own licenses. The optional emu8051
research dependency is obtained separately and has its own MIT notice; it is
not vendored here. Firmware binaries and proprietary vendor tools are not
distributed or relicensed by this project.

The initial public snapshot was offered under GPL-2.0-only. This update adds
the MIT option for our original code; it does not withdraw licenses already
granted for earlier copies. The Git history is preserved.

The license warranty exclusions remain in effect. Also read the
[hardware-risk disclaimer](DISCLAIMER.md) before installing or tuning cooling.
