/*
 *  Driver for the Elgato 4k60 Pro mk.2 HDMI capture card.
 *
 *  Copyright (c) 2021 Steven Toth <stoth@kernellabs.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include "sc0710.h"

MODULE_DESCRIPTION("Driver for SC0710 based TV cards");
MODULE_AUTHOR("Steven Toth <stoth@kernellabs.com>");
MODULE_LICENSE("GPL");

/* 1 = Basic device statistics
 * 2 = PCIe register dump for entire device
 */
unsigned int procfs_verbosity = 3;
module_param(procfs_verbosity, int, 0644);
MODULE_PARM_DESC(procfs_verbosity, "enable procfs debugging via /proc/sc0710");

unsigned int thread_hdmi_active = 1;
module_param(thread_hdmi_active, int, 0644);
MODULE_PARM_DESC(thread_hdmi_active, "should HDMI thread run");

unsigned int thread_dma_active = 1;
module_param(thread_dma_active, int, 0644);
MODULE_PARM_DESC(thread_dma_active, "should dma thread run");

unsigned int thread_poll_interval = 100;
module_param(thread_poll_interval, int, 0644);
MODULE_PARM_DESC(thread_poll_interval, "have the kernel thread poll every N ms (def:100)");

unsigned int thread_dma_poll_interval_ms = 2;
module_param(thread_dma_poll_interval_ms, int, 0644);
MODULE_PARM_DESC(thread_dma_poll_interval_ms, "have the kernel thread poll dma every N ms (def:2)");

static unsigned int debug;
module_param(debug, int, 0644);
MODULE_PARM_DESC(debug, "enable debug messages");

unsigned int msi_enable = 0;
module_param_named(msi_enable, msi_enable, int, 0644);
MODULE_PARM_DESC(msi_enable, "use msi interrupts (def: 1)");

static unsigned int card[]  = {[0 ... (SC0710_MAXBOARDS - 1)] = UNSET };
module_param_array(card,  int, NULL, 0444);
MODULE_PARM_DESC(card, "card type");

#define dprintk(level, fmt, arg...)\
	do { if (debug >= level)\
		printk(KERN_DEBUG "%s: " fmt, dev->name, ## arg);\
	} while (0)

static unsigned int sc0710_devcount;
static DEFINE_MUTEX(devlist);
LIST_HEAD(sc0710_devlist);

void sc_andor(struct sc0710_dev *dev, int bar, u32 reg, u32 mask, u32 value)
{
	u32 newval = (readl(dev->lmmio[bar]+((reg)>>2)) & ~(mask)) | ((value) & (mask));
	writel(newval, dev->lmmio[bar]+((reg)>>2));
}

u32 sc_read(struct sc0710_dev *dev, int bar, u32 reg)
{
	return readl(dev->lmmio[bar] + (reg >> 2));
}

void sc_write(struct sc0710_dev *dev, int bar, u32 reg, u32 value)
{
	writel(value, dev->lmmio[bar] + (reg >>2));
}

void sc_set(struct sc0710_dev *dev, int bar, u32 reg, u32 bit)
{
	sc_andor(dev, bar, (reg), (bit), (bit));
}

void sc_clr(struct sc0710_dev *dev, int bar, u32 reg, u32 bit)
{
	sc_andor(dev, bar, (reg), (bit), 0);
}

static void sc0710_shutdown(struct sc0710_dev *dev)
{
	/* Disable all interrupts */
	/* Power down all function blocks */
}

static int get_resources(struct sc0710_dev *dev)
{
	if (request_mem_region(pci_resource_start(dev->pci, 0), pci_resource_len(dev->pci, 0), dev->name) == 0)
	{
		printk(KERN_ERR "%s: can't get var[0] memory @ 0x%llx\n",
			dev->name, (unsigned long long)pci_resource_start(dev->pci, 0));
		return -EBUSY;
	}

	if (request_mem_region(pci_resource_start(dev->pci, 1), pci_resource_len(dev->pci, 1), dev->name) == 0)
	{
		printk(KERN_ERR "%s: can't get bar[1] memory @ 0x%llx\n",
			dev->name, (unsigned long long)pci_resource_start(dev->pci, 1));
		return -EBUSY;
	}

	return 0;
}

static int sc0710_dev_setup(struct sc0710_dev *dev)
{
	int i;

	mutex_init(&dev->lock);
	mutex_init(&dev->signalMutex);

	atomic_inc(&dev->refcount);

	dev->nr = sc0710_devcount++;
	sprintf(dev->name, "sc0710[%d]", dev->nr);

	/* board config */
	dev->board = UNSET;
	if (card[dev->nr] < sc0710_bcount)
		dev->board = card[dev->nr];
	for (i = 0; UNSET == dev->board  &&  i < sc0710_idcount; i++)
		if (dev->pci->subsystem_vendor == sc0710_subids[i].subvendor &&
		    dev->pci->subsystem_device == sc0710_subids[i].subdevice)
			dev->board = sc0710_subids[i].card;
	if (UNSET == dev->board) {
		dev->board = SC0710_BOARD_UNKNOWN;
		sc0710_card_list(dev);
	}

	/* The keepalive thread needs a mutex */
	mutex_init(&dev->kthread_hdmi_lock);
	mutex_init(&dev->kthread_dma_lock);

	if (get_resources(dev) < 0) {
		printk(KERN_ERR "%s No more PCIe resources for "
		       "subsystem: %04x:%04x\n",
		       dev->name, dev->pci->subsystem_vendor,
		       dev->pci->subsystem_device);

		sc0710_devcount--;
		return -ENODEV;
	}

	/* PCIe stuff */
	dev->lmmio[0] = ioremap(pci_resource_start(dev->pci, 0), pci_resource_len(dev->pci, 0));
	dev->bmmio[0] = (u8 __iomem *)dev->lmmio[0];
	dev->lmmio[1] = ioremap(pci_resource_start(dev->pci, 1), pci_resource_len(dev->pci, 1));
	dev->bmmio[1] = (u8 __iomem *)dev->lmmio[1];

	printk(KERN_INFO "%s: subsystem: %04x:%04x, board: %s [card=%d,%s]\n",
	       dev->name, dev->pci->subsystem_vendor,
	       dev->pci->subsystem_device, sc0710_boards[dev->board].name,
	       dev->board, card[dev->nr] == dev->board ?
	       "insmod option" : "autodetected");

	return 0;
}

static void sc0710_dev_unregister(struct sc0710_dev *dev)
{
	release_mem_region(pci_resource_start(dev->pci, 0), pci_resource_len(dev->pci, 0));
	release_mem_region(pci_resource_start(dev->pci, 1), pci_resource_len(dev->pci, 1));

	if (!atomic_dec_and_test(&dev->refcount))
		return;

	sc0710_dma_channel_free(dev, 0);
	sc0710_dma_channel_free(dev, 1);

	iounmap(dev->lmmio[0]);
	iounmap(dev->lmmio[1]);
}

static irqreturn_t sc0710_irq(int irq, void *dev_id)
{
	//struct sc0710_dev *dev = dev_id;
	u32 irq_mask = 0, irq_status = 0, irq_clear = 0;
	int handled = 0;

//	irq_mask = tm_read(INTR_MSK);
//	irq_status = tm_read(INTR_STS);
//	irq_clear = tm_read(INTR_CLR);

	printk(KERN_ERR "irq: msk:%08x clr:%08x sts:%08x\n",
		irq_mask, irq_clear, irq_status);

	return IRQ_RETVAL(handled);
}

#ifdef CONFIG_PROC_FS
static int sc0710_proc_state_show(struct seq_file *m, void *v)
{
	struct sc0710_dev *dev;
	struct list_head *list;

	if (sc0710_devcount == 0)
		return 0;

	/* For each sc0710 in the system */
	list_for_each(list, &sc0710_devlist) {
		dev = list_entry(list, struct sc0710_dev, devlist);

		/* Show channel metrics */
		//sc0710_i2c_hdmi_status_dump(dev);
		sc0710_i2c_read_hdmi_status(dev);
		sc0710_i2c_read_status2(dev);
		sc0710_i2c_read_status3(dev);
		sc0710_i2c_read_procamp(dev);

		mutex_lock(&dev->signalMutex);
	        if (dev->locked) {
			seq_printf(m, "%s    HDMI: %dx%d%c (%dx%d)\n",
				dev->name,
				dev->width, dev->height,
				dev->interlaced ? 'i' : 'p',
				dev->pixelLineH, dev->pixelLineV);
		} else {
			seq_printf(m, "%s    HDMI: no signal\n", dev->name);
		}
		mutex_unlock(&dev->signalMutex);

		seq_printf(m, "%s procamp: brightness %d\n", dev->name, dev->brightness);
		seq_printf(m, "%s procamp: contrast %d\n", dev->name, dev->contrast);
		seq_printf(m, "%s procamp: saturation %d\n", dev->name, dev->saturation);
		seq_printf(m, "%s procamp: hue %d\n", dev->name, dev->hue);

	}

	return 0;
}

static int sc0710_proc_show(struct seq_file *m, void *v)
{
	struct sc0710_dev *dev;
	struct list_head *list;
	int i;
	u32 val;

	if (sc0710_devcount == 0)
		return 0;

	/* For each sc0710 in the system */
	list_for_each(list, &sc0710_devlist) {
		dev = list_entry(list, struct sc0710_dev, devlist);
		seq_printf(m, "%s = %p\n", dev->name, dev);

		if (procfs_verbosity & 0x02) {
			seq_printf(m, "Full PCI Register Dump:\n");
			for (i = 0; i < 0x100000; i += 4) {
				val = sc_read(dev, 0, i);
				if (val) {
					seq_printf(m, " 0x%04x = %08x\n", i, val);
				}
			}
		}

	}

	return 0;
}

static int sc0710_proc_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, sc0710_proc_show, NULL);
}

static int sc0710_proc_state_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, sc0710_proc_state_show, NULL);
}

static struct file_operations sc0710_proc_fops = {
	.open		= sc0710_proc_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static struct file_operations sc0710_proc_state_fops = {
	.open		= sc0710_proc_state_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int sc0710_proc_create(void)
{
	struct proc_dir_entry *pe;

	pe = proc_create("sc0710", S_IRUGO, NULL, &sc0710_proc_fops);
	if (!pe)
		return -ENOMEM;

	pe = proc_create("sc0710-state", S_IRUGO, NULL, &sc0710_proc_state_fops);
	if (!pe)
		return -ENOMEM;

	return 0;
}
#endif

static int sc0710_thread_dma_function(void *data)
{
	struct sc0710_dev *dev = data;
	int ret, i;

	dprintk(1, "%s() Started\n", __func__);

	msleep(2000);

	set_freezable();

	while (1) {
		msleep_interruptible(thread_dma_poll_interval_ms);

		if (kthread_should_stop())
			break;

		try_to_freeze();

		if (thread_dma_active == 0)
			continue;

		/* Other parts of the driver need to guarantee that
		 * various 'keep alives' aren't happening. We'll
		 * prevent race conditions by allowing the
		 * rest of the driver to dictate when
		 * this keepalives can occur.
		 */
		mutex_lock(&dev->kthread_dma_lock);

		mutex_unlock(&dev->kthread_dma_lock);

		for (i = 0; i < SC0710_MAX_CHANNELS; i++) {
			ret = sc0710_dma_channel_service(&dev->channel[i]);
		}
	}

	thread_dma_active = 0;
	dprintk(1, "%s() Stopped\n", __func__);
	return 0;
}

static int sc0710_thread_hdmi_function(void *data)
{
	struct sc0710_dev *dev = data;

	dprintk(1, "%s() Started\n", __func__);

	msleep(2000);

	set_freezable();

	while (1) {
		msleep_interruptible(thread_poll_interval);

		if (kthread_should_stop())
			break;

		try_to_freeze();

		if (thread_hdmi_active == 0)
			continue;
		/* Other parts of the driver need to guarantee that
		 * various 'keep alives' aren't happening. We'll
		 * prevent race conditions by allowing the
		 * rest of the driver to dictate when
		 * this keepalives can occur.
		 */
		mutex_lock(&dev->kthread_hdmi_lock);

		mutex_unlock(&dev->kthread_hdmi_lock);
	}

	thread_hdmi_active = 0;
	dprintk(1, "%s() Stopped\n", __func__);
	return 0;
}

static int sc0710_initdev(struct pci_dev *pci_dev,
	const struct pci_device_id *pci_id)
{
	struct sc0710_dev *dev;
	int err;

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (NULL == dev)
		return -ENOMEM;

	/* pci init */
	dev->pci = pci_dev;
	if (pci_enable_device(pci_dev)) {
		err = -EIO;
		goto fail_unreg;
	}

	/* print pci info */
	pci_read_config_byte(pci_dev, PCI_CLASS_REVISION, &dev->pci_rev);
	pci_read_config_byte(pci_dev, PCI_LATENCY_TIMER,  &dev->pci_lat);
	printk(KERN_INFO "sc0710 device found at %s, rev: %d, irq: %d, "
		"latency: %d\n",
		pci_name(pci_dev), dev->pci_rev, pci_dev->irq,
		dev->pci_lat);
	printk(KERN_INFO "sc0710 bar[0]: 0x%llx [0x%x bytes]\n",
		(unsigned long long)pci_resource_start(pci_dev, 0),
		(unsigned int)pci_resource_len(pci_dev, 0));
	printk(KERN_INFO "sc0710 bar[1]: 0x%llx [0x%x bytes]\n",
		(unsigned long long)pci_resource_start(pci_dev, 1),
		(unsigned int)pci_resource_len(pci_dev, 1));

	pci_set_master(pci_dev);
	if (!pci_dma_supported(pci_dev, 0xffffffff)) {
		printk("%s/0: Oops: no 32bit PCI DMA ???\n", dev->name);
		err = -EIO;
		goto fail_irq;
	}

	/* MAP the PCIe space, register i2c, program any PCIe quirks. */
	if (sc0710_dev_setup(dev) < 0) {
		err = -EINVAL;
		goto fail_unreg;
	}

	if ((msi_enable) && (!pci_enable_msi(pci_dev))) {
		printk("%s() MSI interrupts enabled\n", __func__);
		err = request_irq(pci_dev->irq, sc0710_irq,
			IRQF_DISABLED, dev->name, dev);
	} else {
		printk("%s() MSI interrupts disabled (driver default)\n", __func__);
		err = request_irq(pci_dev->irq, sc0710_irq,
			IRQF_SHARED | IRQF_DISABLED, dev->name, dev);
	}
	if (err < 0) {
		printk(KERN_ERR "%s: can't get IRQ %d\n",
		       dev->name, pci_dev->irq);
		goto fail_irq;
	}

	/* Card specific tweaks with subsystems etc */
	sc0710_card_setup(dev);

	pci_set_drvdata(pci_dev, dev);

	printk(KERN_INFO "sc0710 device at %s\n", pci_name(pci_dev));

	sc0710_dma_channel_alloc(dev, 0, CHDIR_INPUT, 0x1000, CHTYPE_VIDEO);
	sc0710_dma_channel_alloc(dev, 1, CHDIR_INPUT, 0x1100, CHTYPE_AUDIO);

	/* Put this in a global list so we can track multiple boards */
	mutex_lock(&devlist);
	list_add_tail(&dev->devlist, &sc0710_devlist);
	mutex_unlock(&devlist);

	dev->kthread_hdmi = kthread_run(sc0710_thread_hdmi_function, dev, "sc0710 hdmi");
	if (!dev->kthread_hdmi) {
		printk(KERN_ERR "%s() Failed to create "
			"hdmi kernel thread\n", __func__);
	} else
		dprintk(1, "%s() Created the HDMI thread\n", __func__);

	dev->kthread_dma = kthread_run(sc0710_thread_dma_function, dev, "sc0710 dma");
	if (!dev->kthread_dma) {
		printk(KERN_ERR "%s() Failed to create "
			"dma kernel thread\n", __func__);
	} else
		dprintk(1, "%s() Created the DMA thread\n", __func__);

	return 0;

fail_irq:
	sc0710_dev_unregister(dev);
fail_unreg:
	kfree(dev);
	return err;
}

static void sc0710_finidev(struct pci_dev *pci_dev)
{
	struct sc0710_dev *dev = pci_get_drvdata(pci_dev);
	int i = 0;

	if (dev->kthread_dma) {
		kthread_stop(dev->kthread_dma);
		dev->kthread_dma = NULL;

		while (thread_dma_active) {
			msleep(5);
			if (i++ > 3)
				break;
		}
	}

	if (dev->kthread_hdmi) {
		kthread_stop(dev->kthread_hdmi);
		dev->kthread_hdmi = NULL;

		while (thread_hdmi_active) {
			msleep(500);
			if (i++ > 8)
				break;
		}
	}

	sc0710_shutdown(dev);

	pci_disable_device(pci_dev);

	/* unregister stuff */
	free_irq(pci_dev->irq, dev);
	if (msi_enable)
		pci_disable_msi(pci_dev);

	mutex_lock(&devlist);
	list_del(&dev->devlist);
	mutex_unlock(&devlist);

	sc0710_dev_unregister(dev);

	kfree(dev);
}

static struct pci_device_id sc0710_pci_tbl[] = {
	{
		.vendor       = 0x12ab,
		.device       = 0x0710,
		.subvendor    = PCI_ANY_ID,
		.subdevice    = PCI_ANY_ID,
	} , {
		/* List terminator */
	}
};
MODULE_DEVICE_TABLE(pci, sc0710_pci_tbl);

static struct pci_driver sc0710_pci_driver = {
	.name     = "sc0710",
	.id_table = sc0710_pci_tbl,
	.probe    = sc0710_initdev,
	.remove   = sc0710_finidev,
	/* TODO */
	.suspend  = NULL,
	.resume   = NULL,
};

static int __init sc0710_init(void)
{
	printk(KERN_INFO "sc0710 driver version %d.%d.%d loaded\n",
	       (SC0710_VERSION_CODE >> 16) & 0xff,
	       (SC0710_VERSION_CODE >>  8) & 0xff,
	       SC0710_VERSION_CODE & 0xff);
#ifdef SNAPSHOT
	printk(KERN_INFO "sc0710: snapshot date %04d-%02d-%02d\n",
	       SNAPSHOT/10000, (SNAPSHOT/100)%100, SNAPSHOT%100);
#endif
#ifdef CONFIG_PROC_FS
	sc0710_proc_create();
#endif
	return pci_register_driver(&sc0710_pci_driver);
}

static void __exit sc0710_fini(void)
{
#ifdef CONFIG_PROC_FS
	remove_proc_entry("sc0710", NULL);
	remove_proc_entry("sc0710-state", NULL);
#endif
	pci_unregister_driver(&sc0710_pci_driver);
	printk(KERN_INFO "sc0710 driver unloaded\n");
}

module_init(sc0710_init);
module_exit(sc0710_fini);

