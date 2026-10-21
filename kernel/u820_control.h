/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef U820_CONTROL_H
#define U820_CONTROL_H

#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/types.h>
#else
#include <errno.h>
#include <stdbool.h>
#endif

struct u820_io {
	int (*read)(unsigned short address, unsigned char *value);
	int (*write)(unsigned short address, unsigned char value);
};

struct u820_boost {
	bool active;
	unsigned char level;
	unsigned char secondary;
};

/* Raw duty units; the tested electrical range deliberately excludes 0 and 200. */
#define U820_DUTY_MIN 46
#define U820_DUTY_MAX 127
#define U820_MANUAL_MODE 0x84

struct u820_manual {
	bool active;
	unsigned char duty;
};

static inline int u820_manual_restore(struct u820_manual *manual,
				      const struct u820_io *io)
{
	unsigned char mode;
	int error;

	if (!manual->active)
		return 0;
	error = io->write(0x080a, 0);
	if (error)
		return error;
	error = io->read(0x080a, &mode);
	if (error)
		return error;
	if (mode != 0)
		return -EPROTO;
	manual->active = false;
	manual->duty = 0;
	return 0;
}

/* Refreshing a lease still checks state and temperature before each write. */
static inline int u820_manual_set(struct u820_manual *manual,
				  const struct u820_io *io, unsigned int duty)
{
	static const unsigned short addresses[] = {0x00b0, 0x0470, 0x080a, 0x1807};
	unsigned char values[4], value;
	unsigned int index;
	int error;

	if (duty < U820_DUTY_MIN || duty > U820_DUTY_MAX)
		return -EINVAL;
	for (index = 0; index < 4; index++) {
		error = io->read(addresses[index], &values[index]);
		if (error)
			return error;
	}
	if (values[0] != 1 || values[3] != 0 ||
	    values[2] != (manual->active ? U820_MANUAL_MODE : 0))
		return -EBUSY;
	if (values[1] < 20 || values[1] >= 90)
		return -ERANGE;
	/* A partially completed write requires rollback, even on an error return. */
	manual->active = true;
	error = io->write(0x080a, U820_MANUAL_MODE);
	if (error)
		return error;
	error = io->read(0x080a, &value);
	if (error || value != U820_MANUAL_MODE)
		return error ? error : -EPROTO;
	error = io->write(0x1808, duty);
	if (error)
		return error;
	error = io->read(0x1808, &value);
	if (error || value != duty)
		return error ? error : -EPROTO;
	manual->duty = duty;
	return 0;
}

static inline bool u820_manual_invalid(unsigned int duty, unsigned int temperature,
		unsigned int state, unsigned int mode, unsigned int primary,
		unsigned int secondary, long rpm)
{
	return duty < U820_DUTY_MIN || duty > U820_DUTY_MAX ||
	       temperature < 20 || temperature >= 90 || state != 1 ||
	       mode != U820_MANUAL_MODE || primary != duty || secondary != 0 ||
	       rpm < 500 || rpm >= 7000;
}

static inline bool u820_boost_invalid(unsigned int level, unsigned int temperature,
		unsigned int state, unsigned int mode, unsigned int primary,
		unsigned int secondary, long rpm, bool settled)
{
	static const unsigned char outputs[] = {0, 95, 127};

	if (level < 1 || level > 2 || temperature > 75 || state != 1 ||
	    mode != (0x80 | level))
		return true;
	return settled && (primary != outputs[level] || secondary != outputs[level] ||
			   rpm <= 0 || rpm >= 7000);
}

static inline unsigned int u820_auto_output(unsigned int temperature)
{
	if (temperature < 30)
		return 0;
	if (temperature < 45)
		return 46;
	if (temperature < 55)
		return 56;
	if (temperature < 65)
		return 74;
	if (temperature < 85)
		return (50 * temperature + 4150) / 100;
	return 92;
}

/* Caller serializes all operations and guarantees a timed restore attempt. */
static inline int u820_boost_start(struct u820_boost *boost,
				   const struct u820_io *io, unsigned int level)
{
	static const unsigned short addresses[] = {
		0x00b0, 0x080a, 0x1807, 0x0470, 0x1808, 0x0470,
	};
	unsigned char values[6], mode;
	unsigned int index;
	int error;

	if (boost->active)
		return -EBUSY;
	if (level < 1 || level > 2)
		return -EINVAL;
	for (index = 0; index < 6; index++) {
		error = io->read(addresses[index], &values[index]);
		if (error)
			return error;
	}
	/* Only the observed automatic state and idle secondary output are supported. */
	if (values[0] != 1 || values[1] != 0 || values[2] != 0)
		return -EBUSY;
	if (values[3] < 30 || values[3] > 75)
		return -ERANGE;
	if (values[3] != values[5] || values[4] != u820_auto_output(values[3]))
		return -EAGAIN;
	boost->secondary = values[2];
	boost->level = level;
	/* A failed transaction may still have reached the EC: rollback remains due. */
	boost->active = true;
	error = io->write(0x080a, 0x80 | level);
	if (error)
		return error;
	error = io->read(0x080a, &mode);
	if (error)
		return error;
	return mode == (0x80 | level) ? 0 : -EPROTO;
}

static inline int u820_boost_restore(struct u820_boost *boost,
				     const struct u820_io *io)
{
	unsigned char value;
	int error;

	if (!boost->active)
		return 0;
	error = io->write(0x080a, 0);
	if (error)
		return error;
	error = io->read(0x080a, &value);
	if (error)
		return error;
	if (value != 0)
		return -EPROTO;
	/* Clear manual mode before restoring the second output it affected. */
	error = io->write(0x1807, boost->secondary);
	if (error)
		return error;
	error = io->read(0x1807, &value);
	if (error)
		return error;
	if (value != boost->secondary)
		return -EPROTO;
	boost->active = false;
	boost->level = 0;
	return 0;
}

#endif
