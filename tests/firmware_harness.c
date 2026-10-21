/* Offline execution of selected CB9EC routines; never accesses hardware.
 * Build with the pinned emu8051 sources described in docs/ec_firmware.md.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "emu8051.h"
#include "../kernel/u820_control.h"

static struct em8051 cpu;
static unsigned char code[65536], xram[65536], upper[128];
static unsigned int checks;

static void expect(unsigned int actual, unsigned int wanted, const char *label)
{
	if (actual != wanted) {
		fprintf(stderr, "%s: got 0x%x, expected 0x%x\n", label, actual, wanted);
		exit(1);
	}
	checks++;
}

static void exception(struct em8051 *state, int reason)
{
	fprintf(stderr, "Emulator exception %d at 0x%04x\n", reason, state->mPC);
	exit(1);
}

static void invoke_until(unsigned int address, unsigned int stop)
{
	cpu.mSFR[REG_SP] = 0xd0;
	cpu.mSFR[REG_IE] = 0;
	cpu.mSFR[REG_PSW] = 0;
	cpu.mTickDelay = 0;
	push_to_stack(&cpu, 0xff);
	push_to_stack(&cpu, 0xff);
	cpu.mPC = address;
	for (unsigned int ticks = 0; ticks < 1000000; ticks++) {
		if (cpu.mPC == stop)
			return;
		tick(&cpu);
	}
	fprintf(stderr, "Routine 0x%04x did not return; PC=0x%04x\n", address, cpu.mPC);
	exit(1);
}

static void invoke(unsigned int address)
{
	invoke_until(address, 0xffff);
}

static void command(unsigned char value)
{
	cpu.mLowerData[0x35] = value;
	cpu.mLowerData[0x36] = 0;
	cpu.mLowerData[7] = value;
	invoke(0x033e);
	cpu.mLowerData[7] = value;
	invoke(0xb1c5);
}

static void data(unsigned char value)
{
	expect(cpu.mLowerData[0x36], 1, "one data byte expected");
	cpu.mLowerData[0x37] = value;
	cpu.mLowerData[7] = cpu.mLowerData[0x35];
	invoke(0x030b);
	cpu.mLowerData[7] = cpu.mLowerData[0x35];
	invoke(0xb109);
	cpu.mLowerData[0x36]--;
}

static void select_address(unsigned int address)
{
	command(0xa2);
	data(address & 255);
	command(0xa3);
	data(address >> 8);
	expect((xram[0xca] << 8) | xram[0xcb], address, "selected address");
}

static void test_protocol(void)
{
	static const unsigned int addresses[] = {0x0400, 0x0422, 0x080a, 0x080b, 0x1808};
	for (unsigned int i = 0; i < sizeof(addresses) / sizeof(addresses[0]); i++) {
		unsigned int address = addresses[i];
		xram[address] = 0x30 + i;
		select_address(address);
		command(0xa4);
		expect(xram[0x1501], 0x30 + i, "A4 returned selected byte");
		expect(xram[address], 0x30 + i, "A4 preserved selected byte");
		/* ACPI/ec_sys traffic may run between vendor transactions. */
		xram[0x0455] = 0x69;
		command(0x80);
		data(0x55);
		expect(xram[0x1501], 0x69, "standard ACPI read");
		expect((xram[0xca] << 8) | xram[0xcb], address,
		       "standard read preserved vendor pointer");
		command(0xa4);
		expect(xram[0x1501], 0x30 + i, "A4 after interleaved ACPI read");
	}
	select_address(0x080a);
	command(0xa5);
	data(0x81);
	expect(xram[0x080a], 0x81, "offline-only A5 wrote selected byte");
	cpu.mLowerData[7] = 0x92;
	invoke(0x033e);
	expect(cpu.mLowerData[0x36], 2, "base 92 requests two bytes");
	cpu.mLowerData[7] = 0x92;
	invoke(0xb1c5);
	expect(cpu.mLowerData[0x36], 1, "OEM handler breaks base 92 protocol");
}

static void test_fan(void)
{
	static const unsigned char modes[] = {0x81, 0x82, 0x83};
	static const unsigned char outputs[] = {95, 127, 200};
	xram[0xb0] = 1;
	for (unsigned int temperature = 0; temperature < 256; temperature++) {
		unsigned int automatic;
		xram[0x470] = temperature;
		xram[0x80a] = 0;
		invoke(0xc149);
		automatic = xram[0x1808];
		expect(u820_auto_output(temperature), automatic, "driver curve matches firmware");
		if (automatic > 92) {
			fprintf(stderr, "Automatic output exceeded 92 at %u\n", temperature);
			exit(1);
		}
		for (unsigned int mode = 0; mode < 3; mode++) {
			select_address(0x080a);
			command(0xa5);
			data(modes[mode]);
			invoke(0xc149);
			expect(xram[0x1808], outputs[mode], "manual PWM 1808");
			expect(xram[0x1807], outputs[mode], "manual PWM 1807");
			select_address(0x080a);
			command(0xa5);
			data(0);
			invoke(0xc149);
			expect(xram[0x1808], automatic, "automatic control restored");
			expect(xram[0x1807], outputs[mode],
			       "secondary PWM remains manual after restoring mode");
		}
	}
	xram[0x470] = 50;
	invoke(0xc149);
	expect(xram[0x1808], 56, "automatic output at temperature 50");
	xram[0x80a] = 0x80;
	invoke(0xc149);
	expect(xram[0x1808], 0, "80 stops fan; never expose this mode");
}

static int firmware_read(unsigned short address, unsigned char *value)
{
	select_address(address);
	command(0xa4);
	*value = xram[0x1501];
	return 0;
}

static int firmware_write(unsigned short address, unsigned char value)
{
	select_address(address);
	command(0xa5);
	data(value);
	invoke(0xc149);
	return 0;
}

static void test_driver_restoration(void)
{
	static const struct u820_io io = {firmware_read, firmware_write};
	static const unsigned int outputs[] = {0, 95, 127};
	struct u820_boost boost = {0};

	xram[0xb0] = 1;
	for (unsigned int temperature = 30; temperature <= 75; temperature++) {
		for (unsigned int level = 1; level <= 2; level++) {
			xram[0x80a] = 0;
			xram[0x1807] = 0;
			xram[0x470] = temperature;
			invoke(0xc149);
			expect(u820_boost_start(&boost, &io, level), 0, "driver start through firmware");
			expect(xram[0x1808], outputs[level], "driver changed primary output");
			expect(xram[0x1807], outputs[level], "firmware changed secondary output");
			xram[0x470] = temperature + 1;
			expect(u820_boost_restore(&boost, &io), 0, "driver restore through firmware");
			expect(xram[0x1808], u820_auto_output(temperature + 1), "restored changing automatic curve");
			expect(xram[0x1807], 0, "secondary output completely restored");
		}
	}
}

static void test_identity_and_temperature(void)
{
	invoke(0xe4c7);
	expect(xram[0x0400], 1, "EC major version");
	expect(xram[0x0401], 5, "V1.06 file reports minor 05");
	expect(memcmp(xram + 0x0422, "12:21:32", 8), 0, "build time");
	expect(memcmp(xram + 0x0431, "2021/01/20", 10), 0, "build date");
	xram[0xb0] = 1;
	xram[0x470] = 40;
	xram[0x462] = 50;
	invoke(0xced3);
	expect(xram[0x471], 50, "temperature 71 is maximum of 62 and 70");
	xram[0x470] = 60;
	invoke(0xced3);
	expect(xram[0x471], 60, "temperature 71 follows higher CPU input");
}

static void test_manual_duty(void)
{
	static const struct u820_io io = {firmware_read, firmware_write};
	struct u820_manual manual = {0};

	xram[0xb0] = 1;
	xram[0x1807] = 0;
	for (unsigned int temperature = 20; temperature < 90; temperature++) {
		xram[0x470] = temperature;
		xram[0x80a] = 0;
		invoke(0xc149);
		for (unsigned int duty = U820_DUTY_MIN; duty <= U820_DUTY_MAX; duty++) {
			expect(u820_manual_set(&manual, &io, duty), 0, "manual duty transaction");
			for (unsigned int cycle = 0; cycle < 3; cycle++)
				invoke(0xc149);
			expect(xram[0x1808], duty, "84 preserves host duty across fan cycles");
			expect(xram[0x1807], 0, "manual duty does not touch secondary PWM");
		}
		expect(u820_manual_restore(&manual, &io), 0, "manual restores auto");
		expect(xram[0x1808], u820_auto_output(temperature), "auto resumes after manual");
	}
}

static void test_tach(void)
{
	static const unsigned int periods[] = {0, 500, 1000, 2000, 6999, 7000, 65535};
	for (unsigned int i = 0; i < sizeof(periods) / sizeof(periods[0]); i++) {
		unsigned int period = periods[i];
		unsigned int wanted = period == 0 ? 0 : period >= 7000 ? 7000 : 0x20e6da / period;
		xram[0x181e] = period & 255;
		xram[0x181f] = period >> 8;
		/* Stop after the result store, before unrelated banked maintenance. */
		invoke_until(0xcfd9, 0xcfee);
		expect((xram[0x080b] << 8) | xram[0x080c], wanted, "tach conversion and byte order");
		printf("tach period %u -> stored count %u\n", period, wanted);
	}
}

int main(int argc, char **argv)
{
	FILE *input;
	if (argc != 2) {
		fprintf(stderr, "Usage: firmware_harness CB9EC_V1.06.BIN\n");
		return 2;
	}
	input = fopen(argv[1], "rb");
	if (!input) {
		perror("open firmware");
		return 2;
	}
	if (fread(code, 1, sizeof(code), input) != sizeof(code)) {
		fprintf(stderr, "Firmware must contain at least 64 KiB\n");
		fclose(input);
		return 2;
	}
	fclose(input);
	cpu.mCodeMem = code;
	cpu.mCodeMemMaxIdx = 65535;
	cpu.mExtData = xram;
	cpu.mExtDataMaxIdx = 65535;
	cpu.mUpperData = upper;
	cpu.except = exception;
	reset(&cpu, false);
	test_protocol();
	test_fan();
	test_tach();
	test_identity_and_temperature();
	test_driver_restoration();
	test_manual_duty();
	printf("PASS: %u assertions against actual firmware routines, offline only.\n", checks);
	return 0;
}
