// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Remote processor machine-specific module for R-Car Gen3 - Cortex-R7
 *
 * Copyright (C) 2022 Renesas Electronics
 *
 */

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/notifier.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/remoteproc.h>
#include <linux/delay.h>

#include "remoteproc_internal.h"

#include <misc/rcar-mfis/rcar_mfis_public.h>
#define MFIS_CHANNEL 0 //use this mfis channel to trigger interrupts

static char *rcar_cr7_fw_name;
module_param(rcar_cr7_fw_name, charp, S_IRUGO);
MODULE_PARM_DESC(rcar_cr7_fw_name,
		 "Name of CR7 firmware file in /lib/firmware (if not specified defaults to 'rproc-cr7-fw')");

#define RST_BASE                0xE6160000
#define RST_CR7BAR_OFFSET       0x00000070

#define SYSC_BASE               0xE6180000
#define SYSC_PWRSR7_OFFSET      0x00000240
#define SYSC_PWRONCR7_OFFSET    0x0000024C

#define APMU_CR7PSTR            0XE6153040

#define CPG_BASE                0xE6150000
#define CPG_WPCR_OFFSET         0x00000904
#define CPG_WPR_OFFSET          0x00000900

#define MSSR_BASE               0xE6150000 //same as CPG
#define MSSR_SRCR2_OFFSET       0x000000B0
#define MSSR_SRSTCLR2_OFFSET    0x00000948

#define CR7_BASE                0xF0100000
#define CR7_WBPWRCTLR_OFFSET    0x00000F80
#define CR7_WBCTLR_OFFSET       0x00000000


/**
 * struct rcar_cr7_rproc - rcar_cr7 remote processor instance state
 * @rproc: rproc handle
 * @workqueue: work queue list
 * @cr7_already_running: indicate Cortex-R7 core is already running or not
 * @mem_va: virtual memory address
 * @mem_da: device memory address
 * @mem_len: length of internal memory regions data
 */
struct rcar_cr7_rproc {
	struct rproc *rproc;
	struct work_struct workqueue;
	bool cr7_already_running;
	void __iomem *mem_va;
	phys_addr_t mem_da;
	u64 mem_len;
};

/**
 * handle_event() - inbound virtqueue message workqueue function
 * @work: work queue list
 *
 * This callback is registered with the R-Car MFIS atomic notifier
 * chain and is called every time the remote processor (Cortex-R7)
 * wants to notify us of pending messages available.
 */
static void handle_event(struct work_struct *work)
{
        struct rcar_cr7_rproc *rrproc =
                container_of(work, struct rcar_cr7_rproc, workqueue);

	/* Process incoming buffers on all our vrings */
        rproc_vq_interrupt(rrproc->rproc, 0);
        rproc_vq_interrupt(rrproc->rproc, 1);
}

/**
 * cr7_interrupt_cb()
 * @self: R-Car Cortex CR7 notifer block
 * @action: type of interrupt request
 * @data: message data
 *
 * This callback is registered with the R-Car MFIS atomic notifier
 * chain and is called every time the remote processor (Cortex-R7)
 * wants to notify us of pending messages available.
 */
static int cr7_interrupt_cb(struct notifier_block *self, unsigned long action, void *data)
{
	struct rcar_cr7_rproc *rrproc = (struct rcar_cr7_rproc *)data;
	struct device *dev = rrproc->rproc->dev.parent;

	dev_dbg(dev, "%s\n", __FUNCTION__);

	schedule_work(&rrproc->workqueue);

	return NOTIFY_DONE;
}

static struct notifier_block rcar_cr7_notifier_block = {
	.notifier_call = cr7_interrupt_cb,
};

static int is_cr7_running(void)
{
	void __iomem *mmio_apmu_base;
	u32 regval;

	/* CR7 Power Status Register (CR7PSTR) */
	mmio_apmu_base = ioremap(APMU_CR7PSTR, 4);
	regval = ioread32(mmio_apmu_base);
	iounmap(mmio_apmu_base);

	if ((regval & 3) == 0)
		return 1;

	return 0;
}

static int rcar_cr7_rproc_start(struct rproc *rproc)
{
	struct device *dev = rproc->dev.parent;
	//struct rcar_cr7_rproc *rrproc = (struct rcar_cr7_rproc *)rproc->priv;

	void __iomem *mmio_cpg_base;
	void __iomem *mmio_rst_base;
	void __iomem *mmio_sysc_base;
	void __iomem *mmio_apmu_base;
	u32 regval;


	dev_dbg(dev, "%s\n", __FUNCTION__);

	/* If the CR7 is already running, leave it alone */
	if (is_cr7_running())
		return 0;

	// CR7 Power-Up Sequence (Sec. 5A.3.3 R-Car Gen3 HW User Manual)
	//////// 1. clear write protection for CPG register
	mmio_cpg_base = ioremap(CPG_BASE, 4);
	// Clear CPG Write Protect (CPGWPCR.WPE)
	iowrite32(0x5a5affff, (mmio_cpg_base + CPG_WPR_OFFSET));
	iowrite32(0xa5a50000, (mmio_cpg_base + CPG_WPCR_OFFSET));

	//////// 2. Set boot address
	// Get Reset Controller node (RST)
	mmio_rst_base = ioremap(RST_BASE, 4);
	if (rproc->bootaddr & ~0xfffc0000)
		dev_warn(dev, "Boot address (0x%llx) not aligned!\n", rproc->bootaddr);
	regval = (rproc->bootaddr & 0xfffc0000); //Set Boot Addr
	regval |= 0x10; //Enable BAR
	iowrite32(regval, (mmio_rst_base + RST_CR7BAR_OFFSET));

	//////// 3. CR7 Power-On set
	// Get System Controller node (SYSC)
	mmio_sysc_base = ioremap(SYSC_BASE, 4);
	regval = 0x1; //Start power-resume sequence
	iowrite32(regval, (mmio_sysc_base + SYSC_PWRONCR7_OFFSET));


	//////// 4. Wait until Power-On
	// Get Advanced Power Management Unit (APMU)
	// CR7 Power Status Register (CR7PSTR)
	mmio_apmu_base = ioremap(APMU_CR7PSTR, 4);
	do {
		regval = ioread32(mmio_apmu_base) & 0x3; //APMU_CR7PSTR
		regval |= ioread32(mmio_sysc_base+SYSC_PWRSR7_OFFSET) &0x10;
	} while (regval != 0x10);

	//////// 5. Clear Soft Reset bit
	// MSSR Arm Realtime Core Reset Set
	iowrite32((1<<22), (mmio_cpg_base + MSSR_SRSTCLR2_OFFSET));

	iounmap(mmio_cpg_base);
	iounmap(mmio_rst_base);
	iounmap(mmio_sysc_base);
	iounmap(mmio_apmu_base);

	dev_dbg(dev, "%s: Reset released.\n", __FUNCTION__);
	return 0;
}

static int rcar_cr7_rproc_stop(struct rproc *rproc)
{
	struct device *dev = rproc->dev.parent;
	//struct rcar_cr7_rproc *rrproc = (struct rcar_cr7_rproc *)rproc->priv;

	dev_dbg(dev, "%s\n", __FUNCTION__);

	/* Implement me */

	return 0;
}

/* kick a virtqueue */
static void rcar_cr7_rproc_kick(struct rproc *rproc, int vqid)
{
	int ret;
	struct device *dev = rproc->dev.parent;
	struct rcar_mfis_msg msg;
	// struct rcar_cr7_rproc *rrproc = (struct rcar_cr7_rproc *)rproc->priv;
	unsigned int n_tries = 3;

	dev_dbg(dev, "%s\n", __FUNCTION__);

	msg.icr = vqid;
	msg.mbr = 0;

	do {
	    ret = rcar_mfis_trigger_interrupt(MFIS_CHANNEL, msg);
	    if (ret)
		udelay(500);

	} while (ret && n_tries--);

	if (ret) {
		dev_dbg(dev, "%s failed\n", __FUNCTION__);
	}
}

static void *rcar_cr7_da_to_va(struct rproc *rproc, u64 da, size_t len)
{
	struct rcar_cr7_rproc *rrproc = rproc->priv;
	int offset;

	offset = da - rrproc->mem_da;
	if (offset < 0 || offset + len > rrproc->mem_len)
		return NULL;

	return (void __force *)rrproc->mem_va + offset;
}

static int rcar_cr7_rproc_elf_load_segments(struct rproc *rproc,
					    const struct firmware *fw)
{
	struct rcar_cr7_rproc *rrproc = rproc->priv;

	/* If the CR7 is already running, do not download the image */
	if (!rrproc->cr7_already_running) {
		return rproc_elf_load_segments(rproc, fw);
	}

	return 0;
}

static int rcar_cr7_rproc_parse_fw(struct rproc *rproc, const struct firmware *fw)
{
	return rproc_elf_load_rsc_table(rproc, fw);
}

static struct resource_table *
rcar_cr7_rproc_elf_find_loaded_rsc_table(struct rproc *rproc,
				         const struct firmware *fw)
{
	return rproc_elf_find_loaded_rsc_table(rproc, fw);
}

static int rcar_cr7_rproc_elf_sanity_check(struct rproc *rproc,
					   const struct firmware *fw)
{
	return rproc_elf_sanity_check(rproc, fw);
}

static u64 rcar_cr7_rproc_elf_get_boot_addr(struct rproc *rproc,
					    const struct firmware *fw)
{
	return rproc_elf_get_boot_addr(rproc, fw);
}

static const struct rproc_ops rcar_cr7_rproc_ops = {
	.start = rcar_cr7_rproc_start,
	.stop = rcar_cr7_rproc_stop,
	.kick = rcar_cr7_rproc_kick,
	.da_to_va = rcar_cr7_da_to_va,
	.load = rcar_cr7_rproc_elf_load_segments,
	.parse_fw = rcar_cr7_rproc_parse_fw,
	.find_loaded_rsc_table = rcar_cr7_rproc_elf_find_loaded_rsc_table,
	.sanity_check = rcar_cr7_rproc_elf_sanity_check,
	.get_boot_addr = rcar_cr7_rproc_elf_get_boot_addr,
};

static int rcar_cr7_rproc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rcar_cr7_rproc *rrproc;
	struct device_node *np = dev->of_node;
	struct device_node *node;
	struct resource res;
	struct rproc *rproc;
	int ret;

	rproc = rproc_alloc(dev, "cr7", &rcar_cr7_rproc_ops, rcar_cr7_fw_name, sizeof(*rrproc));
	if (!rproc) {
		return -ENOMEM;
	}

	rrproc = rproc->priv;
	rrproc->rproc = rproc;
	rproc->has_iommu = false;

	node = of_parse_phandle(np, "memory-region", 0);
	if (!node) {
		dev_err(dev, "no memory-region specified\n");
		ret = -EINVAL;
		goto free_rproc;
	}

	ret = of_address_to_resource(node, 0, &res);
	if (ret) {
		dev_err(dev, "unable to resolve memory region\n");
		goto free_rproc;
	}

	rrproc->mem_da = res.start;
	rrproc->mem_len = resource_size(&res);
	rrproc->mem_va = devm_ioremap_wc(dev, rrproc->mem_da, rrproc->mem_len);
	if (IS_ERR(rrproc->mem_va)) {
		dev_err(dev, "unable to map memory region: %pa+%llx\n",
			&res.start, rrproc->mem_len);
		ret = PTR_ERR(rrproc->mem_va);
		goto free_rproc;
	}

	/* If the CR7 is already running, don't download new firmware.
	 * Note that we still require the matching elf firmware to be present in
	 * the file system, as we use that to extract the resource table info. */
	if (is_cr7_running())
		rrproc->cr7_already_running = true;

	INIT_WORK(&rrproc->workqueue, handle_event);

	platform_set_drvdata(pdev, rrproc);

	ret = rcar_mfis_register_notifier(MFIS_CHANNEL, &rcar_cr7_notifier_block, rrproc);
	if (ret) {
		dev_err(dev, "cannot register notifier on mfis channel %d\n", MFIS_CHANNEL);
		goto free_rproc;
	}

	ret = rproc_add(rproc);
	if (ret) {
		dev_err(dev, "rproc_add failed: %d\n", ret);
		goto unregister_notifier;
	}

	return 0;

unregister_notifier:
	rcar_mfis_unregister_notifier(MFIS_CHANNEL, &rcar_cr7_notifier_block);
	flush_work(&rrproc->workqueue);
free_rproc:
	rproc_free(rproc);
	return ret;
}

static int rcar_cr7_rproc_remove(struct platform_device *pdev)
{
	struct rcar_cr7_rproc *rrproc = platform_get_drvdata(pdev);
	struct rproc *rproc = rrproc->rproc;

	rcar_mfis_unregister_notifier(MFIS_CHANNEL, &rcar_cr7_notifier_block);
	flush_work(&rrproc->workqueue);
	rproc_del(rproc);
	rproc_free(rproc);

	return 0;
}

static const struct of_device_id rcar_cr7_rproc_of_match[] = {
	{ .compatible = "renesas,rcar-cr7", },
	{ },
};
MODULE_DEVICE_TABLE(of, rcar_cr7_rproc_of_match);

static struct platform_driver rcar_cr7_rproc_driver = {
	.probe = rcar_cr7_rproc_probe,
	.remove = rcar_cr7_rproc_remove,
	.driver = {
		.name = "rcar-cr7-rproc",
		.of_match_table = of_match_ptr(rcar_cr7_rproc_of_match),
	},
};

module_platform_driver(rcar_cr7_rproc_driver);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("RCAR_CR7 Remote Processor control driver");
