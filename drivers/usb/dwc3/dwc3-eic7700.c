// SPDX-License-Identifier: GPL-2.0
/*
 * Eswin Specific Glue layer
 *
 * Copyright 2024, Beijing ESWIN Computing Technology Co., Ltd.. All rights reserved.
 *
 * Authors: Wei Yang <yangwei1@eswincomputing.com>
 *          Senchuan Zhang <zhangsenchuan@eswincomputing.com>
 */

#include <linux/async.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/pm_runtime.h>
#include <linux/extcon.h>
#include <linux/freezer.h>
#include <linux/iopoll.h>
#include <linux/reset.h>
#include <linux/usb.h>
#include <linux/pm.h>
#include <linux/usb/hcd.h>
#include <linux/usb/ch9.h>
#include <linux/extcon-provider.h>
#include <linux/mfd/syscon.h>
#include <linux/bitfield.h>
#include <linux/regmap.h>
#include <linux/gpio/consumer.h>
#include "core.h"
#include "io.h"

#define HSP_USB_VBUS_FSEL 0x2a
#define HSP_USB_MPLL_DEFAULT 0x0

#define HSP_USB_BUS_FILTER_EN (0x1 << 0)
#define HSP_USB_BUS_CLKEN_GM (0x1 << 9)
#define HSP_USB_BUS_CLKEN_GS (0x1 << 16)
#define HSP_USB_BUS_SW_RST (0x1 << 24)
#define HSP_USB_BUS_CLK_EN (0x1 << 28)

#define HSP_USB_AXI_LP_XM_CSYSREQ (0x1 << 0)
#define HSP_USB_AXI_LP_XS_CSYSREQ (0x1 << 16)

struct dwc3_eswin {
	int num_clks;
	bool connected;
	bool suspended;
	bool force_mode;
	bool is_phy_on;
	struct device *dev;
    struct clk_bulk_data *clks;
	struct dwc3 *dwc;
	struct extcon_dev *edev;
	struct usb_hcd *hcd;
	struct notifier_block device_nb;
	struct notifier_block host_nb;
	struct work_struct otg_work;
	struct mutex lock;
	struct reset_control *vaux_rst;
	struct device *child_dev;
	enum usb_role new_usb_role;
	struct gpio_desc *hub_gpio;
};

static ssize_t dwc3_mode_show(struct device *device,
			     struct device_attribute *attr, char *buf)
{
	struct dwc3_eswin *eswin = dev_get_drvdata(device);
	struct dwc3 *dwc = eswin->dwc;
	int ret;

	switch (dwc->current_dr_role) {
	case USB_DR_MODE_HOST:
		ret = sysfs_emit(buf, "host\n");
		break;
	case USB_DR_MODE_PERIPHERAL:
		ret = sysfs_emit(buf, "peripheral\n");
		break;
	case USB_DR_MODE_OTG:
		ret = sysfs_emit(buf, "otg\n");
		break;
	default:
		ret = sysfs_emit(buf, "UNKNOWN\n");
	}

	return ret;
}

static ssize_t dwc3_mode_store(struct device *device,
			     struct device_attribute *attr, const char *buf,
			     size_t count)
{
	struct dwc3_eswin *eswin = dev_get_drvdata(device);
	struct dwc3 *dwc = eswin->dwc;
	enum usb_role new_role;
	struct usb_role_switch *role_sw = dwc->role_sw;

	if (!strncmp(buf, "1", 1) || !strncmp(buf, "host", 4)) {
		new_role = USB_ROLE_HOST;
	} else if (!strncmp(buf, "0", 1) || !strncmp(buf, "peripheral", 10)) {
		new_role = USB_ROLE_DEVICE;
	} else {
		dev_info(eswin->dev, "illegal dr_mode\n");
		return count;
	}
	eswin->force_mode = true;

	mutex_lock(&eswin->lock);
	usb_role_switch_set_role(role_sw, new_role);
	mutex_unlock(&eswin->lock);

	return count;
}

static DEVICE_ATTR_RW(dwc3_mode);

static ssize_t dwc3_hub_rst_show(struct device *device,
				 struct device_attribute *attr, char *buf)
{
	struct dwc3_eswin *eswin = dev_get_drvdata(device);

	if (!IS_ERR(eswin->hub_gpio))
		return sprintf(buf, "%d", gpiod_get_raw_value(eswin->hub_gpio));

	return sysfs_emit(buf, "UNKONWN");
}

static ssize_t dwc3_hub_rst_store(struct device *device,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct dwc3_eswin *eswin = dev_get_drvdata(device);

	if (!IS_ERR(eswin->hub_gpio)) {
		if (!strncmp(buf, "0", 1))
			gpiod_set_raw_value(eswin->hub_gpio, 0);
		else
			gpiod_set_raw_value(eswin->hub_gpio, 1);
	}

	return count;
}

static DEVICE_ATTR_RW(dwc3_hub_rst);

static struct attribute *dwc3_eswin_attrs[] = {
	&dev_attr_dwc3_mode.attr,
	&dev_attr_dwc3_hub_rst.attr,
	NULL,
};

static struct attribute_group dwc3_eswin_attr_group = {
	.name = NULL, /* we want them in the same directory */
	.attrs = dwc3_eswin_attrs,
};

static int dwc3_eswin_device_notifier(struct notifier_block *nb,
				      unsigned long event, void *ptr)
{
	struct dwc3_eswin *eswin =
		container_of(nb, struct dwc3_eswin, device_nb);

	mutex_lock(&eswin->lock);
	eswin->new_usb_role = USB_ROLE_DEVICE;
	mutex_unlock(&eswin->lock);
	if (!eswin->suspended)
		schedule_work(&eswin->otg_work);

	return NOTIFY_DONE;
}

static int dwc3_eswin_host_notifier(struct notifier_block *nb,
				    unsigned long event, void *ptr)
{
	struct dwc3_eswin *eswin = container_of(nb, struct dwc3_eswin, host_nb);

	mutex_lock(&eswin->lock);
	eswin->new_usb_role = USB_ROLE_HOST;
	mutex_unlock(&eswin->lock);
	if (!eswin->suspended)
		schedule_work(&eswin->otg_work);

	return NOTIFY_DONE;
}

static void dwc3_eswin_otg_extcon_evt_work(struct work_struct *work)
{
	struct dwc3_eswin *eswin =
		container_of(work, struct dwc3_eswin, otg_work);
	struct usb_role_switch *role_sw = eswin->dwc->role_sw;

	if (true == eswin->force_mode)
		return;
	mutex_lock(&eswin->lock);
	usb_role_switch_set_role(role_sw, eswin->new_usb_role);
	mutex_unlock(&eswin->lock);
}

static int dwc3_eswin_get_extcon_dev(struct dwc3_eswin *eswin)
{
	struct device *dev = eswin->dev;
	struct extcon_dev *edev;
	s32 ret = 0;

	if (device_property_present(dev, "extcon")) {
		edev = extcon_get_edev_by_phandle(dev, 0);
		if (IS_ERR(edev))
			return dev_err_probe(dev, PTR_ERR(edev),
					     "couldn't get extcon device\n");
		eswin->edev = edev;
		eswin->device_nb.notifier_call = dwc3_eswin_device_notifier;
		ret = devm_extcon_register_notifier(dev, edev, EXTCON_USB,
					     &eswin->device_nb);
		if (ret < 0)
			dev_err(dev, "failed to register notifier for USB\n");

		eswin->host_nb.notifier_call = dwc3_eswin_host_notifier;
		ret = devm_extcon_register_notifier(dev, edev, EXTCON_USB_HOST,
					     &eswin->host_nb);
		if (ret < 0)
			dev_err(dev, "failed to register notifier for USB-HOST\n");
	}

	return 0;
}

static int dwc3_eswin_deassert(struct dwc3_eswin *eswin)
{
	int rc;

	if (eswin->vaux_rst) {
		rc = reset_control_deassert(eswin->vaux_rst);
		if (rc) {
			dev_err(eswin->dev, "Failed to deassert reset: %d\n", rc);
			return rc;
		}
	}

	return 0;
}

static int dwc3_eswin_assert(struct dwc3_eswin *eswin)
{
	int rc;

	if (eswin->vaux_rst) {
		rc = reset_control_assert(eswin->vaux_rst);
		if (rc) {
			dev_err(eswin->dev, "Failed to assert reset: %d\n", rc);
			return rc;
		}
	}

	return 0;
}

static int dwc_usb_clk_init(struct device *dev)
{
	struct regmap *regmap;
	u32 hsp_usb_bus;
	u32 hsp_usb_axi_lp;
	u32 hsp_usb_vbus_freq;
	u32 hsp_usb_mpll;
	u32 args[4];

	regmap = syscon_regmap_lookup_by_phandle_args(dev->of_node,
						"eswin,hsp_sp_csr",
						4, args);
	if (IS_ERR(regmap))
		return dev_err_probe(dev, PTR_ERR(regmap),
				     "No hsp_sp_csr phandle specified\n");

	hsp_usb_bus       = args[0];
	hsp_usb_axi_lp    = args[1];
	hsp_usb_vbus_freq = args[2];
	hsp_usb_mpll      = args[3];

	/*
	 * usb1 clock init
	 * ref clock is 24M, below need to be set to satisfy usb phy requirement(125M)
	 */
	regmap_write(regmap, hsp_usb_vbus_freq, HSP_USB_VBUS_FSEL);
	regmap_write(regmap, hsp_usb_mpll, HSP_USB_MPLL_DEFAULT);
	/*
	 * reset usb core and usb phy
	 */
	regmap_write(regmap, hsp_usb_bus,
			     HSP_USB_BUS_FILTER_EN | HSP_USB_BUS_CLKEN_GM |
			     HSP_USB_BUS_CLKEN_GS | HSP_USB_BUS_SW_RST |
			     HSP_USB_BUS_CLK_EN);
	regmap_write(regmap, hsp_usb_axi_lp,
		     HSP_USB_AXI_LP_XM_CSYSREQ | HSP_USB_AXI_LP_XS_CSYSREQ);

	return 0;
}

static int dwc3_eswin_probe(struct platform_device *pdev)
{
	struct dwc3_eswin *eswin;
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node, *child;
	struct platform_device *child_pdev;
	int ret;
	int err_desc = 0;

	eswin = devm_kzalloc(dev, sizeof(*eswin), GFP_KERNEL);
	if (!eswin)
		return -ENOMEM;

	eswin->hub_gpio = devm_gpiod_get(dev, "hub-rst", GPIOD_OUT_HIGH);
	err_desc = IS_ERR(eswin->hub_gpio);
	if (!err_desc)
		gpiod_set_raw_value(eswin->hub_gpio, 1);

	eswin->dev = dev;
	eswin->force_mode = false;

	platform_set_drvdata(pdev, eswin);
	mutex_init(&eswin->lock);

	eswin->num_clks = devm_clk_bulk_get_all_enabled(dev, &eswin->clks);
	if (eswin->num_clks < 0)
		return dev_err_probe(dev, eswin->num_clks,
				     "failed to get usb clocks\n");

	eswin->vaux_rst = devm_reset_control_get(dev, "vaux");
	if (IS_ERR(eswin->vaux_rst))
		return dev_err_probe(dev, PTR_ERR(eswin->vaux_rst),
					 "Failed to asic0_rst handle\n");

	dwc3_eswin_deassert(eswin);
	dwc_usb_clk_init(dev);

	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);
	ret = pm_runtime_get_sync(dev);
	if (ret < 0) {
		dev_err(dev, "get_sync failed with err %d\n", ret);
		goto err1;
	}

	child = of_get_child_by_name(np, "dwc3");
	if (!child) {
		dev_err(dev, "failed to find dwc3 core node\n");
		ret = -ENODEV;
		goto err1;
	}
	/* Allocate and initialize the core */
	ret = of_platform_populate(np, NULL, NULL, dev);
	if (ret) {
		dev_err(dev, "failed to create dwc3 core\n");
		goto err1;
	}

	INIT_WORK(&eswin->otg_work, dwc3_eswin_otg_extcon_evt_work);
	child_pdev = of_find_device_by_node(child);
	if (!child_pdev) {
		dev_err(dev, "failed to find dwc3 core device\n");
		ret = -ENODEV;
		goto err2;
	}
	eswin->dwc = platform_get_drvdata(child_pdev);
	if (!eswin->dwc) {
		dev_err(dev, "failed to get drvdata dwc3\n");
		ret = -EPROBE_DEFER;
		goto err2;
	}
	eswin->child_dev = &child_pdev->dev;
	ret = dwc3_eswin_get_extcon_dev(eswin);
	if (ret < 0)
		dev_err(dev, "couldn't get extcon device: %d\n", ret);

	ret = sysfs_create_group(&dev->kobj, &dwc3_eswin_attr_group);
	if (ret)
		dev_err(dev, "failed to create sysfs group: %d\n", ret);

	return ret;
err2:
	cancel_work_sync(&eswin->otg_work);
	of_platform_depopulate(dev);

err1:
	pm_runtime_put_sync(dev);
	pm_runtime_disable(dev);
	dwc3_eswin_assert(eswin);

	return ret;
}

static void dwc3_eswin_remove(struct platform_device *pdev)
{
	struct dwc3_eswin *eswin = platform_get_drvdata(pdev);
	struct device *dev = &pdev->dev;

	cancel_work_sync(&eswin->otg_work);

	sysfs_remove_group(&dev->kobj, &dwc3_eswin_attr_group);

	/* Restore hcd state before unregistering xhci */
	if (eswin->edev && !eswin->connected) {
		struct usb_hcd *hcd = dev_get_drvdata(&eswin->dwc->xhci->dev);

		pm_runtime_get_sync(dev);

		/*
		 * The xhci code does not expect that HCDs have been removed.
		 * It will unconditionally call usb_remove_hcd() when the xhci
		 * driver is unloaded in of_platform_depopulate(). This results
		 * in a crash if the HCDs were already removed. To avoid this
		 * crash, add the HCDs here as dummy operation.
		 * This code should be removed after pm runtime support
		 * has been added to xhci.
		 */
		if (hcd->state == HC_STATE_HALT) {
			usb_add_hcd(hcd, hcd->irq, IRQF_SHARED);
			usb_add_hcd(hcd->shared_hcd, hcd->irq, IRQF_SHARED);
		}
	}

	of_platform_depopulate(dev);

	pm_runtime_put_sync(dev);
	pm_runtime_disable(dev);

	dwc3_eswin_assert(eswin);
}

#ifdef CONFIG_PM
static int dwc3_eswin_runtime_suspend(struct device *dev)
{
	struct dwc3_eswin *eswin = dev_get_drvdata(dev);
	clk_bulk_disable_unprepare(eswin->num_clks, eswin->clks);

	device_init_wakeup(dev, false);

	return 0;
}

static int dwc3_eswin_runtime_resume(struct device *dev)
{
	struct dwc3_eswin *eswin = dev_get_drvdata(dev);
	int ret;

	ret = clk_bulk_prepare_enable(eswin->num_clks, eswin->clks);
		if (ret) {
			dev_err(dev, "failed to enable clocks: %d\n", ret);
			return ret;
	}

	device_init_wakeup(dev, true);

	return 0;
}

static int __maybe_unused dwc3_eswin_suspend(struct device *dev)
{
	struct dwc3_eswin *eswin = dev_get_drvdata(dev);
	struct dwc3 *dwc = eswin->dwc;

	eswin->suspended = true;
	cancel_work_sync(&eswin->otg_work);

	/*
	 * The flag of is_phy_on is only true if
	 * the DWC3 is in Host mode.
	 */
	if (eswin->is_phy_on) {
		phy_power_off(dwc->usb2_generic_phy[0]);

		/*
		 * If link state is Rx.Detect, it means that
		 * no usb device is connecting with the DWC3
		 * Host, and need to power off the USB3 PHY.
		 */
		dwc->link_state = dwc3_gadget_get_link_state(dwc);
		if (dwc->link_state == DWC3_LINK_STATE_RX_DET)
			phy_power_off(dwc->usb3_generic_phy[0]);
	}

	return 0;
}

static int __maybe_unused dwc3_eswin_resume(struct device *dev)
{
	struct dwc3_eswin *eswin = dev_get_drvdata(dev);
	struct dwc3 *dwc = eswin->dwc;

	eswin->suspended = false;

	if (eswin->is_phy_on) {
		phy_power_on(dwc->usb2_generic_phy[0]);

		if (dwc->link_state == DWC3_LINK_STATE_RX_DET)
			phy_power_on(dwc->usb3_generic_phy[0]);
	}

	if (eswin->edev)
		schedule_work(&eswin->otg_work);

	return 0;
}

static const struct dev_pm_ops dwc3_eswin_dev_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(dwc3_eswin_suspend, dwc3_eswin_resume)
		SET_RUNTIME_PM_OPS(dwc3_eswin_runtime_suspend,
				   dwc3_eswin_runtime_resume, NULL)
};

#define DEV_PM_OPS (&dwc3_eswin_dev_pm_ops)
#else
#define DEV_PM_OPS NULL
#endif /* CONFIG_PM */

static const struct of_device_id eswin_dwc3_match[] = {
	{
		.compatible = "eswin,eic7700-dwc3",
	}, {

	}
};

MODULE_DEVICE_TABLE(of, eswin_dwc3_match);

static struct platform_driver dwc3_eswin_driver = {
	.probe = dwc3_eswin_probe,
	.remove = dwc3_eswin_remove,
	.driver = {
		.name = "eic7700-dwc3",
		.pm = DEV_PM_OPS,
		.of_match_table = eswin_dwc3_match,
	},
};

module_platform_driver(dwc3_eswin_driver);

MODULE_AUTHOR("Wei Yang <yangwei1@eswincomputing.com");
MODULE_AUTHOR("Senchuan Zhang <zhangsenchuan@eswincomputing.com");
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("DesignWare USB3 ESWIN Glue Layer");
