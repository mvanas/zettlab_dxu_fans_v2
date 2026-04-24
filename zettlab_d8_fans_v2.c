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
 */
#include <linux/device.h>
#include <linux/err.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>

#define DRIVER_NAME     "zettlab_dxu_fans"
#define ZETTLAB_NR_FANS 3

/*
 * Board-specific MMIO window for the Zettlab D6U/D8U Ultra fan controller.
 * This driver intentionally keeps the same fixed address model as v1.
 */
#define FAN_BASE_ADDR   0xFE0B0456
#define FAN_REG_SIZE    0x10

#define PWM_MAX_HW      0xB7
#define PWM_MODE_MANUAL 1
#define PWM_MODE_AUTO   2

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
	struct mutex lock;
	u8 pwm_enable[ZETTLAB_NR_FANS];
};

static int zettlab_valid_channel(int channel)
{
	return (channel >= 0 && channel < ZETTLAB_NR_FANS) ? 0 : -EINVAL;
}

static u16 zettlab_read_rpm(struct zettlab_data *data, int channel)
{
	const struct zettlab_fan_desc *fan = &zettlab_fans[channel];
	u8 hi = ioread8(data->base + fan->rpm_offset);
	u8 lo = ioread8(data->base + fan->rpm_offset + 1);

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

static int zettlab_read_string(struct device *dev, enum hwmon_sensor_types type,
				u32 attr, int channel, const char **str)
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

static int zettlab_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct zettlab_data *data;
	struct device *hwmon_dev;
	int i;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	mutex_init(&data->lock);
	for (i = 0; i < ZETTLAB_NR_FANS; i++)
		data->pwm_enable[i] = PWM_MODE_MANUAL;

	data->base = devm_ioremap(dev, FAN_BASE_ADDR, FAN_REG_SIZE);
	if (!data->base)
		return dev_err_probe(dev, -ENOMEM,
				     "failed to map fan controller MMIO at 0x%lx\n",
				     (unsigned long)FAN_BASE_ADDR);

	hwmon_dev = devm_hwmon_device_register_with_info(dev, DRIVER_NAME, data,
							  &zettlab_chip_info,
							  NULL);
	if (IS_ERR(hwmon_dev))
		return dev_err_probe(dev, PTR_ERR(hwmon_dev),
				     "failed to register hwmon device\n");

	platform_set_drvdata(pdev, data);
	return 0;
}

static struct platform_driver zettlab_driver = {
	.probe = zettlab_probe,
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

	zettlab_pdev = platform_device_register_simple(DRIVER_NAME, -1, NULL, 0);
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
MODULE_DESCRIPTION("Zettlab D6U/D8U Ultra hwmon fan driver");

module_init(zettlab_init);
module_exit(zettlab_exit);
