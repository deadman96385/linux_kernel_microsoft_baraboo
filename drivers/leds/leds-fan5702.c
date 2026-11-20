// SPDX-License-Identifier: GPL-2.0-only
/* FAN5702 status LEDs on the first-generation Microsoft HoloLens. */

#include <linux/acpi.h>
#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/leds.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pm.h>
#include <linux/slab.h>
#include <linux/string.h>

#define FAN5702_ADDRESS		0x36
#define FAN5702_GENERAL		0x10
#define FAN5702_CONFIG		0x20
#define FAN5702_CHA		0xa0
#define FAN5702_BRIGHTNESS_MASK	GENMASK(5, 0)
#define FAN5702_VENDOR_MASK	GENMASK(7, 6)
#define FAN5702_ENABLE_MASK	GENMASK(4, 0)

/* Stock selects 20 mA full scale, no PWM, and default output grouping. */
#define FAN5702_BOARD_CONFIG	0x00
/* LED class zero is off. Levels 1 through 40 select codes 0 through 0x27. */
#define FAN5702_MAX_BRIGHTNESS	40
#define FAN5702_NUM_CHANNELS	5

static const u8 fan5702_brightness_regs[FAN5702_NUM_CHANNELS] = {
	FAN5702_CHA, 0x30, 0x40, 0x50, 0x60,
};

static const char *const fan5702_led_names[FAN5702_NUM_CHANNELS] = {
	"hololens::status-1", "hololens::status-2", "hololens::status-3",
	"hololens::status-4", "hololens::status-5",
};

struct fan5702;

struct fan5702_led {
	struct led_classdev cdev;
	struct fan5702 *chip;
	unsigned int channel;
};

struct fan5702 {
	struct i2c_client *client;
	struct gpio_desc *enable_gpio;
	struct mutex lock;
	struct fan5702_led leds[FAN5702_NUM_CHANNELS];
	u8 levels[FAN5702_NUM_CHANNELS];
	bool powered;
	bool suspended;
};

static const struct acpi_gpio_params fan5702_enable_gpio = { 0, 0, false };
static const struct acpi_gpio_mapping fan5702_gpios[] = {
	{ "enable-gpios", &fan5702_enable_gpio, 1 },
	{ }
};

static int fan5702_power_off(struct fan5702 *chip)
{
	int ret;

	ret = gpiod_direction_output_raw(chip->enable_gpio, 0);
	if (!ret)
		chip->powered = false;

	return ret;
}

static void fan5702_disable(void *data)
{
	struct fan5702 *chip = data;
	int ret;

	ret = fan5702_power_off(chip);
	if (ret)
		dev_warn(&chip->client->dev, "failed to deassert enable: %d\n", ret);
}

static int fan5702_power_on(struct fan5702 *chip)
{
	int ret;

	/* The stock driver's GPIO request carries the physical pin level. */
	ret = gpiod_direction_output_raw(chip->enable_gpio, 1);
	if (ret)
		return ret;

	usleep_range(1000, 2000);
	return 0;
}

/* With lock held, program the brightness registers before the shared mask. */
static int fan5702_write_levels(struct fan5702 *chip, const u8 *levels)
{
	u8 enable = 0;
	unsigned int channel;
	int ret;

	for (channel = 0; channel < FAN5702_NUM_CHANNELS; channel++) {
		u8 level = levels[channel];

		if (level)
			enable |= BIT(channel);
		ret = i2c_smbus_write_byte_data(chip->client,
						fan5702_brightness_regs[channel],
						level ? level - 1 : 0);
		if (ret < 0)
			return ret;
	}

	return i2c_smbus_write_byte_data(chip->client, FAN5702_GENERAL, enable);
}

static int fan5702_restore(struct fan5702 *chip, const u8 *levels)
{
	int ret;

	ret = fan5702_power_on(chip);
	if (ret)
		return ret;

	ret = i2c_smbus_write_byte_data(chip->client, FAN5702_CONFIG,
					FAN5702_BOARD_CONFIG);
	if (!ret)
		ret = fan5702_write_levels(chip, levels);
	if (ret) {
		fan5702_disable(chip);
		return ret;
	}

	chip->powered = true;
	return 0;
}

static int fan5702_brightness_set(struct led_classdev *cdev,
				 enum led_brightness brightness)
{
	struct fan5702_led *led = container_of(cdev, struct fan5702_led, cdev);
	struct fan5702 *chip = led->chip;
	u8 levels[FAN5702_NUM_CHANNELS];
	int ret = 0;

	mutex_lock(&chip->lock);
	memcpy(levels, chip->levels, sizeof(levels));
	levels[led->channel] = brightness;
	if (!chip->suspended) {
		if (!chip->powered) {
			ret = fan5702_restore(chip, levels);
		} else {
			ret = fan5702_write_levels(chip, levels);
			if (ret && fan5702_write_levels(chip, chip->levels))
				dev_warn_ratelimited(&chip->client->dev,
						     "failed to restore LED levels\n");
		}
	}
	if (!ret)
		chip->levels[led->channel] = brightness;
	mutex_unlock(&chip->lock);

	return ret;
}

static enum led_brightness fan5702_brightness_get(struct led_classdev *cdev)
{
	struct fan5702_led *led = container_of(cdev, struct fan5702_led, cdev);
	struct fan5702 *chip = led->chip;
	u8 brightness;

	mutex_lock(&chip->lock);
	brightness = chip->levels[led->channel];
	mutex_unlock(&chip->lock);
	return brightness;
}

static int fan5702_read_boot_state(struct fan5702 *chip)
{
	struct i2c_client *client = chip->client;
	unsigned int channel;
	int config, general, level;

	level = i2c_smbus_read_byte_data(client, FAN5702_CHA);
	if (level < 0)
		return level;
	if ((level & FAN5702_VENDOR_MASK) != FAN5702_VENDOR_MASK)
		return -ENODEV;

	general = i2c_smbus_read_byte_data(client, FAN5702_GENERAL);
	if (general < 0)
		return general;
	config = i2c_smbus_read_byte_data(client, FAN5702_CONFIG);
	if (config < 0)
		return config;
	if (config != FAN5702_BOARD_CONFIG || (general & ~FAN5702_ENABLE_MASK))
		return -EINVAL;

	for (channel = 0; channel < FAN5702_NUM_CHANNELS; channel++) {
		if (!(general & BIT(channel)))
			continue;

		level = i2c_smbus_read_byte_data(client, fan5702_brightness_regs[channel]);
		if (level < 0)
			return level;
		chip->levels[channel] = min((level & FAN5702_BRIGHTNESS_MASK) + 1,
					    FAN5702_MAX_BRIGHTNESS);
	}

	return 0;
}

static int fan5702_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct fan5702 *chip;
	unsigned int channel;
	int was_enabled, ret;

	if (client->addr != FAN5702_ADDRESS ||
	    !i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_BYTE_DATA))
		return -ENODEV;

	chip = devm_kzalloc(dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;
	chip->client = client;
	mutex_init(&chip->lock);
	i2c_set_clientdata(client, chip);

	ret = devm_acpi_dev_add_driver_gpios(dev, fan5702_gpios);
	if (ret)
		return dev_err_probe(dev, ret, "failed to map the ACPI GPIO\n");
	chip->enable_gpio = devm_gpiod_get(dev, "enable", GPIOD_ASIS);
	if (IS_ERR(chip->enable_gpio))
		return dev_err_probe(dev, PTR_ERR(chip->enable_gpio),
				     "failed to get the enable GPIO\n");

	was_enabled = gpiod_get_raw_value_cansleep(chip->enable_gpio);
	if (was_enabled < 0)
		return dev_err_probe(dev, was_enabled, "failed to read enable GPIO\n");
	if (!was_enabled) {
		ret = fan5702_power_on(chip);
		if (ret)
			return dev_err_probe(dev, ret, "failed to assert enable GPIO\n");
	}
	ret = fan5702_read_boot_state(chip);
	if (ret) {
		if (!was_enabled)
			fan5702_disable(chip);
		return dev_err_probe(dev, ret, "failed to validate FAN5702 board state\n");
	}
	chip->powered = true;

	ret = devm_add_action_or_reset(dev, fan5702_disable, chip);
	if (ret)
		return ret;

	for (channel = 0; channel < FAN5702_NUM_CHANNELS; channel++) {
		struct fan5702_led *led = &chip->leds[channel];

		led->chip = chip;
		led->channel = channel;
		led->cdev.name = fan5702_led_names[channel];
		led->cdev.max_brightness = FAN5702_MAX_BRIGHTNESS;
		led->cdev.brightness = chip->levels[channel];
		led->cdev.brightness_set_blocking = fan5702_brightness_set;
		led->cdev.brightness_get = fan5702_brightness_get;
		ret = devm_led_classdev_register(dev, &led->cdev);
		if (ret)
			return dev_err_probe(dev, ret, "failed to register status LED %u\n",
					     channel + 1);
	}

	return 0;
}

static int fan5702_suspend(struct device *dev)
{
	struct fan5702 *chip = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&chip->lock);
	ret = fan5702_power_off(chip);
	if (!ret)
		chip->suspended = true;
	mutex_unlock(&chip->lock);

	return ret;
}

static int fan5702_resume(struct device *dev)
{
	struct fan5702 *chip = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&chip->lock);
	chip->suspended = false;
	ret = fan5702_restore(chip, chip->levels);
	mutex_unlock(&chip->lock);

	return ret;
}

static void fan5702_shutdown(struct i2c_client *client)
{
	struct fan5702 *chip = i2c_get_clientdata(client);

	mutex_lock(&chip->lock);
	chip->suspended = true;
	fan5702_disable(chip);
	mutex_unlock(&chip->lock);
}

static DEFINE_SIMPLE_DEV_PM_OPS(fan5702_pm_ops, fan5702_suspend, fan5702_resume);

static const struct acpi_device_id fan5702_acpi_ids[] = {
	{ "MSFT0A0B" },
	{ }
};
MODULE_DEVICE_TABLE(acpi, fan5702_acpi_ids);

static struct i2c_driver fan5702_driver = {
	.driver = {
		.name = "leds-fan5702",
		.acpi_match_table = fan5702_acpi_ids,
		.pm = pm_sleep_ptr(&fan5702_pm_ops),
	},
	.probe = fan5702_probe,
	.shutdown = fan5702_shutdown,
};
module_i2c_driver(fan5702_driver);

MODULE_DESCRIPTION("FAN5702 status LED driver for Microsoft HoloLens 1");
MODULE_LICENSE("GPL");
