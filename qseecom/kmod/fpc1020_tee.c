// SPDX-License-Identifier: GPL-2.0-only
/*
 * FPC1020 TEE platform driver for LG V30 (joan) on mainline Linux 7.3.
 *
 * Controls vdd_io regulator, hardware reset GPIO, and rising-edge wakeup IRQ,
 * exposing the sysfs interface under /sys/devices/virtual/input/ required by
 * LG's fingerprint.msm8998.so HAL. SPI transactions are handled in TrustZone
 * by fpctzapp.
 */

#include <linux/atomic.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/pm_wakeup.h>
#include <linux/regulator/consumer.h>

#define FPC_TTW_HOLD_TIME_MS	1000
#define RESET_LOW_SLEEP_MIN_US	5000
#define RESET_LOW_SLEEP_MAX_US	5100
#define RESET_HIGH_SLEEP1_US	100
#define RESET_HIGH_SLEEP2_US	5000
#define PWR_ON_SLEEP_MIN_US	100
#define PWR_ON_SLEEP_MAX_US	1000

static const char * const pctl_names[] = {
	"fpc1020_reset_reset",
	"fpc1020_reset_active",
	"fpc1020_irq_active",
};

struct fpc1020_data {
	struct device *dev;
	struct input_dev *input;
	struct pinctrl *pinctrl;
	struct pinctrl_state *pinctrl_state[ARRAY_SIZE(pctl_names)];
	struct regulator *vdd_io;
	struct gpio_desc *irq_gpio;
	struct gpio_desc *rst_gpio;
	int irq;
	struct mutex lock;
	bool prepared;
	bool vdd_enabled;
	atomic_t wakeup_enabled;
};

static int select_pin_ctl(struct fpc1020_data *fpc, const char *name)
{
	size_t i;
	int rc;

	if (!fpc->pinctrl)
		return 0;

	for (i = 0; i < ARRAY_SIZE(pctl_names); i++) {
		if (!strncmp(pctl_names[i], name, strlen(pctl_names[i]))) {
			if (!fpc->pinctrl_state[i])
				return 0;
			rc = pinctrl_select_state(fpc->pinctrl,
						  fpc->pinctrl_state[i]);
			if (rc)
				dev_err(fpc->dev, "cannot select '%s': %d\n",
					name, rc);
			return rc;
		}
	}

	return -EINVAL;
}

static int fpc_vdd_set(struct fpc1020_data *fpc, bool enable)
{
	int rc = 0;

	if (!fpc->vdd_io)
		return 0;

	if (enable && !fpc->vdd_enabled) {
		rc = regulator_enable(fpc->vdd_io);
		if (!rc)
			fpc->vdd_enabled = true;
	} else if (!enable && fpc->vdd_enabled) {
		rc = regulator_disable(fpc->vdd_io);
		if (!rc)
			fpc->vdd_enabled = false;
	}
	return rc;
}

static int fpc_hw_reset(struct fpc1020_data *fpc)
{
	int irq_val;

	select_pin_ctl(fpc, "fpc1020_reset_active");
	gpiod_set_value_cansleep(fpc->rst_gpio, 1);
	usleep_range(RESET_HIGH_SLEEP1_US, RESET_HIGH_SLEEP1_US + 100);

	select_pin_ctl(fpc, "fpc1020_reset_reset");
	gpiod_set_value_cansleep(fpc->rst_gpio, 0);
	usleep_range(RESET_LOW_SLEEP_MIN_US, RESET_LOW_SLEEP_MAX_US);

	select_pin_ctl(fpc, "fpc1020_reset_active");
	gpiod_set_value_cansleep(fpc->rst_gpio, 1);
	usleep_range(RESET_HIGH_SLEEP2_US, RESET_HIGH_SLEEP2_US + 100);

	irq_val = gpiod_get_value_cansleep(fpc->irq_gpio);
	dev_info(fpc->dev, "IRQ after reset: %d\n", irq_val);
	return 0;
}

static int fpc_device_prepare(struct fpc1020_data *fpc, bool enable)
{
	int rc = 0;

	mutex_lock(&fpc->lock);
	if (enable && !fpc->prepared) {
		select_pin_ctl(fpc, "fpc1020_reset_reset");
		gpiod_set_value_cansleep(fpc->rst_gpio, 0);
		rc = fpc_vdd_set(fpc, true);
		if (rc)
			goto out;
		usleep_range(PWR_ON_SLEEP_MIN_US, PWR_ON_SLEEP_MAX_US);
		select_pin_ctl(fpc, "fpc1020_reset_active");
		gpiod_set_value_cansleep(fpc->rst_gpio, 1);
		fpc->prepared = true;
	} else if (!enable && fpc->prepared) {
		select_pin_ctl(fpc, "fpc1020_reset_reset");
		gpiod_set_value_cansleep(fpc->rst_gpio, 0);
		usleep_range(PWR_ON_SLEEP_MIN_US, PWR_ON_SLEEP_MAX_US);
		fpc_vdd_set(fpc, false);
		fpc->prepared = false;
	}
out:
	mutex_unlock(&fpc->lock);
	return rc;
}

static ssize_t clk_enable_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	return count;
}
static DEVICE_ATTR_WO(clk_enable);

static ssize_t pinctl_set_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct fpc1020_data *fpc = dev_get_drvdata(dev);
	int rc;

	mutex_lock(&fpc->lock);
	rc = select_pin_ctl(fpc, buf);
	mutex_unlock(&fpc->lock);
	return rc ? rc : count;
}
static DEVICE_ATTR_WO(pinctl_set);

static ssize_t regulator_enable_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t count)
{
	struct fpc1020_data *fpc = dev_get_drvdata(dev);
	char name[16];
	char op;
	int rc;

	if (sscanf(buf, "%15[^,],%c", name, &op) != 2)
		return -EINVAL;
	if (op != 'e' && op != 'd')
		return -EINVAL;

	mutex_lock(&fpc->lock);
	rc = fpc_vdd_set(fpc, op == 'e');
	mutex_unlock(&fpc->lock);
	return rc ? rc : count;
}
static DEVICE_ATTR_WO(regulator_enable);

static ssize_t hw_reset_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct fpc1020_data *fpc = dev_get_drvdata(dev);
	int rc;

	if (strncmp(buf, "reset", 5))
		return -EINVAL;

	mutex_lock(&fpc->lock);
	rc = fpc_hw_reset(fpc);
	mutex_unlock(&fpc->lock);
	return rc ? rc : count;
}
static DEVICE_ATTR_WO(hw_reset);

static ssize_t device_prepare_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	struct fpc1020_data *fpc = dev_get_drvdata(dev);
	int rc;

	if (!strncmp(buf, "enable", 6))
		rc = fpc_device_prepare(fpc, true);
	else if (!strncmp(buf, "disable", 7))
		rc = fpc_device_prepare(fpc, false);
	else
		return -EINVAL;

	return rc ? rc : count;
}
static DEVICE_ATTR_WO(device_prepare);

static ssize_t wakeup_enable_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct fpc1020_data *fpc = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", atomic_read(&fpc->wakeup_enabled));
}

static ssize_t wakeup_enable_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct fpc1020_data *fpc = dev_get_drvdata(dev);
	ssize_t ret = count;

	mutex_lock(&fpc->lock);
	if (!strncmp(buf, "enable", 6) || buf[0] == '1')
		atomic_set(&fpc->wakeup_enabled, 1);
	else if (!strncmp(buf, "disable", 7) || buf[0] == '0')
		atomic_set(&fpc->wakeup_enabled, 0);
	else
		ret = -EINVAL;
	mutex_unlock(&fpc->lock);
	return ret;
}
static DEVICE_ATTR(wakeup_enable, 0664, wakeup_enable_show,
		   wakeup_enable_store);

static ssize_t irq_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct fpc1020_data *fpc = dev_get_drvdata(dev);
	int val = gpiod_get_value_cansleep(fpc->irq_gpio);

	return sysfs_emit(buf, "%d\n", val > 0 ? 1 : 0);
}

static ssize_t irq_store(struct device *dev,
			 struct device_attribute *attr,
			 const char *buf, size_t count)
{
	return count;
}
static DEVICE_ATTR(irq, 0664, irq_show, irq_store);

static struct attribute *fpc1020_attrs[] = {
	&dev_attr_pinctl_set.attr,
	&dev_attr_device_prepare.attr,
	&dev_attr_regulator_enable.attr,
	&dev_attr_hw_reset.attr,
	&dev_attr_wakeup_enable.attr,
	&dev_attr_clk_enable.attr,
	&dev_attr_irq.attr,
	NULL,
};

static const struct attribute_group fpc1020_attr_group = {
	.attrs = fpc1020_attrs,
};

static irqreturn_t fpc1020_irq_handler(int irq, void *handle)
{
	struct fpc1020_data *fpc = handle;

	if (atomic_read(&fpc->wakeup_enabled))
		pm_wakeup_event(fpc->dev, FPC_TTW_HOLD_TIME_MS);

	sysfs_notify(&fpc->input->dev.kobj, NULL, "irq");
	return IRQ_HANDLED;
}

static int fpc1020_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct fpc1020_data *fpc;
	size_t i;
	int irqf, rc;

	fpc = devm_kzalloc(dev, sizeof(*fpc), GFP_KERNEL);
	if (!fpc)
		return -ENOMEM;

	fpc->dev = dev;
	mutex_init(&fpc->lock);
	atomic_set(&fpc->wakeup_enabled, 0);
	platform_set_drvdata(pdev, fpc);

	fpc->irq_gpio = devm_gpiod_get(dev, "irq", GPIOD_IN);
	if (IS_ERR(fpc->irq_gpio))
		return dev_err_probe(dev, PTR_ERR(fpc->irq_gpio),
				     "failed to get irq-gpios\n");

	fpc->rst_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(fpc->rst_gpio))
		return dev_err_probe(dev, PTR_ERR(fpc->rst_gpio),
				     "failed to get reset-gpios\n");

	fpc->vdd_io = devm_regulator_get_optional(dev, "vdd_io");
	if (IS_ERR(fpc->vdd_io)) {
		if (PTR_ERR(fpc->vdd_io) == -ENODEV)
			fpc->vdd_io = NULL;
		else
			return dev_err_probe(dev, PTR_ERR(fpc->vdd_io),
					     "failed to get vdd_io supply\n");
	}

	fpc->pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR(fpc->pinctrl)) {
		if (PTR_ERR(fpc->pinctrl) == -EPROBE_DEFER)
			return -EPROBE_DEFER;
		fpc->pinctrl = NULL;
	} else {
		for (i = 0; i < ARRAY_SIZE(pctl_names); i++) {
			struct pinctrl_state *st;

			st = pinctrl_lookup_state(fpc->pinctrl, pctl_names[i]);
			if (!IS_ERR(st))
				fpc->pinctrl_state[i] = st;
		}
		select_pin_ctl(fpc, "fpc1020_reset_reset");
		select_pin_ctl(fpc, "fpc1020_irq_active");
	}

	fpc->irq = platform_get_irq_optional(pdev, 0);
	if (fpc->irq < 0)
		fpc->irq = gpiod_to_irq(fpc->irq_gpio);
	if (fpc->irq < 0)
		return dev_err_probe(dev, fpc->irq, "failed to get IRQ\n");

	/*
	 * Register under /sys/devices/virtual/input/ (parent == NULL) because
	 * LG's fingerprint.msm8998.so scans /sys/devices/virtual/input/ for a
	 * node with name == "fingerprint".
	 */
	fpc->input = input_allocate_device();
	if (!fpc->input)
		return -ENOMEM;

	fpc->input->name = "fingerprint";
	fpc->input->dev.init_name = "lge_fingerprint";
	input_set_drvdata(fpc->input, fpc);

	rc = input_register_device(fpc->input);
	if (rc) {
		input_free_device(fpc->input);
		return rc;
	}

	rc = sysfs_create_group(&fpc->input->dev.kobj, &fpc1020_attr_group);
	if (rc)
		goto err_unreg_input;

	irqf = IRQF_TRIGGER_RISING | IRQF_ONESHOT;
	if (of_property_read_bool(dev->of_node, "fpc,enable-wakeup")) {
		irqf |= IRQF_NO_SUSPEND;
		device_init_wakeup(dev, true);
	}

	rc = devm_request_threaded_irq(dev, fpc->irq, NULL,
				       fpc1020_irq_handler, irqf,
				       dev_name(dev), fpc);
	if (rc)
		goto err_rm_sysfs;

	enable_irq_wake(fpc->irq);

	if (of_property_read_bool(dev->of_node, "fpc,enable-on-boot"))
		fpc_device_prepare(fpc, true);

	mutex_lock(&fpc->lock);
	fpc_hw_reset(fpc);
	mutex_unlock(&fpc->lock);

	dev_info(dev, "fpc1020_tee ready (irq=%d)\n", fpc->irq);
	return 0;

err_rm_sysfs:
	sysfs_remove_group(&fpc->input->dev.kobj, &fpc1020_attr_group);
err_unreg_input:
	input_unregister_device(fpc->input);
	return rc;
}

static void fpc1020_remove(struct platform_device *pdev)
{
	struct fpc1020_data *fpc = platform_get_drvdata(pdev);

	disable_irq_wake(fpc->irq);
	sysfs_remove_group(&fpc->input->dev.kobj, &fpc1020_attr_group);
	input_unregister_device(fpc->input);
	fpc_device_prepare(fpc, false);
}

static const struct of_device_id fpc1020_of_match[] = {
	{ .compatible = "fpc,fpc1020" },
	{}
};
MODULE_DEVICE_TABLE(of, fpc1020_of_match);

static struct platform_driver fpc1020_driver = {
	.driver = {
		.name = "fpc1020",
		.of_match_table = fpc1020_of_match,
	},
	.probe = fpc1020_probe,
	.remove = fpc1020_remove,
};

module_platform_driver(fpc1020_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("FPC1020 TEE fingerprint platform driver for LG V30");
