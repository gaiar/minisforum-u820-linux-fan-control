// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* CB9 EC telemetry and opt-in bounded boost; see docs/ec_firmware.md. */
#include <linux/acpi.h>
#include <linux/device.h>
#include <linux/dmi.h>
#include <linux/err.h>
#include <linux/hwmon.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/reboot.h>
#include <linux/suspend.h>
#include <linux/workqueue.h>

#include "u820_control.h"

static bool probe_extended;
module_param(probe_extended, bool, 0400);
MODULE_PARM_DESC(probe_extended, "Explicitly enable the experimental A2/A3/A4 read protocol");
static bool allow_boost;
module_param(allow_boost, bool, 0400);
MODULE_PARM_DESC(allow_boost, "Opt in to guarded five-second fan boost presets");
static bool allow_manual;
module_param(allow_manual, bool, 0400);
MODULE_PARM_DESC(allow_manual, "Opt in to guarded raw duty with a three-second lease");

static DEFINE_MUTEX(ec_lock);
static struct device *parent_device;
static struct device *hwmon_device;
static struct u820_boost boost_state;
static struct u820_manual manual_state;
static struct delayed_work boost_work;
static unsigned long boost_deadline;
static bool boost_reference;
static bool stopping;
static bool control_compatible;

/* All callers hold ec_lock across pointer selection and the response. */
static int xram_read(u16 address, u8 *value)
{
	u8 low = address;
	u8 high = address >> 8;
	int error;

	/* These commands select the read pointer; A5 (memory write) is never sent. */
	error = ec_transaction(0xa2, &low, 1, NULL, 0);
	if (error)
		return error;
	error = ec_transaction(0xa3, &high, 1, NULL, 0);
	if (error)
		return error;
	return ec_transaction(0xa4, NULL, 0, value, 1);
}

static int xram_write_limited(unsigned short address, unsigned char value)
{
	u8 low = address, high = address >> 8;
	int error;

	if ((!allow_boost && !allow_manual) || !control_compatible)
		return -EPERM;
	if (!((address == 0x080a && (value == 0 ||
	       (allow_boost && (value == 0x81 || value == 0x82)) ||
	       (allow_manual && value == U820_MANUAL_MODE))) ||
	      (allow_boost && address == 0x1807 && value == 0) ||
	      (allow_manual && address == 0x1808 &&
	       value >= U820_DUTY_MIN && value <= U820_DUTY_MAX)))
		return -EPERM;
	error = ec_transaction(0xa2, &low, 1, NULL, 0);
	if (error)
		return error;
	error = ec_transaction(0xa3, &high, 1, NULL, 0);
	if (error)
		return error;
	return ec_transaction(0xa5, &value, 1, NULL, 0);
}

static const struct u820_io control_io = {
	.read = xram_read,
	.write = xram_write_limited,
};

/* Keep the module pinned until every affected register has been restored. */
static int restore_locked(void)
{
	bool was_active = boost_state.active || manual_state.active;
	int error = u820_boost_restore(&boost_state, &control_io);

	if (!error)
		error = u820_manual_restore(&manual_state, &control_io);

	if (error) {
		pr_err_ratelimited("u820_ec_probe: automatic restore failed (%d); retrying\n", error);
		return error;
	}
	if (was_active) {
		pr_info("u820_ec_probe: automatic cooling restored\n");
		if (boost_reference) {
			boost_reference = false;
			module_put(THIS_MODULE);
		}
	}
	return 0;
}

static int check_firmware(void)
{
	static const u8 identity_offsets[] = {
		0x00, 0x01, 0x02, 0x03,
		0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29,
		0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a,
	};
	u8 identity[ARRAY_SIZE(identity_offsets)];
	u8 extended;
	int error, index;

	/* Check standard reads before trying any vendor command. */
	for (index = 0; index < ARRAY_SIZE(identity_offsets); index++) {
		error = ec_read(identity_offsets[index], &identity[index]);
		if (error)
			return error;
	}
	if (memcmp(identity, "\x01\x05\x01\x00", 4) ||
	    memcmp(identity + 12, "2021/01/20", 10) ||
	    (memcmp(identity + 4, "11:28:11", 8) &&
	     memcmp(identity + 4, "12:21:32", 8)))
		return -ENODEV;

	for (index = 0; index < ARRAY_SIZE(identity_offsets); index++) {
		error = xram_read(0x0400 + identity_offsets[index], &extended);
		if (error)
			return error;
		if (extended != identity[index]) {
			pr_err("u820_ec_probe: identity mismatch at 0x%04x: ACPI=%02x extended=%02x\n",
			       0x0400 + identity_offsets[index], identity[index], extended);
			return -EPROTO;
		}
	}
	pr_info("u820_ec_probe: standard/extended identity agrees; EC %.8s %.10s\n",
		identity + 4, identity + 12);
	/* Only the build whose live telemetry was cross-checked is writable. */
	control_compatible = !memcmp(identity + 4, "11:28:11", 8);
	return 0;
}

static int tach_read(u16 address, long *value)
{
	u8 high, low, repeated_high;
	int attempt, error;

	for (attempt = 0; attempt < 3; attempt++) {
		error = xram_read(address, &high);
		if (error)
			return error;
		error = xram_read(address + 1, &low);
		if (error)
			return error;
		error = xram_read(address, &repeated_high);
		if (error)
			return error;
		if (high == repeated_high) {
			*value = ((unsigned int)high << 8) | low;
			return *value <= 7000 ? 0 : -ERANGE;
		}
	}
	return -EAGAIN;
}

static void boost_watchdog(struct work_struct *work)
{
	u8 temperature, state, mode, primary, secondary;
	long rpm;
	bool restore;
	int error = 0;

	mutex_lock(&ec_lock);
	if (!boost_state.active && !manual_state.active)
		goto out;
	restore = stopping || time_after_eq(jiffies, boost_deadline);
	if (!restore) {
		error = xram_read(0x0470, &temperature);
		if (!error)
			error = xram_read(0x00b0, &state);
		if (!error)
			error = xram_read(0x080a, &mode);
		if (!error)
			error = xram_read(0x1808, &primary);
		if (!error)
			error = xram_read(0x1807, &secondary);
		if (!error)
			error = tach_read(0x080b, &rpm);
		/* Allow one second for the EC's periodic fan routine to run. */
		if (manual_state.active)
			restore = error || u820_manual_invalid(manual_state.duty,
				temperature, state, mode, primary, secondary, rpm);
		else
			restore = error || u820_boost_invalid(boost_state.level, temperature,
				state, mode, primary, secondary, rpm,
				time_after_eq(jiffies, boost_deadline - 4 * HZ));
	}
	if (restore) {
		pr_info("u820_ec_probe: cooling lease ended or guard tripped (I/O=%d)\n", error);
		restore_locked();
	}
	if (boost_state.active || manual_state.active)
		schedule_delayed_work(&boost_work, msecs_to_jiffies(500));
out:
	mutex_unlock(&ec_lock);
}

static ssize_t fan_boost_show(struct device *dev, struct device_attribute *attr,
			     char *buffer)
{
	unsigned int level;

	mutex_lock(&ec_lock);
	level = boost_state.level;
	mutex_unlock(&ec_lock);
	return sysfs_emit(buffer, "%u\n", level);
}

static ssize_t fan_boost_store(struct device *dev, struct device_attribute *attr,
			      const char *buffer, size_t count)
{
	unsigned int level;
	int error;

	if (!allow_boost || !control_compatible)
		return -EPERM;
	error = kstrtouint(buffer, 10, &level);
	if (error || level > 2)
		return -EINVAL;
	mutex_lock(&ec_lock);
	if (!level) {
		error = restore_locked();
	} else if (stopping || boost_state.active || manual_state.active) {
		error = -EBUSY;
	} else if (!try_module_get(THIS_MODULE)) {
		error = -ENODEV;
	} else {
		boost_deadline = jiffies + 5 * HZ;
		error = u820_boost_start(&boost_state, &control_io, level);
		if (boost_state.active) {
			boost_reference = true;
			if (error)
				restore_locked();
			else
				pr_info("u820_ec_probe: boost %u started; expires in five seconds\n", level);
		} else {
			module_put(THIS_MODULE);
		}
	}
	if (boost_state.active)
		mod_delayed_work(system_wq, &boost_work, msecs_to_jiffies(500));
	mutex_unlock(&ec_lock);
	return error ? error : count;
}
static DEVICE_ATTR(fan_boost, 0600, fan_boost_show, fan_boost_store);

/* Zero releases the lease. Every nonzero write renews it for three seconds. */
static ssize_t fan_pwm_raw_show(struct device *dev, struct device_attribute *attr,
			       char *buffer)
{
	unsigned int duty;

	mutex_lock(&ec_lock);
	duty = manual_state.active ? manual_state.duty : 0;
	mutex_unlock(&ec_lock);
	return sysfs_emit(buffer, "%u\n", duty);
}

static ssize_t fan_pwm_raw_store(struct device *dev, struct device_attribute *attr,
				const char *buffer, size_t count)
{
	unsigned int duty;
	bool newly_pinned = false;
	int error;

	if (!allow_manual || !control_compatible)
		return -EPERM;
	error = kstrtouint(buffer, 10, &duty);
	if (error || (duty && (duty < U820_DUTY_MIN || duty > U820_DUTY_MAX)))
		return -EINVAL;
	mutex_lock(&ec_lock);
	if (!duty) {
		error = restore_locked();
	} else if (stopping || boost_state.active) {
		error = -EBUSY;
	} else {
		if (!boost_reference) {
			if (!try_module_get(THIS_MODULE)) {
				error = -ENODEV;
				goto out;
			}
			boost_reference = newly_pinned = true;
		}
		error = u820_manual_set(&manual_state, &control_io, duty);
		if (error && manual_state.active)
			restore_locked();
		if (newly_pinned && !manual_state.active && boost_reference) {
			boost_reference = false;
			module_put(THIS_MODULE);
		}
		if (!error)
			boost_deadline = jiffies + 3 * HZ;
	}
	/* Do not defer an already scheduled watchdog when a heartbeat arrives. */
	if (manual_state.active || boost_state.active)
		schedule_delayed_work(&boost_work, msecs_to_jiffies(500));
out:
	mutex_unlock(&ec_lock);
	return error ? error : count;
}
static DEVICE_ATTR(fan_pwm_raw, 0600, fan_pwm_raw_show, fan_pwm_raw_store);

static int prepare_stop(void)
{
	int error;

	mutex_lock(&ec_lock);
	stopping = true;
	error = restore_locked();
	if (boost_state.active || manual_state.active)
		mod_delayed_work(system_wq, &boost_work, msecs_to_jiffies(500));
	mutex_unlock(&ec_lock);
	return error;
}

static int reboot_notify(struct notifier_block *block, unsigned long event, void *unused)
{
	prepare_stop();
	return NOTIFY_DONE;
}

static int pm_notify(struct notifier_block *block, unsigned long event, void *unused)
{
	switch (event) {
	case PM_SUSPEND_PREPARE:
	case PM_HIBERNATION_PREPARE:
	case PM_RESTORE_PREPARE:
		if (prepare_stop()) {
			mutex_lock(&ec_lock);
			stopping = false;
			mutex_unlock(&ec_lock);
			return NOTIFY_BAD;
		}
		break;
	case PM_POST_SUSPEND:
	case PM_POST_HIBERNATION:
	case PM_POST_RESTORE:
		mutex_lock(&ec_lock);
		stopping = false;
		mutex_unlock(&ec_lock);
		break;
	}
	return NOTIFY_OK;
}

static struct notifier_block reboot_notifier = { .notifier_call = reboot_notify };
static struct notifier_block pm_notifier = { .notifier_call = pm_notify };

static umode_t sensor_visible(const void *data, enum hwmon_sensor_types type,
			     u32 attr, int channel)
{
	if (type == hwmon_fan && channel == 0 &&
	    (attr == hwmon_fan_input || attr == hwmon_fan_label))
		return 0444;
	return 0;
}

static int sensor_read(struct device *dev, enum hwmon_sensor_types type,
		       u32 attr, int channel, long *value)
{
	int error;

	if (type != hwmon_fan || attr != hwmon_fan_input || channel != 0)
		return -EOPNOTSUPP;
	mutex_lock(&ec_lock);
	error = tach_read(0x080b, value);
	mutex_unlock(&ec_lock);
	return error;
}

static int sensor_label(struct device *dev, enum hwmon_sensor_types type,
			u32 attr, int channel, const char **label)
{
	if (type != hwmon_fan || attr != hwmon_fan_label || channel != 0)
		return -EOPNOTSUPP;
	*label = "EC tachometer 1 (experimental)";
	return 0;
}

static ssize_t registers_show(struct device *dev, struct device_attribute *attr,
			      char *buffer)
{
	static const u16 addresses[] = {
		0x00b0, 0x0462, 0x0470, 0x0471, 0x0472,
		0x080a, 0x080b, 0x080c, 0x083d, 0x083e, 0x1807, 0x1808,
		0x181f, 0x181e, 0x1821, 0x1820,
		0x1800, 0x1801, 0x180a, 0x180b, 0x180c, 0x180d,
		0x180e, 0x180f, 0x1841, 0x1842, 0x1843,
	};
	u8 values[ARRAY_SIZE(addresses)];
	int error = 0, index, length = 0;

	mutex_lock(&ec_lock);
	for (index = 0; index < ARRAY_SIZE(addresses); index++) {
		error = xram_read(addresses[index], &values[index]);
		if (error)
			break;
	}
	mutex_unlock(&ec_lock);
	if (error)
		return error;
	for (index = 0; index < ARRAY_SIZE(addresses); index++)
		length += sysfs_emit_at(buffer, length, "0x%04x 0x%02x\n",
				       addresses[index], values[index]);
	return length;
}
static DEVICE_ATTR_RO(registers);

static struct attribute *extra_attrs[] = {
	&dev_attr_registers.attr,
	&dev_attr_fan_boost.attr,
	&dev_attr_fan_pwm_raw.attr,
	NULL,
};

static umode_t extra_visible(struct kobject *object, struct attribute *attr, int index)
{
	if (attr == &dev_attr_fan_boost.attr && (!allow_boost || !control_compatible))
		return 0;
	if (attr == &dev_attr_fan_pwm_raw.attr && (!allow_manual || !control_compatible))
		return 0;
	return attr->mode;
}

static const struct attribute_group extra_group = {
	.attrs = extra_attrs,
	.is_visible = extra_visible,
};
static const struct attribute_group *extra_groups[] = { &extra_group, NULL };

static const struct hwmon_ops sensor_ops = {
	.is_visible = sensor_visible,
	.read = sensor_read,
	.read_string = sensor_label,
};

static const struct hwmon_channel_info * const sensor_info[] = {
	HWMON_CHANNEL_INFO(fan, HWMON_F_INPUT | HWMON_F_LABEL),
	NULL,
};

static const struct hwmon_chip_info sensor_chip = {
	.ops = &sensor_ops,
	.info = sensor_info,
};

static int __init u820_probe_init(void)
{
	int error;

	INIT_DELAYED_WORK(&boost_work, boost_watchdog);
	if (!probe_extended) {
		pr_err("u820_ec_probe: requires explicit probe_extended=1\n");
		return -EINVAL;
	}
	if (!dmi_match(DMI_PRODUCT_NAME, "U820") ||
	    !dmi_match(DMI_BOARD_NAME, "CB9") ||
	    !dmi_match(DMI_BIOS_VERSION, "CB9_FV102"))
		return -ENODEV;
	mutex_lock(&ec_lock);
	error = check_firmware();
	mutex_unlock(&ec_lock);
	if (error) {
		pr_err("u820_ec_probe: identity/protocol check failed: %d\n", error);
		return error;
	}
	if ((allow_boost || allow_manual) && !control_compatible)
		return -ENODEV;
	parent_device = root_device_register("u820_ec_probe");
	if (IS_ERR(parent_device))
		return PTR_ERR(parent_device);
	error = register_reboot_notifier(&reboot_notifier);
	if (error)
		goto unregister_parent;
	error = register_pm_notifier(&pm_notifier);
	if (error)
		goto unregister_reboot;
	hwmon_device = hwmon_device_register_with_info(parent_device,
			"u820_ec_probe", NULL, &sensor_chip, extra_groups);
	if (IS_ERR(hwmon_device)) {
		error = PTR_ERR(hwmon_device);
		goto unregister_pm;
	}
	pr_info("u820_ec_probe: telemetry ready; bounded boost %s\n",
		allow_boost ? "enabled" : "disabled");
	return 0;
unregister_pm:
	unregister_pm_notifier(&pm_notifier);
unregister_reboot:
	unregister_reboot_notifier(&reboot_notifier);
unregister_parent:
	root_device_unregister(parent_device);
	return error;
}

static void __exit u820_probe_exit(void)
{
	prepare_stop();
	hwmon_device_unregister(hwmon_device);
	unregister_pm_notifier(&pm_notifier);
	unregister_reboot_notifier(&reboot_notifier);
	cancel_delayed_work_sync(&boost_work);
	root_device_unregister(parent_device);
}

module_init(u820_probe_init);
module_exit(u820_probe_exit);
MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("Minisforum U820 CB9 EC telemetry and bounded fan boost");
MODULE_VERSION("0.2.0");
