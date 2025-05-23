// SPDX-License-Identifier: GPL-2.0
/*
 * ESWIN PCIe root complex driver
 *
 * Copyright 2024, Beijing ESWIN Computing Technology Co., Ltd.. All rights reserved.
 *
 * Authors: Yu Ning <ningyu@eswincomputing.com>
 *          Senchuan Zhang <zhangsenchuan@eswincomputing.com>
 */
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/resource.h>
#include <linux/types.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/reset.h>
#include "pcie-designware.h"
#include <linux/pm_runtime.h>

struct eswin_pcie {
	struct dw_pcie pci;
	void __iomem *mgmt_base;
    struct clk_bulk_data *clks;
	struct reset_control *powerup_rst;
	struct reset_control *cfg_rst;
	struct reset_control *perst;

    int num_clks;
};

#define PCIE_PM_SEL_AUX_CLK BIT(16)
#define PCIEMGMT_APP_HOLD_PHY_RST BIT(6)
#define PCIEMGMT_APP_LTSSM_ENABLE BIT(5)
#define PCIEMGMT_DEVICE_TYPE_MASK 0xf

#define PCIEMGMT_CTRL0_OFFSET 0x0
#define PCIEMGMT_STATUS0_OFFSET 0x100

#define PCIE_TYPE_DEV_VEND_ID 0x0
#define PCIE_DSP_PF0_MSI_CAP 0x50
#define PCIE_NEXT_CAP_PTR 0x70
#define DEVICE_CONTROL_DEVICE_STATUS 0x78

#define PCIE_MSI_MULTIPLE_MSG_32 (0x5 << 17)
#define PCIE_MSI_MULTIPLE_MSG_MASK (0x7 << 17)

#define PCIEMGMT_LINKUP_STATE_VALIDATE ((0x11 << 2) | 0x3)
#define PCIEMGMT_LINKUP_STATE_MASK 0xff

static int eswin_pcie_start_link(struct dw_pcie *pci)
{
	struct device *dev = pci->dev;
	struct eswin_pcie *pcie = dev_get_drvdata(dev);
	u32 val;

	/* Enable LTSSM */
	val = readl_relaxed(pcie->mgmt_base + PCIEMGMT_CTRL0_OFFSET);
	val |= PCIEMGMT_APP_LTSSM_ENABLE;
	writel_relaxed(val, pcie->mgmt_base + PCIEMGMT_CTRL0_OFFSET);
	return 0;
}

static int eswin_pcie_link_up(struct dw_pcie *pci)
{
	struct device *dev = pci->dev;
	struct eswin_pcie *pcie = dev_get_drvdata(dev);
	u32 val;

	val = readl_relaxed(pcie->mgmt_base + PCIEMGMT_STATUS0_OFFSET);
	if ((val & PCIEMGMT_LINKUP_STATE_MASK) ==
		PCIEMGMT_LINKUP_STATE_VALIDATE)
		return 1;
	else
		return 0;
}

static int eswin_pcie_power_on(struct eswin_pcie *pcie)
{
	int ret = 0;

	/* pciet_cfg_rstn */
	ret = reset_control_reset(pcie->cfg_rst);
	if (ret) {
		dev_err(pcie->pci.dev, "cfg signal is invalid");
		return ret;
	}

	/* pciet_powerup_rstn */
	ret = reset_control_reset(pcie->powerup_rst);
	if (ret) {
		dev_err(pcie->pci.dev, "powerup signal is invalid");
		return ret;
	}

	return ret;
}

static int eswin_pcie_power_off(struct eswin_pcie *eswin_pcie)
{
	reset_control_assert(eswin_pcie->perst);
	reset_control_assert(eswin_pcie->powerup_rst);
	reset_control_assert(eswin_pcie->cfg_rst);

	return 0;
}

static int eswin_pcie_host_init(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct eswin_pcie *pcie = dev_get_drvdata(pci->dev);
	int ret;
	u32 val;

	ret = clk_bulk_prepare_enable(pcie->num_clks, pcie->clks);
	if (ret)
		return dev_err_probe(pci->dev, ret, "failed to enable clocks\n");

	ret = eswin_pcie_power_on(pcie);
	if (ret)
		return ret;

	/* set device type : rc */
	val = readl_relaxed(pcie->mgmt_base + PCIEMGMT_CTRL0_OFFSET);
	val &= 0xfffffff0;
	writel_relaxed(val | 0x4, pcie->mgmt_base + PCIEMGMT_CTRL0_OFFSET);

	ret = reset_control_assert(pcie->perst);
	if (ret) {
		dev_err(pci->dev, "reset control assert signal is invalid");
		return ret;
	}

	msleep(100);
	ret = reset_control_deassert(pcie->perst);
	if (ret) {
		dev_err(pci->dev, "reset control deassert signal is invalid");
		return ret;
	}

	/* app_hold_phy_rst */
	val = readl_relaxed(pcie->mgmt_base + PCIEMGMT_CTRL0_OFFSET);
	val &= ~(0x40);
	writel_relaxed(val, pcie->mgmt_base + PCIEMGMT_CTRL0_OFFSET);

	/* wait pm_sel_aux_clk to 0 */
	for (ret = 50; ret > 0; ret--) {
		val = readl_relaxed(pcie->mgmt_base + PCIEMGMT_STATUS0_OFFSET);
		if (!(val & PCIE_PM_SEL_AUX_CLK))
			break;
		msleep(2);
	}

	if (!ret) {
		dev_info(pcie->pci.dev, "No clock exist.\n");
		eswin_pcie_power_off(pcie);
		clk_bulk_disable_unprepare(pcie->num_clks, pcie->clks);
		return -ENODEV;
	}

	/* config eswin vendor id and eic7700 device id */
	dw_pcie_writel_dbi(pci, PCIE_TYPE_DEV_VEND_ID, 0x20301fe1);

	/* lane fix config, real driver NOT need, default x4 */
	val = dw_pcie_readl_dbi(pci, PCIE_PORT_MULTI_LANE_CTRL);
	val &= 0xffffff80;
	val |= 0x44;
	dw_pcie_writel_dbi(pci, PCIE_PORT_MULTI_LANE_CTRL, val);

	val = dw_pcie_readl_dbi(pci, DEVICE_CONTROL_DEVICE_STATUS);
	val &= ~(0x7 << 5);
	val |= (0x2 << 5);
	dw_pcie_writel_dbi(pci, DEVICE_CONTROL_DEVICE_STATUS, val);

	/*  config support 32 msi vectors */
	val = dw_pcie_readl_dbi(pci, PCIE_DSP_PF0_MSI_CAP);
	val &= ~PCIE_MSI_MULTIPLE_MSG_MASK;
	val |= PCIE_MSI_MULTIPLE_MSG_32;
	dw_pcie_writel_dbi(pci, PCIE_DSP_PF0_MSI_CAP, val);

	/* disable msix cap */
	val = dw_pcie_readl_dbi(pci, PCIE_NEXT_CAP_PTR);
	val &= 0xffff00ff;
	dw_pcie_writel_dbi(pci, PCIE_NEXT_CAP_PTR, val);

	return 0;
}

static const struct dw_pcie_host_ops eswin_pcie_host_ops = {
	.init = eswin_pcie_host_init,
};

static const struct dw_pcie_ops dw_pcie_ops = {
	.start_link = eswin_pcie_start_link,
	.link_up = eswin_pcie_link_up,
};

static int eswin_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct dw_pcie *pci;
	struct eswin_pcie *pcie;
	int err;

	pcie = devm_kzalloc(dev, sizeof(*pcie), GFP_KERNEL);
	if (!pcie)
		return -ENOMEM;
	pci = &pcie->pci;
	pci->dev = dev;
	pci->ops = &dw_pcie_ops;
	pci->pp.ops = &eswin_pcie_host_ops;

	/* SiFive specific region: mgmt */
	pcie->mgmt_base = devm_platform_ioremap_resource_byname(pdev, "mgmt");
	if (IS_ERR(pcie->mgmt_base))
		return PTR_ERR(pcie->mgmt_base);

	/* Fetch clocks */
	pcie->num_clks = devm_clk_bulk_get_all(dev, &pcie->clks);
	if (pcie->num_clks < 0)
		return dev_err_probe(dev, pcie->num_clks,
				     "failed to get pcie clocks\n");

	/* Fetch reset */
	pcie->powerup_rst = devm_reset_control_get_optional(&pdev->dev, "powerup");
	if (IS_ERR_OR_NULL(pcie->powerup_rst))
		dev_err_probe(dev, PTR_ERR(pcie->powerup_rst),
			     "unable to get powerup reset\n");

	pcie->cfg_rst = devm_reset_control_get_optional(&pdev->dev, "cfg");
	if (IS_ERR_OR_NULL(pcie->cfg_rst))
		dev_err_probe(dev, PTR_ERR(pcie->cfg_rst),
			     "unable to get cfg reset\n");

	pcie->perst = devm_reset_control_get_optional(&pdev->dev, "pwren");
	if (IS_ERR_OR_NULL(pcie->perst))
		dev_err_probe(dev, PTR_ERR(pcie->perst),
			     "unable to get perst\n");

	platform_set_drvdata(pdev, pcie);

	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);
	err = pm_runtime_get_sync(dev);
	if (err < 0) {
		dev_err(dev, "pm_runtime_get_sync failed: %d\n", err);
		goto pm_runtime_put;
	}

	return dw_pcie_host_init(&pci->pp);

pm_runtime_put:
	pm_runtime_put_sync(dev);
	pm_runtime_disable(dev);

	return err;
}

static void eswin_pcie_remove(struct platform_device *pdev)
{
	struct eswin_pcie *pcie = platform_get_drvdata(pdev);

	dw_pcie_host_deinit(&pcie->pci.pp);
	pm_runtime_put_sync(&pdev->dev);
	pm_runtime_disable(&pdev->dev);

	eswin_pcie_power_off(pcie);
	clk_bulk_disable_unprepare(pcie->num_clks, pcie->clks);

}

static void eswin_pcie_shutdown(struct platform_device *pdev)
{
	struct eswin_pcie *pcie = platform_get_drvdata(pdev);

	/* Bring down link, so bootloader gets clean state in case of reboot */
	reset_control_assert(pcie->perst);
}

static int eswin_pcie_suspend(struct device *dev)
{
	struct eswin_pcie *pcie = dev_get_drvdata(dev);

	if (!pm_runtime_status_suspended(dev))
		clk_bulk_disable_unprepare(pcie->num_clks, pcie->clks);

	return 0;
}

static int eswin_pcie_resume(struct device *dev)
{
	int ret;

	struct eswin_pcie *pcie = dev_get_drvdata(dev);
	if (!pm_runtime_status_suspended(dev)){
		ret = clk_bulk_prepare_enable(pcie->num_clks, pcie->clks);
		if (ret)
			return dev_err_probe(dev, ret, "failed to enable clocks\n");
	}
	return 0;
}

static int eswin_pcie_runtime_suspend(struct device *dev)
{
	struct eswin_pcie *pcie = dev_get_drvdata(dev);

	clk_bulk_disable_unprepare(pcie->num_clks, pcie->clks);

	return 0;
}

static int eswin_pcie_runtime_resume(struct device *dev)
{
	struct eswin_pcie *pcie = dev_get_drvdata(dev);

	return clk_bulk_prepare_enable(pcie->num_clks, pcie->clks);
}

static const struct dev_pm_ops eswin_pcie_pm_ops = {
	RUNTIME_PM_OPS(eswin_pcie_runtime_suspend, eswin_pcie_runtime_resume,
		     NULL)
	NOIRQ_SYSTEM_SLEEP_PM_OPS(eswin_pcie_suspend, eswin_pcie_resume)
};

static const struct of_device_id eswin_pcie_of_match[] = {
	{
		.compatible = "eswin,eic7700-pcie",
	},
	{},
};

static struct platform_driver eswin_pcie_driver = {
	.driver = {
			.name = "eic7700-pcie",
			.of_match_table = eswin_pcie_of_match,
			.suppress_bind_attrs = true,
			.pm = &eswin_pcie_pm_ops,
	},
	.probe = eswin_pcie_probe,
	.remove = eswin_pcie_remove,
	.shutdown = eswin_pcie_shutdown,
};

module_platform_driver(eswin_pcie_driver);

MODULE_DEVICE_TABLE(of, eswin_pcie_of_match);
MODULE_DESCRIPTION("PCIe host controller driver for eic7700 SoCs");
MODULE_AUTHOR("Yu Ning <ningyu@eswincomputing.com>");
MODULE_AUTHOR("Senchuan Zhang <zhangsenchuan@eswincomputing.com>");
MODULE_LICENSE("GPL v2");
