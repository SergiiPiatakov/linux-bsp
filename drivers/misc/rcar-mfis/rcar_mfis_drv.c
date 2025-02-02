// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* rcar_mfis_drv.c -- R-Car MFIS
*
* Copyright (C) 2022 Renesas Electronics Corporation
*/

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/slab.h>     /* kmalloc() */
#include <linux/notifier.h>

#include "rcar_mfis_drv.h"
#include <misc/rcar-mfis/rcar_mfis_public.h>

#define IICR(n) (0x0400 + n * 0x8)
#define EICR(n) (0x0404 + n * 0x8)
#define IMBR(n) (0x0440 + n * 0x4)
#define EMBR(n) (0x0460 + n * 0x4)

static struct rcar_mfis_dev* rcmfis_dev = NULL;

static irqreturn_t mfis_irq_handler(int irq, void *data)
{
	u32 value = 0;
	struct rcar_mfis_msg msg;
	struct platform_device *pdev = rcmfis_dev->pdev;
	struct device *dev = &pdev->dev;

	struct rcar_mfis_ch* rcar_mfis_ch = (struct rcar_mfis_ch*)data;
	unsigned int ch = rcar_mfis_ch->id;

	dev_dbg(dev, "interrupt! ch %d\n", ch);

	value = rcar_mfis_reg_read(rcmfis_dev, EICR(ch));
	if (value & 0x1)
	{
		msg.mbr = rcar_mfis_reg_read(rcmfis_dev, EMBR(ch));
		msg.icr = value >> 1; //get rid of EIR bit

		atomic_notifier_call_chain(&rcar_mfis_ch->notifier_head, msg.icr, rcar_mfis_ch->notifier_data);

		/* clear interrupt flag */
		rcar_mfis_reg_write(rcmfis_dev, EICR(ch), value & (~0x1));

		return IRQ_HANDLED;
	}

	return IRQ_NONE;
}

static struct rcar_mfis_ch* rcar_mfis_channel_get(unsigned int channel)
{
	struct rcar_mfis_ch *rcar_mfis_ch = NULL;
	int i;

	for (i=0; i<NUM_MFIS_CHANNELS; i++) {
		if (rcmfis_dev->channels[i].initialized && rcmfis_dev->channels[i].id == channel) {
			rcar_mfis_ch = &rcmfis_dev->channels[i];
			break;
		}
	}
	return rcar_mfis_ch;
}


/****** Exported functions ******/

int rcar_mfis_trigger_interrupt(int channel, struct rcar_mfis_msg msg)
{
	struct rcar_mfis_ch* rcar_mfis_ch;
	int ret = 0;
	u32 icr;

	rcar_mfis_ch = rcar_mfis_channel_get(channel);
	if (!rcar_mfis_ch) {
		return -EINVAL;
	}

	/* Check whether the CR7 is still processing a previous interrupt */
	icr = rcar_mfis_reg_read(rcmfis_dev, IICR(channel));
	if (icr & 0x1) {
		return -EBUSY;
	}

	rcar_mfis_reg_write(rcmfis_dev, IMBR(channel), msg.mbr);
	rcar_mfis_reg_write(rcmfis_dev, IICR(channel), (msg.icr << 1) | 1);

	return ret;
}
EXPORT_SYMBOL(rcar_mfis_trigger_interrupt);

int rcar_mfis_register_notifier(int channel, struct notifier_block *nb, void *data)
{
	struct rcar_mfis_ch* rcar_mfis_ch;
	struct atomic_notifier_head *nh;

	rcar_mfis_ch = rcar_mfis_channel_get(channel);
	if (!rcar_mfis_ch) {
		return -EINVAL;
	}
    
    if(NULL == rcmfis_dev){
        printk("mfis driver not propoerly loaded. Check device tree for renesas,mfis\n");
        return -ENXIO;
    }
    

	rcar_mfis_ch->notifier_data = data;

	nh = &rcar_mfis_ch->notifier_head;
	return atomic_notifier_chain_register(nh, nb);
}
EXPORT_SYMBOL(rcar_mfis_register_notifier);

int rcar_mfis_unregister_notifier(int channel, struct notifier_block *nb)
{
	struct rcar_mfis_ch* rcar_mfis_ch;
	struct atomic_notifier_head *nh;

	rcar_mfis_ch = rcar_mfis_channel_get(channel);
	if (!rcar_mfis_ch) {
		return -EINVAL;
	}

    if(NULL == rcmfis_dev){
        printk("mfis driver not propoerly loaded. Check device tree for renesas,mfis\n");
        return -ENXIO;
    }

	nh = &rcar_mfis_ch->notifier_head;
	return atomic_notifier_chain_unregister(nh, nb);
}
EXPORT_SYMBOL(rcar_mfis_unregister_notifier);

static int rcar_mfis_probe(struct platform_device *pdev)
{
	int ret = 0;
	u32 __iomem *mmio_base;
	struct resource *res = NULL;
	struct resource *irq = NULL;
	struct device *dev = &pdev->dev;
	int i = 0;
	u32 num_mfis_channels = 0;
	u32 value = 0;

	dev_dbg(dev, "R-Car MFIS probe start\n");

	num_mfis_channels = of_property_count_elems_of_size(dev->of_node, "renesas,mfis-channels", sizeof(u32));
	if (value == -EINVAL) {
		dev_err(dev, "can't find renesas,mfis-channels property\n");
		return value;
	}

	/* Allocate device struct */
	rcmfis_dev = kzalloc(sizeof(struct rcar_mfis_dev), GFP_KERNEL);
	if (!rcmfis_dev) {
		dev_err(dev, "Failed to allocate memory for rcar_mfis struct.\n");
		return -ENOMEM;
	}

	rcmfis_dev->pdev = pdev;

	/* Map MFIS registers */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	mmio_base = (u32 __iomem *)devm_ioremap(dev, res->start, resource_size(res));
	if (IS_ERR(mmio_base)) {
		dev_err(dev, "Failed to remap MFIS registers.\n");
		ret = PTR_ERR(mmio_base);
		goto free_mfis_dev;
	}

	rcmfis_dev->mmio_base = mmio_base;

	for (i=0; i<num_mfis_channels; i++)
	{
		struct rcar_mfis_ch *mfis_ch;

		ret = of_property_read_u32_index(dev->of_node, "renesas,mfis-channels", i, &value);
		if (ret) {
			dev_warn(dev, "can't read value at index %d in renesas,mfis-channels property. Skipping.\n", i);
			continue;
		}
		else if (value < 0 || value >= NUM_MFIS_CHANNELS) {
			dev_warn(dev, "value at index %d in renesas,mfis-channels property is out of range. Skipping.\n", i);
			continue;
		}

		mfis_ch = &rcmfis_dev->channels[value];
		if (mfis_ch->initialized) {
			dev_warn(dev, "mfis channel %d is already initialized. Skipping.\n", value);
			continue;
		}

		mfis_ch->id = value;

		ATOMIC_INIT_NOTIFIER_HEAD(&mfis_ch->notifier_head);

		/* Get IRQ resource */
		irq = platform_get_resource(pdev, IORESOURCE_IRQ, mfis_ch->id);
		if (!irq) {
			dev_err(dev, "missing IRQ for channel %d. Skipping.\n", mfis_ch->id);
			continue;
		}

		ret = devm_request_irq(dev, irq->start, mfis_irq_handler,
				IRQF_SHARED, dev_name(dev), mfis_ch);
		if (ret < 0) {
			dev_err(dev, "failed to request IRQ for channel %d. Skipping.\n", mfis_ch->id);
			continue;
		}

		mfis_ch->initialized = 1;
		dev_dbg(dev, "channel %d initialized (%s)\n", mfis_ch->id, irq->name);
	}

	dev_dbg(dev, "R-Car MFIS probe done\n");
	return 0;

free_mfis_dev:
	kfree(rcmfis_dev);
	return ret;
}

static int rcar_mfis_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;

	dev_dbg(dev, "R-Car MFIS remove\n");
	kfree(rcmfis_dev);

	return 0;
}

static const struct of_device_id rcar_mfis_of_match[] = {
	{ .compatible = "renesas,mfis" },
	{ },
};
MODULE_DEVICE_TABLE(of, rcar_mfis_of_match);

static struct platform_driver rcar_mfis_driver = {
	.probe      = rcar_mfis_probe,
	.remove     = rcar_mfis_remove,
	.driver     = {
		.name   = "rcar_mfis",
		.of_match_table = rcar_mfis_of_match,
	},
};

static int __init rcar_mfis_init(void)
{
	return platform_driver_register(&rcar_mfis_driver);
}
core_initcall(rcar_mfis_init);

static void __exit rcar_mfis_exit(void)
{
	platform_driver_unregister(&rcar_mfis_driver);
}
module_exit(rcar_mfis_exit);

MODULE_LICENSE("Dual MIT/GPL");
