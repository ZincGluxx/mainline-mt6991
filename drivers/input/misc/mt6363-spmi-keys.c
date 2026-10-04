// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek MT6363 SPMI PMIC keys driver (polled) for the MT6991 port.
 *
 * The MT6991 mainline SPMI controller (mt6991_pmif_arb) runs without an
 * interrupt domain (the mt8196 RCS layout storms on this SoC), so the
 * PMIC IRQs are not available.  The PWRKEY and HOMEKEY debounce status
 * bits in TOPSTATUS are polled instead and reported through the input
 * subsystem, which is how the MT6895 mainline port handles it too.
 *
 * The physical power and volume-up buttons are the MT6363 PWRKEY/HOMEKEY
 * lines; volume-down is a separate SoC GPIO described with gpio-keys.
 *
 * TOPSTATUS layout: PWRKEY debounce at bit1, HOMEKEY debounce at bit3
 * (a pressed key reads back as 0).
 */

#include <linux/delay.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/pm_wakeup.h>
#include <linux/regmap.h>
#include <linux/spmi.h>
#include <linux/workqueue.h>

#define MT6363_REG_TOPSTATUS	0x1e

#define MT6363_PWRKEY_BIT	1
#define MT6363_HOMEKEY_BIT	3

#define MT6363_KEYS_MAX		4
#define MT6363_KEYS_POLL_MS	30

static const struct regmap_config mt6363_keys_regmap_config = {
	.reg_bits	= 16,
	.val_bits	= 8,
	.max_register	= 0xffff,
};

struct mt6363_key {
	unsigned int keycode;
	unsigned int bit;
	bool wakeup;
	bool pressed;
	int irq;
};

struct mt6363_keys {
	struct device *dev;
	struct regmap *regmap;
	struct input_dev *input_dev;
	struct delayed_work poll_work;
	struct mt6363_key keys[MT6363_KEYS_MAX];
	int num_keys;
	u32 last_status;
};

static void mt6363_keys_work(struct work_struct *work)
{
	struct mt6363_keys *keys = container_of(work, struct mt6363_keys,
						poll_work.work);
	unsigned int status;
	int i, ret;

	ret = regmap_read(keys->regmap, MT6363_REG_TOPSTATUS, &status);
	if (ret) {
		dev_err_ratelimited(keys->dev,
				    "failed to read TOPSTATUS: %d\n", ret);
		goto reschedule;
	}

	if (status != keys->last_status) {
		dev_info(keys->dev, "TOPSTATUS = 0x%02x\n", status);
		keys->last_status = status;
	}

	for (i = 0; i < keys->num_keys; i++) {
		bool pressed = !(status & BIT(keys->keys[i].bit));

		if (pressed == keys->keys[i].pressed)
			continue;

		keys->keys[i].pressed = pressed;
		input_report_key(keys->input_dev, keys->keys[i].keycode,
				 pressed);
		input_sync(keys->input_dev);
		dev_info(keys->dev, "key %d (%u) %s\n", i,
			 keys->keys[i].keycode, pressed ? "down" : "up");
	}

reschedule:
	schedule_delayed_work(&keys->poll_work,
			      msecs_to_jiffies(MT6363_KEYS_POLL_MS));
}

static unsigned int mt6363_keys_bit(struct device_node *child, int index)
{
	u32 bit;

	if (!of_property_read_u32(child, "mediatek,debounce-bit", &bit))
		return bit;

	if (of_node_name_eq(child, "power"))
		return MT6363_PWRKEY_BIT;

	if (of_node_name_eq(child, "home"))
		return MT6363_HOMEKEY_BIT;

	/* Fall back to the documented power/home order. */
	return index == 0 ? MT6363_PWRKEY_BIT : MT6363_HOMEKEY_BIT;
}

static struct device_node *mt6363_keys_node(struct device *dev)
{
	struct device_node *np;

	np = of_get_child_by_name(dev->of_node, "keys");
	if (np)
		return np;

	np = of_get_child_by_name(dev->of_node, "mt6363keys");
	if (np)
		return np;

	for_each_child_of_node(dev->of_node, np) {
		if (of_device_is_compatible(np, "mediatek,mt6363-keys"))
			break;
	}

	return np;
}

static int mt6363_keys_parse(struct mt6363_keys *keys)
{
	struct device *dev = keys->dev;
	struct device_node *keys_np, *child;
	struct mt6363_key *key;
	int i = 0, ret = 0;

	keys_np = mt6363_keys_node(dev);
	if (!keys_np)
		return 0;

	for_each_available_child_of_node(keys_np, child) {
		if (i >= MT6363_KEYS_MAX)
			break;

		key = &keys->keys[i];
		if (of_property_read_u32(child, "linux,keycodes",
					 &key->keycode)) {
			dev_err(dev, "key %d: missing linux,keycodes\n", i);
			ret = -EINVAL;
			break;
		}

		key->bit = mt6363_keys_bit(child, i);
		key->wakeup = of_property_read_bool(child, "wakeup-source");
		i++;
	}

	of_node_put(keys_np);
	keys->num_keys = i;

	return ret;
}

static irqreturn_t mt6363_keys_wake_irq(int irq, void *data)
{
	struct mt6363_keys *keys = data;

	pm_wakeup_event(keys->dev, 0);

	return IRQ_HANDLED;
}

static void mt6363_keys_setup_wakeup(struct mt6363_keys *keys)
{
	struct device *dev = keys->dev;
	int i, irq, error;

	for (i = 0; i < keys->num_keys; i++) {
		if (!keys->keys[i].wakeup)
			continue;

		/*
		 * Optional per-key PMIC interrupt.  On MT6991 the SPMI
		 * controller has no IRQ domain, so this normally fails and
		 * only gpio-keys (volume-down) can wake the system.
		 */
		irq = of_irq_get(dev->of_node, i);
		if (irq <= 0) {
			dev_info(dev,
				 "key %d: no PMIC IRQ, not a wakeup source\n",
				 i);
			continue;
		}

		error = devm_request_threaded_irq(dev, irq, NULL,
						  mt6363_keys_wake_irq,
						  IRQF_ONESHOT,
						  "mt6363-spmi-keys", keys);
		if (error) {
			dev_warn(dev, "key %d: failed to request IRQ %d: %d\n",
				 i, irq, error);
			continue;
		}

		device_init_wakeup(dev, true);
		enable_irq_wake(irq);
		keys->keys[i].irq = irq;
		dev_info(dev, "key %d: IRQ %d armed as wakeup source\n",
			 i, irq);
	}
}

static int mt6363_keys_probe(struct spmi_device *sdev)
{
	struct device *dev = &sdev->dev;
	struct mt6363_keys *keys;
	int i, error;

	keys = devm_kzalloc(dev, sizeof(*keys), GFP_KERNEL);
	if (!keys)
		return -ENOMEM;

	keys->dev = dev;
	INIT_DELAYED_WORK(&keys->poll_work, mt6363_keys_work);

	keys->regmap = devm_regmap_init_spmi_ext(sdev,
						 &mt6363_keys_regmap_config);
	if (IS_ERR(keys->regmap))
		return dev_err_probe(dev, PTR_ERR(keys->regmap),
				     "failed to init SPMI regmap\n");

	error = mt6363_keys_parse(keys);
	if (error)
		return error;

	if (keys->num_keys == 0) {
		dev_warn(dev, "no keys described, nothing to do\n");
		return 0;
	}

	keys->input_dev = devm_input_allocate_device(dev);
	if (!keys->input_dev)
		return -ENOMEM;

	keys->input_dev->name = "mt6363-spmi-keys";
	keys->input_dev->id.bustype = BUS_HOST;
	keys->input_dev->dev.parent = dev;

	__set_bit(EV_KEY, keys->input_dev->evbit);
	for (i = 0; i < keys->num_keys; i++)
		__set_bit(keys->keys[i].keycode, keys->input_dev->keybit);

	error = input_register_device(keys->input_dev);
	if (error)
		return error;

	spmi_device_set_drvdata(sdev, keys);

	mt6363_keys_setup_wakeup(keys);

	schedule_delayed_work(&keys->poll_work,
			      msecs_to_jiffies(MT6363_KEYS_POLL_MS));

	dev_info(dev, "registered %d keys (polling %d ms)\n",
		 keys->num_keys, MT6363_KEYS_POLL_MS);

	return 0;
}

static void mt6363_keys_remove(struct spmi_device *sdev)
{
	struct mt6363_keys *keys = spmi_device_get_drvdata(sdev);

	if (keys)
		cancel_delayed_work_sync(&keys->poll_work);
}

static const struct of_device_id mt6363_keys_of_match[] = {
	{ .compatible = "mediatek,mt6363" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6363_keys_of_match);

static struct spmi_driver mt6363_keys_driver = {
	.probe = mt6363_keys_probe,
	.remove = mt6363_keys_remove,
	.driver = {
		.name = "mt6363-spmi-keys",
		.of_match_table = mt6363_keys_of_match,
	},
};
module_spmi_driver(mt6363_keys_driver);

MODULE_AUTHOR("realme GT7 mainline port");
MODULE_DESCRIPTION("MediaTek MT6363 SPMI PMIC polled keys");
MODULE_LICENSE("GPL");
