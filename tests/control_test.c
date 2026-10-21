/* Fault-injection tests of the same control code used by the kernel module. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../kernel/u820_control.h"

static unsigned char memory[65536];
static unsigned int operation, fail_at, corrupt_at, writes;
static bool apply_failed_write;

static int read_memory(unsigned short address, unsigned char *value)
{
	operation++;
	if (operation == fail_at)
		return -EIO;
	*value = memory[address];
	if (operation == corrupt_at)
		*value ^= 1;
	return 0;
}

static int write_memory(unsigned short address, unsigned char value)
{
	assert((address == 0x080a && (value == 0 || value == U820_MANUAL_MODE ||
		(value >= 0x81 && value <= 0x82))) ||
	       (address == 0x1807 && value == 0) ||
	       (address == 0x1808 && value >= U820_DUTY_MIN && value <= U820_DUTY_MAX));
	operation++;
	writes++;
	if (operation != fail_at || apply_failed_write)
		memory[address] = value;
	return operation == fail_at ? -EIO : 0;
}

static const struct u820_io io = {read_memory, write_memory};

static void prepare(struct u820_boost *boost)
{
	memset(memory, 0, sizeof(memory));
	memset(boost, 0, sizeof(*boost));
	operation = fail_at = corrupt_at = writes = 0;
	apply_failed_write = false;
	memory[0xb0] = 1;
	memory[0x470] = 40;
	memory[0x1808] = 46;
}

static void restore(struct u820_boost *boost)
{
	fail_at = corrupt_at = 0;
	assert(u820_boost_restore(boost, &io) == 0);
	assert(!boost->active);
	assert(memory[0x080a] == 0);
	assert(memory[0x1807] == 0);
}

static void test_manual(void)
{
	struct u820_boost unused;
	struct u820_manual manual;
	unsigned int scenarios = 0;

	for (unsigned int failure = 0; failure <= 8; failure++) {
		for (unsigned int partial = 0; partial <= 1; partial++) {
			prepare(&unused);
			memset(&manual, 0, sizeof(manual));
			fail_at = failure;
			apply_failed_write = partial;
			assert(u820_manual_set(&manual, &io, 56) == (failure ? -EIO : 0));
			assert(manual.active == (failure == 0 || failure >= 5));
			fail_at = 0;
			assert(!u820_manual_restore(&manual, &io));
			assert(!manual.active && !memory[0x80a] && !memory[0x1807]);
			scenarios++;
		}
	}
	for (unsigned int point = 6; point <= 8; point += 2) {
		prepare(&unused);
		memset(&manual, 0, sizeof(manual));
		corrupt_at = point;
		assert(u820_manual_set(&manual, &io, 56) == -EPROTO);
		corrupt_at = 0;
		assert(!u820_manual_restore(&manual, &io));
	}
	for (unsigned int failure = 1; failure <= 2; failure++) {
		prepare(&unused);
		memset(&manual, 0, sizeof(manual));
		assert(!u820_manual_set(&manual, &io, 56));
		operation = 0;
		fail_at = failure;
		assert(u820_manual_restore(&manual, &io) == -EIO);
		assert(manual.active);
		fail_at = 0;
		assert(!u820_manual_restore(&manual, &io));
	}
	prepare(&unused);
	memset(&manual, 0, sizeof(manual));
	assert(u820_manual_set(&manual, &io, 0) == -EINVAL);
	assert(u820_manual_set(&manual, &io, 45) == -EINVAL);
	assert(u820_manual_set(&manual, &io, 128) == -EINVAL);
	assert(!writes);
	assert(!u820_manual_set(&manual, &io, 46));
	assert(!u820_manual_set(&manual, &io, 127));
	assert(manual.duty == 127 && memory[0x1808] == 127);
	operation = 0;
	corrupt_at = 2;
	assert(u820_manual_restore(&manual, &io) == -EPROTO);
	corrupt_at = 0;
	assert(!u820_manual_restore(&manual, &io));
	for (unsigned int point = 0; point < 5; point++) {
		static const unsigned short addresses[] = {0xb0, 0x470, 0x80a, 0x1807, 0x470};
		static const unsigned char values[] = {3, 19, 0x81, 1, 90};
		prepare(&unused);
		memset(&manual, 0, sizeof(manual));
		memory[addresses[point]] = values[point];
		assert(u820_manual_set(&manual, &io, 56) < 0);
		assert(!manual.active && !writes);
	}
	assert(!u820_manual_invalid(46, 40, 1, 0x84, 46, 0, 1500));
	assert(u820_manual_invalid(45, 40, 1, 0x84, 45, 0, 1500));
	assert(u820_manual_invalid(128, 40, 1, 0x84, 128, 0, 1500));
	assert(u820_manual_invalid(46, 19, 1, 0x84, 46, 0, 1500));
	assert(u820_manual_invalid(46, 90, 1, 0x84, 46, 0, 1500));
	assert(u820_manual_invalid(46, 40, 3, 0x84, 46, 0, 1500));
	assert(u820_manual_invalid(46, 40, 1, 0, 46, 0, 1500));
	assert(u820_manual_invalid(46, 40, 1, 0x84, 56, 0, 1500));
	assert(u820_manual_invalid(46, 40, 1, 0x84, 46, 1, 1500));
	assert(u820_manual_invalid(46, 40, 1, 0x84, 46, 0, 0));
	assert(u820_manual_invalid(46, 40, 1, 0x84, 46, 0, 7000));
	printf("PASS: manual duty partial-write/rollback tests (%u start failures), renewals and guards.\n", scenarios);
}

int main(void)
{
	struct u820_boost boost;
	unsigned int scenarios = 0;
	for (unsigned int level = 1; level <= 2; level++) {
		for (unsigned int failure = 0; failure <= 8; failure++) {
			for (unsigned int partial = 0; partial <= 1; partial++) {
				prepare(&boost);
				fail_at = failure;
				apply_failed_write = partial;
				assert(u820_boost_start(&boost, &io, level) == (failure ? -EIO : 0));
				assert(boost.active == (failure == 0 || failure >= 7));
				restore(&boost);
				scenarios++;
			}
		}
		for (unsigned int failure = 1; failure <= 4; failure++) {
			for (unsigned int partial = 0; partial <= 1; partial++) {
				prepare(&boost);
				assert(u820_boost_start(&boost, &io, level) == 0);
				memory[0x1807] = 95;
				operation = 0;
				fail_at = failure;
				apply_failed_write = partial;
				assert(u820_boost_restore(&boost, &io) == -EIO);
				assert(boost.active);
				restore(&boost);
				scenarios++;
			}
		}
	}
	prepare(&boost);
	assert(u820_boost_start(&boost, &io, 0) == -EINVAL);
	assert(u820_boost_start(&boost, &io, 3) == -EINVAL);
	assert(u820_boost_start(&boost, &io, 4) == -EINVAL);
	for (unsigned int field = 0; field < 6; field++) {
		static const unsigned short addresses[] = {0xb0, 0x80a, 0x1807, 0x470, 0x1808, 0x470};
		static const unsigned char values[] = {3, 0x81, 1, 29, 95, 76};
		prepare(&boost);
		memory[addresses[field]] = values[field];
		assert(u820_boost_start(&boost, &io, 1) < 0);
		assert(!boost.active && !writes);
		scenarios++;
	}
	prepare(&boost);
	corrupt_at = 6;
	assert(u820_boost_start(&boost, &io, 1) == -EAGAIN);
	assert(!writes);
	prepare(&boost);
	corrupt_at = 8;
	assert(u820_boost_start(&boost, &io, 1) == -EPROTO);
	assert(boost.active);
	restore(&boost);
	for (unsigned int point = 2; point <= 4; point += 2) {
		prepare(&boost);
		assert(u820_boost_start(&boost, &io, 1) == 0);
		assert(u820_boost_start(&boost, &io, 2) == -EBUSY);
		operation = 0;
		corrupt_at = point;
		assert(u820_boost_restore(&boost, &io) == -EPROTO);
		assert(boost.active);
		restore(&boost);
	}
	assert(!u820_boost_invalid(1, 40, 1, 0x81, 95, 95, 2800, true));
	assert(!u820_boost_invalid(2, 40, 1, 0x82, 127, 127, 3500, true));
	assert(!u820_boost_invalid(1, 40, 1, 0x81, 46, 0, 0, false));
	assert(u820_boost_invalid(3, 40, 1, 0x83, 200, 200, 0, true));
	assert(u820_boost_invalid(0, 40, 1, 0, 46, 0, 1500, true));
	assert(u820_boost_invalid(1, 76, 1, 0x81, 95, 95, 2800, true));
	assert(u820_boost_invalid(1, 40, 3, 0x81, 95, 95, 2800, true));
	assert(u820_boost_invalid(1, 40, 1, 0x82, 95, 95, 2800, true));
	assert(u820_boost_invalid(1, 40, 1, 0x81, 46, 95, 2800, true));
	assert(u820_boost_invalid(1, 40, 1, 0x81, 95, 0, 2800, true));
	assert(u820_boost_invalid(1, 40, 1, 0x81, 95, 95, 0, true));
	assert(u820_boost_invalid(1, 40, 1, 0x81, 95, 95, 7000, true));
	printf("PASS: %u fault/state scenarios plus invalid, busy, and mismatched-read checks.\n", scenarios);
	test_manual();
	return 0;
}
