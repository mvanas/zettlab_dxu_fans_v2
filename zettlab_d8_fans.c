// SPDX-License-Identifier: GPL-2.0
/*
 * Zettlab D6U/D8U Ultra hwmon fan driver
 *
 * Original concept and first implementation:
 *   Dean Holland <speedster@haveacry.com>
 *   https://github.com/Haveacry/zettlab-d8-fans
 *
 * v2 rewrite:
 *   Marcel van As
 *
 * Exposes the EC fan controller (3 fans: RPM read, PWM write) via
 * hwmon. Includes a watchdog that forces a safe PWM value if no
 * userspace controller (e.g. fan-control.sh) has written a PWM value
 * within watchdog_timeout seconds, and applies the safe PWM at load
 * time so the fans never sit unsupervised while userspace starts up.
 */
#include <linux/device.h>
#include <linux/err.h>
#include <linux/hwmon.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/workqueue.h>

#define DRIVER_NAME     "zettlab_d8_fans"
#define ZETTLAB_NR_FANS 3

/*
 * MMIO window of the Zettlab D6U/D8U Ultra fan controller.
 * Overridable via the fan_base_addr module parameter in case a board
 * revision moves the EC window.
 */
#define FAN_BASE_ADDR_DEFAULT    0xFE0B0456UL
#define FAN_REG_SIZE            0x10

#define PWM_MAX_HW              0xB7
#define PWM_MODE_MANUAL         1
#define PWM_MODE_AUTO           2

#define WATCHDOG_INTERVAL_SECS   10
#define WATCHDOG_TIMEOUT_DEFAULT 60

static ulong fan_base_addr = FAN_BASE_ADDR_DEFAULT;
module_param(fan_base_addr, ulong, 0444);
MODULE_PARM_DESC(fan_base_addr, "MMIO base address of the fan controller");

static uint watchdog_timeout = WATCHDOG_TIMEOUT_DEFAULT;
module_param(watchdog_timeout, uint, 0644);
MODULE_PARM_DESC(watchdog_timeout,
		 "seconds without a userspace PWM write before the safe PWM is forced (0 = disabled)");

static uint safe_pwm = PWM_MAX_HW;
module_param(safe_pwm, uint, 0644);
MODULE_PARM_DESC(safe_pwm, "PWM value forced at load time and by the watchdog");

struct zettlab_fan_desc {
	const char *label;
	u8 rpm_offset;
	u8 pwm_offset;
	bool auto_supported;
};

static const struct zettlab_fan_desc zettlab_fans[ZETTLAB_NR_FANS] = {
	{ .label = "Disks 1", .rpm_offset = 0, .pwm_offset = 2, .auto_supported = false },
	{ .label = "Disks 2", .rpm_offset = 3, .pwm_offset = 5, .auto_supported = false },
	{ .label = "CPU",     .rpm_offset = 6, .pwm_offset = 8, .auto_supported = true  },
};

struct zettlab_data {
	void __iomem *base;
	struct device *dev;
	struct mutex lock;
	u8 pwm_enable[ZETTLAB_NR_FANS];
	unsigned long last_write;
	bool watchdog_fired;
	bool watchdog_stopped;
	struct delayed_work watchdog_work;
};

static int zettlab_valid_channel(int channel)
{
	return (channel >= 0 && channel < ZETTLAB_NR_FANS) ? 0 : -EINVAL;
}

static u8 zettlab_safe_pwm(void)
{
	return safe_pwm > PWM_MAX_HW ? PWM_MAX_HW : (u8)safe_pwm;
}

static u16 zettlab_read_rpm(struct zettlab_data *data, int channel)
{
	const struct zettlab_fan_desc *fan = &zettlab_fans[channel];
	u8 hi, lo, hi2, lo2;

	/*
	 * Read the register pair twice: the EC may update the value
	 * between the two byte accesses, which would tear the 16-bit
	 * RPM reading.
	 */
	hi = ioread8(data->base + fan->rpm_offset);
	lo = ioread8(data->base + fan->rpm_offset + 1);
	hi2 = ioread8(data->base + fan->rpm_offset);
	lo2 = ioread8(data->base + fan->rpm_offset + 1);

	if (hi != hi2 || lo != lo2) {
		hi = hi2;
		lo = lo2;
	}

	return ((u16)hi << 8) | lo;
}

static u8 zettlab_read_pwm(struct zettlab_data *data, int channel)
{
	return ioread8(data->base + zettlab_fans[channel].pwm_offset);
}

static void zettlab_write_pwm(struct zettlab_data *data, int channel, u8 val)
{
	iowrite8(val, data->base + zettlab_fans[channel].pwm_offset);
}

static void zettlab_force_safe_pwm(struct zettlab_data *data)
{
	int i;

	for (i = 0; i < ZETTLAB_NR_FANS; i++)
		zettlab_write_pwm(data, i, zettlab_safe_pwm());
}

/* ================= Watchdog ================= */

/*
 * Periodically verifies that userspace is still controlling the fans.
 * If no PWM write has occurred within watchdog_timeout seconds, all
 * fans are forced to safe_pwm until userspace control resumes. The
 * first write from userspace resets the watchdog.
 */
static void zettlab_watchdog_work(struct work_struct *work)
{
	struct zettlab_data *data = container_of(to_delayed_work(work),
						 struct zettlab_data,
						 watchdog_work);
	bool fired = false;

	if (watchdog_timeout) {
		mutex_lock(&data->lock);
		if (time_after(jiffies,
				       data->last_write +
				       (unsigned long)watchdog_timeout * HZ)) {
			fired = !data->watchdog_fired;
			data->watchdog_fired = true;
			zettlab_force_safe_pwm(data);
		}
		mutex_unlock(&data->lock);

		if (fired)
			dev_info(data->dev,
				 "no userspace PWM writes for %u s; forcing safe PWM %u until userspace control resumes\n",
				 watchdog_timeout, zettlab_safe_pwm());
	}

	mutex_lock(&data->lock);
	if (!data->watchdog_stopped)
		schedule_delayed_work(&data->watchdog_work,
				      WATCHDOG_INTERVAL_SECS * HZ);
	mutex_unlock(&data->lock);
}

/* ================= Visibility ================= */

static umode_t zettlab_is_visible(const void *drvdata,
				  enum hwmon_sensor_types type,
				  u32 attr, int channel)
{
	if (zettlab_valid_channel(channel))
		return 0;

	switch (type) {
	case hwmon_fan:
		switch (attr) {
		case hwmon_fan_input:
		case hwmon_fan_label:
			return 0444;
		default:
			return 0;
		}

	case hwmon_pwm:
		switch (attr) {
		case hwmon_pwm_input:
			return 0644;
		case hwmon_pwm_enable:
			return zettlab_fans[channel].auto_supported ? 0644 : 0444;
		default:
			return 0;
		}

	default:
		return 0;
	}
}

/* ================= Read ================= */

static int zettlab_read(struct device *dev, enum hwmon_sensor_types type,
			u32 attr, int channel, long *val)
{
	struct zettlab_data *data = dev_get_drvdata(dev);
	int ret;

	ret = zettlab_valid_channel(channel);
	if (ret)
		return ret;

	mutex_lock(&data->lock);

	switch (type) {
	case hwmon_fan:
		if (attr != hwmon_fan_input) {
			ret = -EOPNOTSUPP;
			break;
		}
		*val = zettlab_read_rpm(data, channel);
		ret = 0;
		break;

	case hwmon_pwm:
		switch (attr) {
		case hwmon_pwm_input:
			*val = zettlab_read_pwm(data, channel);
			ret = 0;
			break;
		case hwmon_pwm_enable:
			*val = data->pwm_enable[channel];
		ret = 0;
			break;
		default:
			ret = -EOPNOTSUPP;
			break;
		}
		break;

	default:
		ret = -EOPNOTSUPP;
		break;
	}

	mutex_unlock(&data->lock);
	return ret;
}

/* ================= Write ================= */

static int zettlab_write(struct device *dev, enum hwmon_sensor_types type,
			 u32 attr, int channel, long val)
{
	struct zettlab_data *data = dev_get_drvdata(dev);
	int ret;

	ret = zettlab_valid_channel(channel);
	if (ret)
		return ret;

	if (type != hwmon_pwm)
		return -EOPNOTSUPP;

	mutex_lock(&data->lock);

	switch (attr) {
	case hwmon_pwm_input:
		if (data->pwm_enable[channel] != PWM_MODE_MANUAL) {
			ret = -EOPNOTSUPP;
			break;
		}
		if (val < 0 || val > PWM_MAX_HW) {
			ret = -EINVAL;
			break;
		}
		zettlab_write_pwm(data, channel, (u8)val);

		/* Heartbeat for the watchdog */
		data->last_write = jiffies;
		data->watchdog_fired = false;
		ret = 0;
		break;

	case hwmon_pwm_enable:
		switch (val) {
		case PWM_MODE_MANUAL:
			data->pwm_enable[channel] = PWM_MODE_MANUAL;
			ret = 0;
			break;
		case PWM_MODE_AUTO:
			if (!zettlab_fans[channel].auto_supported) {
				ret = -EOPNOTSUPP;
				break;
			}
			/*
			 * Auto mode: PWM 0 hands the fan back to the EC
			 * firmware (validated on D6U/D8U hardware).
			 */
			data->pwm_enable[channel] = PWM_MODE_AUTO;
			zettlab_write_pwm(data, channel, 0);
			ret = 0;
			break;
		default:
			ret = -EINVAL;
			break;
		}
		break;

	default:
		ret = -EOPNOTSUPP;
		break;
	}

	mutex_unlock(&data->lock);
	return ret;
}

/* ================= Labels ================= */

static int zettlab_read_string(struct device *dev,
			       enum hwmon_sensor_types type,
			       u32 attr, int channel,
			       const char **str)
{
	int ret;

	ret = zettlab_valid_channel(channel);
	if (ret)
		return ret;

	if (type != hwmon_fan || attr != hwmon_fan_label)
		return -EOPNOTSUPP;

	*str = zettlab_fans[channel].label;
	return 0;
}

/* ================= Channel config ================= */

static const struct hwmon_channel_info *zettlab_info[] = {
	HWMON_CHANNEL_INFO(fan,
		HWMON_F_INPUT | HWMON_F_LABEL,
		HWMON_F_INPUT | HWMON_F_LABEL,
		HWMON_F_INPUT | HWMON_F_LABEL),
	HWMON_CHANNEL_INFO(pwm,
		HWMON_PWM_INPUT | HWMON_PWM_ENABLE,
		HWMON_PWM_INPUT | HWMON_PWM_ENABLE,
		HWMON_PWM_INPUT | HWMON_PWM_ENABLE),
	NULL
};

static const struct hwmon_ops zettlab_ops = {
	.is_visible = zettlab_is_visible,
	.read = zettlab_read,
	.write = zettlab_write,
	.read_string = zettlab_read_string,
};

static const struct hwmon_chip_info zettlab_chip_info = {
	.ops = &zettlab_ops,
	.info = zettlab_info,
};

/* ================= Driver ================= */

static int zettlab_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct zettlab_data *data;
	struct device *hwmon_dev;
	int i;

	if (!devm_request_mem_region(dev, fan_base_addr, FAN_REG_SIZE,
				     DRIVER_NAME)) {
		dev_err(dev, "MMIO window 0x%lx-0x%lx is not available\n",
			(unsigned long)fan_base_addr,
			(unsigned long)fan_base_addr + FAN_REG_SIZE - 1);
		return -EBUSY;
	}

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->dev = dev;
	mutex_init(&data->lock);
	for (i = 0; i < ZETTLAB_NR_FANS; i++)
		data->pwm_enable[i] = PWM_MODE_MANUAL;

	data->base = devm_ioremap(dev, fan_base_addr, FAN_REG_SIZE);
	if (!data->base)
		return dev_err_probe(dev, -ENOMEM,
				     "failed to map fan controller MMIO at 0x%lx\n",
				     (unsigned long)fan_base_addr);

	/*
	 * Safe default: force safe PWM at load time so the fans run at
	 * full cooling until userspace (fan-control.sh) takes over. The
	 * watchdog then keeps them there if userspace never shows up.
	 */
	mutex_lock(&data->lock);
	zettlab_force_safe_pwm(data);
	data->last_write = jiffies;
	mutex_unlock(&data->lock);
	dev_info(dev, "fans forced to safe PWM %u until userspace control\n",
		 zettlab_safe_pwm());

	hwmon_dev = devm_hwmon_device_register_with_info(dev, DRIVER_NAME, data,
							 &zettlab_chip_info,
							 NULL);
	if (IS_ERR(hwmon_dev))
		return dev_err_probe(dev, PTR_ERR(hwmon_dev),
				     "failed to register hwmon device\n");

	INIT_DELAYED_WORK(&data->watchdog_work, zettlab_watchdog_work);
	schedule_delayed_work(&data->watchdog_work,
			      WATCHDOG_INTERVAL_SECS * HZ);

	platform_set_drvdata(pdev, data);
	return 0;
}

static int zettlab_remove(struct platform_device *pdev)
{
	struct zettlab_data *data = platform_get_drvdata(pdev);

	/* Stop watchdog rescheduling before cancelling it. */
	mutex_lock(&data->lock);
	data->watchdog_stopped = true;
	mutex_unlock(&data->lock);

	cancel_delayed_work_sync(&data->watchdog_work);

	/* Without the watchdog the fans must not be left unsupervised. */
	mutex_lock(&data->lock);
	zettlab_force_safe_pwm(data);
	mutex_unlock(&data->lock);

	return 0;
}

static struct platform_driver zettlab_driver = {
	.probe = zettlab_probe,
	.remove = zettlab_remove,
	.driver = {
		.name = DRIVER_NAME,
	},
};

static struct platform_device *zettlab_pdev;

static int __init zettlab_init(void)
{
	int ret;

	ret = platform_driver_register(&zettlab_driver);
	if (ret)
		return ret;

	zettlab_pdev = platform_device_register_simple(DRIVER_NAME, -1,
						       NULL, 0);
	if (IS_ERR(zettlab_pdev)) {
		ret = PTR_ERR(zettlab_pdev);
		platform_driver_unregister(&zettlab_driver);
		return ret;
	}

	return 0;
}

static void __exit zettlab_exit(void)
{
	platform_device_unregister(zettlab_pdev);
	platform_driver_unregister(&zettlab_driver);
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Marcel van As");
MODULE_AUTHOR("Dean Holland (speedster@haveacry.com)");
MODULE_DESCRIPTION("Zettlab D6U/D8U Ultra hwmon fan driver with userspace watchdog");

module_init(zettlab_init);
module_exit(zettlab_exit);
