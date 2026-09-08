// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.

#include <linux/module.h>
#include <linux/init.h>
#include <linux/i2c.h>
#include <linux/regmap.h>
#include <linux/gpio.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/of_device.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <linux/regulator/of_regulator.h>

#define SGM3804_REG_POSITIVE_VOLT	0x00
#define SGM3804_REG_NEGATIVE_VOLT	0x01
#define SGM3804_REG_CONTROL              0x03

#define SGM3804_REG_ADD50_MASK          0x04
#define SGM3804_REG_ADD50_SHIFT         2
#define SGM3804_REG_DISP_MASK           0x02
#define SGM3804_REG_DISP_SHIFT          1
#define SGM3804_REG_DISN_MASK           0x01
#define SGM3804_REG_DISN_SHIFT          0

#define SGM3804_BASE_MV               (-6400)
#define SGM3804_STEP_MV                 100
#define SGM3804_MAX_STEPS               27

#define SGM3804_MAX_LDO                 2

struct sgm3804_chip {
	struct device *dev;
	struct regmap *regmap;
	struct regulator *vio_reg;
	int    en_p_gpio;
	int    en_n_gpio;
};

struct regulator_data {
	char *name;
	int min_uv;
	int max_uv;
	unsigned char reg;
	unsigned char mask;
	unsigned char shift;
};

struct sgm3804_regulator {
	struct device *dev;
	struct regmap *regmap;
	struct regulator_desc rdesc;
	struct regulator_dev *rdev;
	struct device_node *of_node;
	struct sgm3804_chip	*chip;
	struct notifier_block nb;
	int    uv;
	int    enabled;
	int    index;
	unsigned char ctrl_reg_val;
	unsigned char selector;
};

struct sgm3804_volt_code {
	unsigned char code;
	int n_volt;
	int p_volt;
};

struct sgm3804_volt_code sgm3804_volt_code_map[] = {
	{ 0x18, -6400, 6400 },
	{ 0x17, -6300, 6300 },
	{ 0x16, -6200, 6200 },
	{ 0x15, -6100, 6100 },
	{ 0x14, -6000, 6000 },
	{ 0x13, -5900, 5800 },
	{ 0x12, -5800, 5900 },
	{ 0x11, -5700, 5700 },
	{ 0x10, -5600, 5600 },
	{ 0x0f, -5500, 5500 },
	{ 0x0e, -5400, 5400 },
	{ 0x0d, -5300, 5300 },
	{ 0x0c, -5200, 5200 },
	{ 0x0b, -5101, 5099 },
	{ 0x0a, -5000, 5000 },
	{ 0x09, -4900, 4900 },
	{ 0x08, -4800, 4800 },
	{ 0x07, -4700, 4700 },
	{ 0x06, -4600, 4600 },
	{ 0x05, -4500, 4500 },
	{ 0x04, -4400, 4400 },
	{ 0x03, -4300, 4300 },
	{ 0x02, -4200, 4200 },
	{ 0x01, -4100, 4100 },
	{ 0x00, -4000, 4000 },
	{ 0x2f, -3900, 3900 },
	{ 0x2e, -3800, 3800 },
	{ 0x2d, -3700, 3700 },
	{ 0x2c, -3600, 3600 },
	{ 0x2b, -3500, 3500 },
	{ 0x2a, -3400, 3400 },
	{ 0x29, -3300, 3300 },
	{ 0x28, -3200, 3200 },
	{ 0x27, -3100, 3100 },
	{ 0x26, -3000, 3000 },
	{ 0x25, -2900, 2900 },
	{ 0x24, -2800, 2800 },
	{ 0x23, -2700, 2700 },
	{ 0x22, -2600, 2600 },
	{ 0x21, -2500, 2500 },
	{ 0x20, -2400, 2400 }
};

static const struct regmap_config regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0x03,
};

static int binary_search_volt_code(int mv)
{
	int low = 0, mid = 0, high = 0;

	high = ARRAY_SIZE(sgm3804_volt_code_map) - 1;

	while (low <= high) {
		mid = low + (high - low) / 2;
		if (sgm3804_volt_code_map[mid].p_volt == mv)
			return mid;
		else if (sgm3804_volt_code_map[mid].p_volt < mv)
			high = mid - 1;
		else
			low = mid + 1;
		}

	return -EINVAL;
}

static int sgm3804_write_voltage(struct sgm3804_regulator *regulator)
{
	int mv = abs(regulator->uv) / 1000;
	int i, ret, reg_val;

	i = binary_search_volt_code(mv);
	if (i < 0) {
		pr_err("<%s %d> failed to find voltage code for %d mV\n",
			__func__, __LINE__, mv);
		return -1;
	}

	reg_val = sgm3804_volt_code_map[i].code;

	ret = regmap_write(regulator->regmap, SGM3804_REG_POSITIVE_VOLT, reg_val);
	if (ret) {
		pr_err("<%s %d> fail to write positive volt, ret %d\n",
			__func__, __LINE__, ret);
		return -1;
	}

	ret = regmap_write(regulator->regmap, SGM3804_REG_NEGATIVE_VOLT, reg_val);
	if (ret) {
		pr_err("<%s %d> fail to write negative volt, ret %d\n",
			__func__, __LINE__, ret);
		return -1;
	}

	return i;
}

static int sgm3804_regulator_get_voltage(struct regulator_dev *rdev)
{
	struct sgm3804_regulator *regulator = rdev_get_drvdata(rdev);

	if (!regulator) {
		pr_err("<%s %d> invalid sgm3804 regulator\n",
			__func__, __LINE__);
		return -EINVAL;
	}
	dev_dbg(regulator->dev, "<%s %d> get voltage [%d]\n",
		__func__, __LINE__, regulator->uv);

	return regulator->uv;
}

static int sgm3804_regulator_set_voltage(struct regulator_dev *rdev,
		int min_uv, int max_uv,
		unsigned int *selector)
{
	struct sgm3804_regulator *regulator = rdev_get_drvdata(rdev);
	int mv = max_uv / 1000;
	int i;

	if (!regulator) {
		pr_err("<%s %d> invalid sgm3804 regulator\n",
			__func__, __LINE__);
		return -EINVAL;
	}

	if (min_uv > max_uv) {
		pr_err("<%s %d> invalid voltage range: min_uv (%d) > max_uv (%d)\n",
			__func__, __LINE__, min_uv, max_uv);
		return -EINVAL;
	}

	i = binary_search_volt_code(mv);
	if (i < 0) {
		pr_err("<%s %d> failed to find voltage code for %d mV\n",
			__func__, __LINE__, mv);
		return -1;
	}

	regulator->selector = *selector = i;
	regulator->uv = max_uv;
	dev_info(regulator->dev, "<%s %d> set voltage [%d %d], selector = %d\n",
		__func__, __LINE__, min_uv, max_uv, *selector);

	return 0;
}

static int sgm3804_regulator_list_voltage(struct regulator_dev *rdev,
		unsigned int selector)
{
	int uV;
	struct sgm3804_regulator *regulator = rdev_get_drvdata(rdev);

	if (!regulator) {
		pr_err("<%s %d> invalid sgm3804 regulator\n",
			__func__, __LINE__);
		return -EINVAL;
	}

	uV = (sgm3804_volt_code_map[selector].p_volt) * 1000;
	dev_dbg(regulator->dev, "<%s %d> selector %d, voltage %d uV\n",
		__func__, __LINE__, selector, uV);

	return uV;
}

static int sgm3804_regulator_enable(struct regulator_dev *rdev)
{
	int ret;
	unsigned char reg_val;
	struct sgm3804_regulator *regulator = rdev_get_drvdata(rdev);
	struct sgm3804_chip *chip = regulator->chip;

	if (!regulator) {
		pr_err("<%s %d> invalid sgm3804 regulator\n",
			__func__, __LINE__);
		return -EINVAL;
	}

	if (gpio_is_valid(chip->en_p_gpio))
		gpio_direction_output(chip->en_p_gpio, 1);

	usleep_range(5000, 5000 + 100);

	ret = sgm3804_write_voltage(regulator);
	if (ret < 0) {
		pr_err("<%s %d> fail to write voltage, ret %d\n",
			__func__, __LINE__, ret);
		return ret;
	}
	regulator->index = ret;

	reg_val = regulator->ctrl_reg_val | (1 << SGM3804_REG_DISP_SHIFT);
	ret = regmap_write(regulator->regmap, SGM3804_REG_CONTROL, reg_val);
	if (ret) {
		pr_err("<%s %d> fail to write control reg\n",
			__func__, __LINE__);
		return ret;
	}
	regulator->ctrl_reg_val = reg_val;

	usleep_range(2000, 2000 + 100);

	if (gpio_is_valid(chip->en_n_gpio))
		gpio_direction_output(chip->en_n_gpio, 1);

	usleep_range(5000, 5000 + 100);

	reg_val = regulator->ctrl_reg_val | (1 << SGM3804_REG_DISN_SHIFT);
	ret = regmap_write(regulator->regmap, SGM3804_REG_CONTROL, reg_val);
	if (ret) {
		pr_err("<%s %d> fail to write control reg\n",
			__func__, __LINE__);
		return ret;
	}
	regulator->ctrl_reg_val = reg_val;
	regulator->enabled = 1;

	dev_info(regulator->dev, "<%s %d> regulator[%s] enable success\n",
		__func__, __LINE__, regulator->rdesc.name);

	return 0;
}

static int sgm3804_regulator_disable(struct regulator_dev *rdev)
{
	struct sgm3804_regulator *regulator = rdev_get_drvdata(rdev);
	struct sgm3804_chip *chip = regulator->chip;

	if (!regulator) {
		pr_err("<%s %d> invalid sgm3804 regulator\n",
			__func__, __LINE__);
		return -EINVAL;
	}

	regulator->ctrl_reg_val &= (~SGM3804_REG_DISP_MASK);
	regulator->ctrl_reg_val &= (~SGM3804_REG_DISN_MASK);

	if (gpio_is_valid(chip->en_n_gpio))
		gpio_direction_output(chip->en_n_gpio, 0);

	usleep_range(2000, 2000 + 100);

	if (gpio_is_valid(chip->en_p_gpio))
		gpio_direction_output(chip->en_p_gpio, 0);

	regulator->enabled = 0;

	dev_dbg(regulator->dev, "<%s %d> regulator[%s] disable success\n",
		__func__, __LINE__, regulator->rdesc.name);

	return 0;
}

static int sgm3804_regulator_is_enabled(struct regulator_dev *rdev)
{
	struct sgm3804_regulator *regulator = rdev_get_drvdata(rdev);

	if (!regulator) {
		pr_err("<%s %d> invalid sgm3804 regulator\n",
			__func__, __LINE__);
		return -EINVAL;
	}

	dev_dbg(regulator->dev, "<%s %d> regulator[%s] enabled = %d\n",
		__func__, __LINE__, regulator->rdesc.name, regulator->enabled);

	return regulator->enabled;
}

static const struct regulator_ops sgm3804_regulator_ops = {
	.list_voltage = sgm3804_regulator_list_voltage,
	.get_voltage = sgm3804_regulator_get_voltage,
	.set_voltage = sgm3804_regulator_set_voltage,
	.enable = sgm3804_regulator_enable,
	.disable = sgm3804_regulator_disable,
	.is_enabled = sgm3804_regulator_is_enabled,
};

static const struct regulator_desc sgm3804_regulator_desc = {
	.name = "sgm3804",
	.id = 0,
	.ops = &sgm3804_regulator_ops,
	.type = REGULATOR_VOLTAGE,
	.owner = THIS_MODULE,
};

static int sgm3804_register_ldo(struct sgm3804_regulator *sgm3804_reg,
		const char *name, struct device_node *child)
{
	int rc;
	struct device *dev = sgm3804_reg->dev;
	struct regulator_config config = {};
	struct regulator_init_data *init_data;
	struct device_node *reg_node = child;

	init_data = of_get_regulator_init_data(dev, reg_node, &sgm3804_reg->rdesc);
	if (init_data == NULL) {
		pr_err("<%s %d> %s: failed to get regulator init data\n",
			__func__, __LINE__, name);
		return -ENODATA;
	}
	if (!init_data->constraints.name) {
		pr_err("<%s %d> %s: regulator name missing\n",
			__func__, __LINE__, name);
		return -EINVAL;
	}

	init_data->constraints.input_uV = init_data->constraints.max_uV;
	init_data->constraints.valid_ops_mask |= REGULATOR_CHANGE_STATUS
						| REGULATOR_CHANGE_VOLTAGE;

	config.dev = dev;
	config.init_data = init_data;
	config.driver_data = sgm3804_reg;
	config.of_node = reg_node;

	sgm3804_reg->of_node = reg_node;
	sgm3804_reg->regmap = sgm3804_reg->chip->regmap;

	sgm3804_reg->rdesc.type = REGULATOR_VOLTAGE;
	sgm3804_reg->rdesc.ops = &sgm3804_regulator_ops;
	sgm3804_reg->rdesc.name = init_data->constraints.name;
	sgm3804_reg->rdesc.uV_step = SGM3804_STEP_MV * 1000;
	sgm3804_reg->rdesc.min_uV = 2400000;
	sgm3804_reg->rdesc.n_voltages = ((6400000 - 2400000)
			/ sgm3804_reg->rdesc.uV_step) + 1;

	sgm3804_reg->rdev = devm_regulator_register(dev, &sgm3804_reg->rdesc,
						&config);
	if (IS_ERR(sgm3804_reg->rdev)) {
		rc = PTR_ERR(sgm3804_reg->rdev);
		pr_err("<%s %d> %s: failed to register regulator rc=%d\n",
			__func__, __LINE__, sgm3804_reg->rdesc.name, rc);
		return rc;
	}

	return 0;
}

static int sgm3804_parse_regulator(struct sgm3804_chip *chip)
{
	int rc = 0;
	const char *name;
	struct device_node *child;
	struct sgm3804_regulator *sgm3804_reg;

	/* parse each subnode and register regulator for regulator child */
	for_each_available_child_of_node(chip->dev->of_node, child) {
		sgm3804_reg = devm_kzalloc(chip->dev, sizeof(*sgm3804_reg), GFP_KERNEL);
		if (!sgm3804_reg) {
			rc = -ENOMEM;
			goto err_put_node;
		}

		sgm3804_reg->regmap = chip->regmap;
		sgm3804_reg->of_node = child;
		sgm3804_reg->dev = chip->dev;
		sgm3804_reg->chip = chip;

		rc = of_property_read_string(child, "regulator-name", &name);
		if (rc)
			continue;

		rc = sgm3804_register_ldo(sgm3804_reg, name, child);
		if (rc < 0) {
			pr_err("<%s %d> failed to register regulator %s rc=%d\n",
				__func__, __LINE__, name, rc);
			goto err_put_node;
		}
	}

	return 0;

err_put_node:
	of_node_put(child);
	return rc;
}

static int sgm3804_request_gpio(struct sgm3804_chip *chip)
{
	int ret;

	if (gpio_is_valid(chip->en_p_gpio)) {
		ret = gpio_request(chip->en_p_gpio, "sgm3804_en_p");
		if (ret) {
			pr_err("<%s %d> Failed to request en_p_gpio\n",
				__func__, __LINE__);
			goto err_request_en_p_gpio;
		}
	}

	if (gpio_is_valid(chip->en_n_gpio)) {
		ret = gpio_request(chip->en_n_gpio, "sgm3804_en_n");
		if (ret) {
			pr_err("<%s %d> Failed to request en_n_gpio\n",
				__func__, __LINE__);
			goto err_request_en_n_gpio;
		}
	}

	return 0;

err_request_en_n_gpio:
	if (gpio_is_valid(chip->en_p_gpio))
		gpio_free(chip->en_p_gpio);
err_request_en_p_gpio:
	return ret;
}

static int sgm3804_free_gpio(struct sgm3804_chip *chip)
{
	if (gpio_is_valid(chip->en_p_gpio))
		gpio_free(chip->en_p_gpio);

	if (gpio_is_valid(chip->en_n_gpio))
		gpio_free(chip->en_n_gpio);

	return 0;
}

static int sgm3804_i2c_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	int ret;
	struct sgm3804_chip *chip;
	struct device_node *node = client->dev.of_node;

	chip = devm_kzalloc(&client->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	chip->dev = &client->dev;

	chip->regmap = devm_regmap_init_i2c(client, &regmap_config);
	if (IS_ERR(chip->regmap)) {
		pr_err("<%s %d> Failed to initialize regmap\n",
			__func__, __LINE__);
		return PTR_ERR(chip->regmap);
	}

	chip->en_p_gpio = of_get_named_gpio(node, "bias-enp-gpio", 0);
	if (!gpio_is_valid(chip->en_p_gpio)) {
		pr_err("<%s %d> Failed to get positive enable GPIO\n",
			__func__, __LINE__);
		return chip->en_p_gpio;
	}

	chip->en_n_gpio = of_get_named_gpio(node, "bias-enn-gpio", 0);
	if (!gpio_is_valid(chip->en_n_gpio)) {
		pr_err("<%s %d> Failed to get negative enable GPIO\n",
			__func__, __LINE__);
	}

	chip->vio_reg = devm_regulator_get(&client->dev, "vio");
	if (IS_ERR(chip->vio_reg)) {
		pr_err("<%s %d> Failed to get vio regulator\n",
			__func__, __LINE__);
		return PTR_ERR(chip->vio_reg);
	}

	ret = regulator_enable(chip->vio_reg);
	if (ret) {
		pr_err("<%s %d> Failed to enable vio regulator\n",
			__func__, __LINE__);
		return ret;
	}

	ret = sgm3804_request_gpio(chip);
	if (ret) {
		pr_err("<%s %d> Failed to request GPIOs\n",
			__func__, __LINE__);
		regulator_disable(chip->vio_reg);
		return ret;
	}

	ret = sgm3804_parse_regulator(chip);
	if (ret) {
		pr_err("<%s %d> Failed to parse regulator nodes\n",
			__func__, __LINE__);
		sgm3804_free_gpio(chip);
		regulator_disable(chip->vio_reg);
		return ret;
	}

	i2c_set_clientdata(client, chip);
	dev_set_drvdata(&client->dev, chip);

	dev_info(&client->dev, "<%s %d> SGM3804 regulator register success\n",
		__func__, __LINE__);
	return 0;
}

static int sgm3804_i2c_remove(struct i2c_client *client)
{
	int ret = 0;
	struct sgm3804_chip *chip = i2c_get_clientdata(client);

	if (!chip) {
		pr_err("<%s %d> No chip data found\n",
			__func__, __LINE__);
		return -ENODEV;
	}

	i2c_set_clientdata(client, NULL);

	sgm3804_free_gpio(chip);

	if (chip->vio_reg) {
		ret = regulator_disable(chip->vio_reg);
		if (ret) {
			pr_err("<%s %d> Failed to disable vio regulator, ret %d\n",
				__func__, __LINE__, ret);
		}
	}

	return ret;
}

static void sgm3804_i2c_shutdown(struct i2c_client *client)
{
	int ret;
	struct sgm3804_chip *chip = i2c_get_clientdata(client);

	if (!chip) {
		pr_err("<%s %d> No chip data found\n",
			__func__, __LINE__);
		return;
	}

	i2c_set_clientdata(client, NULL);

	sgm3804_free_gpio(chip);

	if (chip->vio_reg) {
		ret = regulator_disable(chip->vio_reg);
		if (ret) {
			pr_err("<%s %d> Failed to disable vio regulator, ret %d\n",
				__func__, __LINE__, ret);
		}
	}
}

static int sgm3804_pm_suspend(struct device *dev)
{
	int ret;
	struct sgm3804_chip *chip = dev_get_drvdata(dev);

	if (!chip) {
		pr_err("<%s %d> No chip data found\n",
			__func__, __LINE__);
		return -ENODEV;
	}

	if (chip->vio_reg) {
		ret = regulator_disable(chip->vio_reg);
		if (ret) {
			pr_err("<%s %d> Failed to disable vio regulator, ret %d\n",
				__func__, __LINE__, ret);
			return ret;
		}
	}

	return 0;
}

static int sgm3804_pm_resume(struct device *dev)
{
	int ret;
	struct sgm3804_chip *chip = dev_get_drvdata(dev);

	if (!chip) {
		pr_err("<%s %d> No chip data found\n",
			__func__, __LINE__);
		return -ENODEV;
	}

	if (chip->vio_reg) {
		ret = regulator_enable(chip->vio_reg);
		if (ret) {
			pr_err("<%s %d> Failed to enable vio regulator, ret %d\n",
				__func__, __LINE__, ret);
			return ret;
		}
	}

	return 0;
}

static const struct dev_pm_ops sgm3804_pm_ops = {
	.suspend = sgm3804_pm_suspend,
	.resume = sgm3804_pm_resume,
};

static const struct of_device_id sgm3804_of_match[] = {
	{ .compatible = "sgmicro,sgm3804", },
	{ /* Sentinel */ }
};
MODULE_DEVICE_TABLE(of, sgm3804_of_match);

static const struct i2c_device_id sgm3804_i2c_id[] = {
	{ "sgm3804", 0 },
	{ /* Sentinel */ }
};
MODULE_DEVICE_TABLE(i2c, sgm3804_i2c_id);

static struct i2c_driver sgm3804_i2c_driver = {
	.driver = {
		.name = "sgm3804",
		.of_match_table = sgm3804_of_match,
		.pm = &sgm3804_pm_ops,
	},
	.probe = sgm3804_i2c_probe,
	.remove = sgm3804_i2c_remove,
	.shutdown = sgm3804_i2c_shutdown,
	.id_table = sgm3804_i2c_id,
};

module_i2c_driver(sgm3804_i2c_driver);

MODULE_DESCRIPTION("SGM3804 Voltage Regulator Driver");
MODULE_LICENSE("GPL v2");
