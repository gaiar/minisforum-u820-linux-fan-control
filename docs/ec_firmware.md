# U820 EC firmware findings

The separate U820 auto-power-on package contains the strongest fan-control
evidence: an 8051 EC image, with an extended RAM access protocol and explicit
fan routines. This led to a working Linux EC driver. Selected protocol, duty and restoration
behavior is now validated live; full equivalence of the EC builds remains unknown.

**Seen** means directly present in a file or recorded observation. Offline
execution is stated explicitly. **Inferred** is a supported interpretation.
**Unknown** means unresolved, not absent.

Paths under `evidence/` and `$TMPDIR/re/` identify local research captures;
they are not shipped in the public repository. Firmware binaries and vendor
utilities are not redistributed. Hashes and offsets below document provenance.

## Inputs and provenance

| Input | SHA-256 |
| --- | --- |
| U820_BIOS.rar | `780f37c484402fcbbdb4689656c38b0304da1b4173d8cf927547209c75fe53fe` |
| EFI.zip | `1458f98ce077a59f85aa9bb37dc05c5bd906abc50d60d5541fc27007972ab6ca` |
| USB BIOS UPDATE_fixed.pdf | `d178eccbee95d6dcb7b212ad62552d43a004b6aebd9976624d5bb25ac13d31ca` |
| CB9_FV102.bin | `fcbbca981771c2ef6658b561090268eec50008159ea54a9bbfcbcae4221a7537` |
| U820(auto power on).rar | `6d167409ef342e1a5894b8e72fcad20e6d675880f8dbe384313d5edf5b616ee6` |
| CB9EC_V1.06.BIN | `8b24e51229b1caa6023e30176f4aedba063facb54f0dc8d55fcb1bb34a7ebd5c` |
| eFuitX64.efi | `72c90dccb292255302d290aa16721c33237a5a30aa0f3b0ab508613e8620d866` |

The auto-power-on archive came from a privately held historical U820 firmware
collection. It supplies artifacts only; this project's target is Ubuntu.
Originals were preserved and copied into `$TMPDIR/re/u820-20261021`.
System `bsdtar -tf` inventoried archives; `bsdtar -xf ... -C ...` extracted them
into `bios/`, `efi/`, and `auto-power-on/`. `file`, `xxd`, and bounded strings
searches preceded disassembly. Main BIOS analysis used UEFIExtract A75,
Apple LLVM objdump, and IFRExtractor 1.6.1. ACPI used iasl 20260408.

**Seen:** the supplied main image is CB9_FV102 and is byte-identical to the
historical repository's copies. The live DMI reports that version, but a full
live flash-image comparison has not been performed. The support email does not
explicitly confirm this is the latest release or provide a changelog.

**Seen:** the main U820 archive contains Intel FPT tools and a full-flash/reset
batch file. The general guide illustrates AMI/AfuEfix64 and a different ROM;
EFI.zip's startup references files not supplied in either package. These are
not an established U820 update procedure. No updater was executed.

## Controller and image identity

**Seen:** `FEC64.NSH` names ITE and “IT8987E eFlash Update” and selects
`CB9EC_V1.06.BIN`. The image is 128 KiB with 8051 code and banking trampolines.
Its additive checksum matches the package's `0x17c1320`.

**Inferred:** the board's EC is in the IT8987E family. The physical chip marking
or a hardware identification register has not yet been checked.

**Seen in file:** `ITE EC-V14.4` at `0x50` is a library identifier. Date and time
are at `0x26d2` and `0x26dd`; `CB9EC_V1.06` appears at `0x2700`.
Initialization at `0xe4c7` writes version bytes **01 05 01 00**, date
`2021/01/20`, and time `12:21:32` into XRAM `0x0400`, `0x0431`, and `0x0422`.
Offline execution confirms this: the filename is not the EC's reported version.

**Seen live:** the existing 256-byte ACPI EC window reports the same version
bytes and date, but time **11:28:11**. The builds differ. Version `1.05` alone
is not a sufficient compatibility check. `evidence/ec-baseline.bin` preserves
the initial read.

## Host protocol

These are **seen in the downloaded file and executed offline**. A2/A3/A4 reads
and allowlisted A5 writes were subsequently demonstrated on the live U820:

| Command | Input | Effect | Code offsets |
| --- | --- | --- | --- |
| `80` | one offset byte | Read XRAM `0400 + offset` | `01aa`, `b28d` |
| `A2` | one byte | Set vendor pointer low byte at `00cb` | `b121–b135` |
| `A3` | one byte | Set vendor pointer high byte at `00ca` | `b136–b14a` |
| `A4` | none | Return byte at pointer through output `1501` | `b1a7–b1c4`, `b31c` |
| `A5` | one byte | Write byte at pointer | `b150–b17b` |

Dispatch at `012c` runs the base handler and then the OEM handler. The normal
ACPI `80` read preserves the vendor pointer, including when interleaved between
vendor transactions; this was checked offline. The kernel probe holds its own
mutex across A2/A3/A4 and uses Linux `ec_transaction()` for each transaction.
It must not run alongside another vendor EC debugger. SMM or other firmware
activity touching this pointer remains a live uncertainty.

**Important discovered incompatibility:** base command `92` initially requests
two data bytes, but OEM code at `b1a3` overwrites the count with one. Generic
16-bit EC read/write recipes using `92/93` are not valid for this image.
The driver defaults to read-only A2/A3/A4. Explicit control parameters enable
allowlisted A5 writes; arbitrary EC writes are never exposed.

Linux's existing
[ec_sys](https://raw.githubusercontent.com/torvalds/linux/v6.8/drivers/acpi/ec_sys.c)
exposes the 256-byte ACPI window. The
[ACPI EC driver](https://raw.githubusercontent.com/torvalds/linux/v6.8/drivers/acpi/ec.c)
exports `ec_transaction()` for the additional commands. This is the basis for
the custom probe; raw I/O ports or changing the chipset LGMR are unnecessary.

## Fan routines

**Seen in file:** scheduled thunk `2c10` reaches `cef6`, then routine `c149`.
For state byte `00b0 == 1`, it examines mode `080a`:

| Mode | Register `1808` | Register `1807` |
| --- | --- | --- |
| `00` | Automatic from `0470` | Unchanged by this routine |
| `81` | 95 | 95 |
| `82` | 127 | 127 |
| `83` | 200 | 200 |
| `80` | 0 | 0 |
| `84` | Preserve host-written duty | Unchanged |

These are raw output codes, not calibrated percentages. Mode `80` stops the
outputs and is not a candidate user feature. Modes other than the recognized
values can leave the previous outputs in place. Outside state 1, the routine
sets `1808` to zero regardless of the mode.

Automatic `1808` output for temperature byte T is:

| T | Raw output |
| --- | --- |
| Below 30 | 0 |
| 30–44 | 46 |
| 45–54 | 56 |
| 55–64 | 74 |
| 65–84 | `min((50*T + 4150) / 100, 92)`, integer division |
| 85 or above | 92 |

**Seen offline:** all 256 temperature values were exercised through all three
manual modes and back to automatic. `1808` returns to its automatic result;
**1807 retains the manual value**. Thus writing mode 00 alone does not restore
all affected registers. The role/wiring of 1807 and a complete restoration
procedure was validated live: clear mode first, then restore 1807 to its
observed baseline zero. Physical wiring of 1807 remains unknown.

**Inferred:** `1807/1808` are PWM duties. Their addresses correspond to PWM
registers in the related
[Chromium IT83xx source](https://raw.githubusercontent.com/coreboot/chrome-ec/main/chip/it83xx/registers.h).
That source is supporting family context, not an IT8987E board specification.
An alternate fan routine exists at `c087`; its active use remains unknown.

## Tachometers and temperature derivations

**Seen in file and offline:** `cfd9–cfed` reads raw period `181f:181e`, converts
it via `c039`, and stores a big-endian count at `080b:080c`. The conversion is
zero for zero period, `2156250 / period` for periods 1–6999, and **7000 for
periods 7000 or above**. That last branch is unusual and must not be interpreted
as proof of a 7000 RPM physical fan. Some very small periods can also produce
wrapped 16-bit results. The probe rejects stored values above 7000, but that
does not establish their physical validity.

`d035–d049` applies the same conversion to `1821:1820`, storing `083d:083e`.
Whether a second physical tachometer is wired is unknown. The driver
exposes the first count as `fan1_input`, validated live by monotonic duty
response from approximately 1,545 to 3,520 RPM, and keeps
the second as raw bytes in its register snapshot.

**Seen in file and offline:** `ced3–cef1` sets `0471 = max(0470, 0462)` in
state 1. Thus EC byte 71 is a derived value, not evidence of another sensor.
The live baseline values agree with this relation.

**Seen in file:** `0470` takes `03b8`, or the nonzero override `0495` when at
most 100, otherwise 95 (`ceaf–ced2`). It feeds the fan curve. `0462` and `0472`
are filtered values produced by separate paths through lookup tables and
ADC-like registers (`b81f–ba80`, `bae6`). Physical locations and calibration
remain unvalidated. They are retained as raw candidate values in userspace.

## Reproduce the offline execution

Use the MIT-licensed
[emu8051 source](https://github.com/jarikomppa/emu8051/tree/5dc681275151c4a5d7b85ec9ff4ceb1b25abd5a8)
at commit `5dc681275151c4a5d7b85ec9ff4ceb1b25abd5a8`. Put `core.c`, `opcodes.c`,
`disasm.c`, `emu8051.h`, and `LICENSE` in a temporary directory. No installation
or server access is needed.

```sh
sh scripts/check-firmware.sh \
  "$TMPDIR/re/u820-20261021/auto-power-on/U820(auto power on)/CB9EC_V1.06.BIN" \
  "$TMPDIR/re/u820-20261021/tools/emu8051"
```

The runner verifies the exact firmware and emulator SHA-256 hashes. The harness
compiles with `-Wall -Wextra -Werror` and executes the original 8051 bytes,
including arithmetic and command dispatch. Peripheral registers are ordinary
emulated memory; this is not hardware/timing validation. Banked maintenance
outside the tach calculation is deliberately excluded by stopping at `cfee`.
Results are saved in `evidence/firmware-emulation.txt`.

The disassembly was produced with radare2 6.2.4 using `-a 8051 -b 8` and
`pD 131072 @ 0`. Linear output contains data and banked regions; only traced
instruction boundaries support the claims above. Relevant source files,
disassembly, and scripts are preserved under `evidence/firmware/`.


## Live validation and leased manual duty

**Seen live, 2026-10-21:** kernel 6.8.0-138-generic; EC build 11:28:11.
Twenty-two identity bytes agree through ACPI and extended reads. Presets 1/2
reach approximately 2,875/3,529 RPM. Preset 3 (raw 200) returned zero RPM;
its cause is unknown and the driver rejects it. Do not use it as a maximum.
Raw counter/configuration snapshots include 1801=c8, 1841=c8, 1842=b7,
180d=24. These do not by themselves establish a calibrated duty percentage.

Mode 84 plus a direct 1808 write successfully controls speed without changing
1807. The driver exposes root-only `fan_pwm_raw`: zero releases control, and
46–127 sets a raw duty and renews a three-second lease. A 500 ms kernel worker
checks state, mode, duty, secondary PWM, EC temperature and plausible tachometer.
Deadline expiry or failed feedback restores mode zero. The kernel module stays
pinned until restoration succeeds; failed restoration is retried. Reboot and
suspend notifications also attempt restoration. A crashed kernel or failed EC
can defeat this host-side safeguard; no independent EC timeout is established.

**Seen live:** a duty-56 lease reached about 1,855 RPM, expired without a user
heartbeat, and returned to mode zero, duty 46 and about 1,540 RPM. The subsequent
sweep validated duties 46, 56, 68, 82, 95 and 127. Captures:
`evidence/live-manual-lease.txt`, `evidence/noise-sweep-full.jsonl`.

The portable C control core is shared by the kernel, fault-injection tests and
firmware emulator. The latter executes 180,769 assertions, including all duties
46–127 at temperatures 20–89 and full restoration. A real clangd LSP session
returned zero diagnostics: `evidence/clangd-manual-lsp.json`. GCC W=1 with
KCFLAGS=-Werror and local clangd checks also passed. `.clangd` removes only
GCC-specific code-generation flags unsupported by clangd, retaining kernel
includes and definitions.

**Inferred:** CPU-derived EC input uses PECI signed 10.6 format. Routine d2b9
negates 03c9:03c8, shifts by six, subtracts from 03cd (or 100), and stores 03b8.
This matches the conversion in Linux's
[PECI CPU temperature driver](https://raw.githubusercontent.com/torvalds/linux/v6.8/drivers/hwmon/peci/cputemp.c).
The daemon uses independent, established coretemp readings rather than relying
on this inference or writing the EC temperature override.

To regenerate the kernel compilation database on Ubuntu after a build:

```sh
.venv/bin/python /usr/src/linux-headers-$(uname -r)/scripts/clang-tools/gen_compile_commands.py \
  -d /usr/src/linux-headers-$(uname -r) -o kernel/compile_commands.json kernel
clangd --check=kernel/u820_ec_probe.c --compile-commands-dir=kernel
```

For the actual LSP validation, the retained local runner
`evidence/lsp_check.py` sends initialize, didOpen, documentSymbol, shutdown and
exit, and requires an empty publishDiagnostics result. Logs include source paths,
diagnostic arrays and symbol counts. Compilation databases are host-specific and
excluded from version control.
