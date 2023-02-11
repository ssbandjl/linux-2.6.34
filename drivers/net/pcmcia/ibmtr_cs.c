/*======================================================================

    A PCMCIA token-ring driver for IBM-based cards

    This driver supports the IBM PCMCIA Token-Ring Card.
    Written by Steve Kipisz, kipisz@vnet.ibm.com or
                             bungy@ibm.net

    Written 1995,1996.

    This code is based on pcnet_cs.c from David Hinds.
    
    V2.2.0 February 1999 - Mike Phillips phillim@amtrak.com

    Linux V2.2.x presented significant changes to the underlying
    ibmtr.c code.  Mainly the code became a lot more organized and
    modular.

    This caused the old PCMCIA Token Ring driver to give up and go 
    home early. Instead of just patching the old code to make it 
    work, the PCMCIA code has been streamlined, updated and possibly
    improved.

    This code now only contains code required for the Card Services.
    All we do here is set the card up enough so that the real ibmtr.c
    driver can find it and work with it properly.

    i.e. We set up the io port, irq, mmio memory and shared ram
    memory.  This enables ibmtr_probe in ibmtr.c to find the card and
    configure it as though it was a normal ISA and/or PnP card.

    CHANGES

    v2.2.5 April 1999 Mike Phillips (phillim@amtrak.com)
    Obscure bug fix, required changed to ibmtr.c not ibmtr_cs.c
    
    v2.2.7 May 1999 Mike Phillips (phillim@amtrak.com)
    Updated to version 2.2.7 to match the first version of the kernel
    that the modification to ibmtr.c were incorporated into.
    
    v2.2.17 July 2000 Burt Silverman (burts@us.ibm.com)
    Address translation feature of PCMCIA controller is usable so
    memory windows can be placed in High memory (meaning above
    0xFFFFF.)

======================================================================*/

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/ptrace.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/timer.h>
#include <linux/module.h>
#include <linux/ethtool.h>
#include <linux/netdevice.h>
#include <linux/trdevice.h>
#include <linux/ibmtr.h>

#include <pcmcia/cs_types.h>
#include <pcmcia/cs.h>
#include <pcmcia/cistpl.h>
#include <pcmcia/ds.h>

#include <asm/uaccess.h>
#include <asm/io.h>
#include <asm/system.h>

#define PCMCIA
#include "../tokenring/ibmtr.c"


/*====================================================================*/

/* Parameters that can be set with 'insmod' */

/* MMIO base address */
static u_long mmiobase = 0xce000;

/* SRAM base address */
static u_long srambase = 0xd0000;

/* SRAM size 8,16,32,64 */
static u_long sramsize = 64;

/* Ringspeed 4,16 */
static int ringspeed = 16;

module_param(mmiobase, ulong, 0);
module_param(srambase, ulong, 0);
module_param(sramsize, ulong, 0);
module_param(ringspeed, int, 0);
MODULE_LICENSE("GPL");

/*====================================================================*/

static int ibmtr_config(struct pcmcia_device *link);
static void ibmtr_hw_setup(struct net_device *dev, u_int mmiobase);
static void ibmtr_release(struct pcmcia_device *link);
static void ibmtr_detach(struct pcmcia_device *p_dev);

/*====================================================================*/

typedef struct ibmtr_dev_t {
	struct pcmcia_device	*p_dev;
    struct net_device	*dev;
    dev_node_t          node;
    window_handle_t     sram_win_handle;
    struct tok_info	*ti;
} ibmtr_dev_t;

static void netdev_get_drvinfo(struct net_device *dev,
			       struct ethtool_drvinfo *info)
{
	strcpy(info->driver, "ibmtr_cs");
}

static const struct ethtool_ops netdev_ethtool_ops = {
	.get_drvinfo		= netdev_get_drvinfo,
};

static irqreturn_t ibmtr_interrupt(int irq, void *dev_id) {
	ibmtr_dev_t *info = dev_id;
	struct net_device *dev = info->dev;
	return tok_interrupt(irq, dev);
};

/*======================================================================

    ibmtr_attach() creates an "instance" of the driver, allocating
    local data structures for one device.  The device is registered
    with Card Services.

======================================================================*/

static int __devinit ibmtr_attach(struct pcmcia_device *link)
{
    ibmtr_dev_t *info;
    struct net_device *dev;

    dev_dbg(&link->dev, "ibmtr_attach()\n");

    /* Create new token-ring device */
    info = kzalloc(sizeof(*info), GFP_KERNEL);
    if (!info) return -ENOMEM;
    dev = alloc_trdev(sizeof(struct tok_info));
    if (!dev) {
	kfree(info);
	return -ENOMEM;
    }

    info->p_dev = link;
    link->priv = info;
    info->ti = netdev_priv(dev);

    link->io.Attributes1 = IO_DATA_PATH_WIDTH_8;
    link->io.NumPorts1 = 4;
    link->io.IOAddrLines = 16;
    link->irq.Attributes = IRQ_TYPE_EXCLUSIVE;
    link->irq.Handler = ibmtr_interrupt;
    link->conf.Attributes = CONF_ENABLE_IRQ;
    link->conf.IntType = INT_MEMORY_AND_IO;
    link->conf.Present = PRESENT_OPTION;

    info->dev = dev;

    SET_ETHTOOL_OPS(dev, &netdev_ethtool_ops);

    return ibmtr_config(link);
} /* ibmtr_attach */

/*======================================================================

    This deletes a driver "instance".  The device is de-registered
    with Card Services.  If it has been released, all local data
    structures are freed.  Otherwise, the structures will be freed
    when the device is released.

======================================================================*/

static void ibmtr_detach(struct pcmcia_device *link)
{
    struct ibmtr_dev_t *info = link->priv;
    struct net_device *dev = info->dev;
     struct tok_info *ti = netdev_priv(dev);

    dev_dbg(&link->dev, "ibmtr_detach\n");
    
    /* 
     * When the card removal interrupt hits tok_interrupt(), 
     * bail out early, so we don't crash the machine 
     */
    ti->sram_phys |= 1;

    if (link->dev_node)
	unregister_netdev(dev);
    
    del_timer_sync(&(ti->tr_timer));

    ibmtr_release(link);

    free_netdev(dev);
    kfree(info);
} /* ibmtr_detach */

/*======================================================================

    ibmtr_config() is scheduled to run after a CARD_INSERTION event
    is received, to configure the PCMCIA socket, and to make the
    token-ring device available to the system.

======================================================================*/

static int __devinit ibmtr_config(struct pcmcia_device *link)
{
    ibmtr_dev_t *info = link->priv;
    struct net_device *dev = info->dev;
    struct tok_info *ti = netdev_priv(dev);
    win_req_t req;
    memreq_t mem;
    int i, ret;

    dev_dbg(&link->dev, "ibmtr_config\n");

    link->conf.ConfigIndex = 0x61;

    /* Determine if this is PRIMARY or ALTERNATE. */

    /* Try PRIMARY card at 0xA20-0xA23 */
    link->io.BasePort1 = 0xA20;
    i = pcmcia_request_io(link, &link->io);
    if (i != 0) {
	/* Couldn't get 0xA20-0xA23.  Try ALTERNATE at 0xA24-0xA27. */
	link->io.BasePort1 = 0xA24;
	ret = pcmcia_request_io(link, &link->io);
	if (ret)
		goto failed;
    }
    dev->base_addr = link->io.BasePort1;

    ret = pcmcia_request_irq(link, &link->irq);
    if (ret)
	    goto failed;
    dev->irq = link->irq.AssignedIRQ;
    ti->irq = link->irq.AssignedIRQ;
    ti->global_int_enable=GLOBAL_INT_ENABLE+((dev->irq==9) ? 2 : dev->irq);

    /* Allocate the MMIO memory window */
    req.Attributes = WIN_DATA_WIDTH_16|WIN_MEMORY_TYPE_CM|WIN_ENABLE;
    req.Attributes |= WIN_USE_WAIT;
    req.Base = 0; 
    req.Size = 0x2000;
    req.AccessSpeed = 250;
    ret = pcmcia_request_window(link, &req, &link->win);
    if (ret)
	    goto failed;

    mem.CardOffset = mmiobase;
    mem.Page = 0;
    ret = pcmcia_map_mem_page(link, link->win, &mem);
    if (ret)
	    goto failed;
    ti->mmio = ioremap(req.Base, req.Size);

    /* Allocate the SRAM memory window */
    req.Attributes = WIN_DATA_WIDTH_16|WIN_MEMORY_TYPE_CM|WIN_ENABLE;
    req.Attributes |= WIN_USE_WAIT;
    req.Base = 0;
    req.Size = sramsize * 1024;
    req.AccessSpeed = 250;
    ret = pcmcia_request_window(link, &req, &info->sram_win_handle);
    if (ret)
	    goto failed;

    mem.CardOffset = srambase;
    mem.Page = 0;
    ret = pcmcia_map_mem_page(link, info->sram_win_handle, &mem);
    if (ret)
	    goto failed;

    ti->sram_base = mem.CardOffset >> 12;
    ti->sram_virt = ioremap(req.Base, req.Size);
    ti->sram_phys = req.Base;

    ret = pcmcia_request_configuration(link, &link->conf);
    if (ret)
	    goto failed;

    /*  Set up the Token-Ring Controller Configuration Register and
        turn on the card.  Check the "Local Area Network Credit Card
        Adapters Technical Reference"  SC30-3585 for this info.  */
    ibmtr_hw_setup(dev, mmiobase);

    link->dev_node = &info->node;
    SET_NETDEV_DEV(dev, &link->dev);

    i = ibmtr_probe_card(dev);
    if (i != 0) {
	printk(KERN_NOTICE "ibmtr_cs: register_netdev() failed\n");
	link->dev_node = NULL;
	goto failed;
    }

    strcpy(info->node.dev_name, dev->name);

    printk(KERN_INFO
	   "%s: port %#3lx, irq %d,  mmio %#5lx, sram %#5lx, hwaddr=%pM\n",
           dev->name, dev->base_addr, dev->irq,
	   (u_long)ti->mmio, (u_long)(ti->sram_base << 12),
	   dev->dev_addr);
    return 0;

failed:
    ibmtr_release(link);
    return -ENODEV;
} /* ibmtr_config */

/*======================================================================

    After a card is removed, ibmtr_release() will unregister the net
    device, and release the PCMCIA configuration.  If the device is
    still open, this will be postponed until it is closed.

======================================================================*/

static void ibmtr_release(struct pcmcia_device *link)
{
	ibmtr_dev_t *info = link->priv;
	struct net_device *dev = info->dev;

	dev_dbg(&link->dev, "ibmtr_release\n");

	if (link->win) {
		struct tok_info *ti = netdev_priv(dev);
		iounmap(ti->mmio);
		pcmcia_release_window(link, info->sram_win_handle);
	}
	pcmcia_disable_device(link);
}

static int ibmtr_suspend(struct pcmcia_device *link)
{
	ibmtr_dev_t *info = link->priv;
	struct net_device *dev = info->dev;

	if (link->open)
		netif_device_detach(dev);

	return 0;
}

static int __devinit ibmtr_resume(struct pcmcia_device *link)
{
	ibmtr_dev_t *info = link->priv;
	struct net_device *dev = info->dev;

	if (link->open) {
		ibmtr_probe(dev);	/* really? */
		netif_device_attach(dev);
	}

	return 0;
}


/*====================================================================*/

static void ibmtr_hw_setup(struct net_device *dev, u_int mmiobase)
{
    int i;

    /* Bizarre IBM behavior, there are 16 bits of information we
       need to set, but the card only allows us to send 4 bits at a 
       time.  For each byte sent to base_addr, bits 7-4 tell the
       card which part of the 16 bits we are setting, bits 3-0 contain 
       the actual information */

    /* First nibble provides 4 bits of mmio */
    i = (mmiobase >> 16) & 0x0F;
    outb(i, dev->base_addr);

    /* Second nibble provides 3 bits of mmio */
    i = 0x10 | ((mmiobase >> 12) & 0x0E);
    outb(i, dev->base_addr);

    /* Third nibble, hard-coded values */
    i = 0x26;
    outb(i, dev->base_addr);

    /* Fourth nibble sets shared ram page size */

    /* 8 = 00, 16 = 01, 32 = 10, 64 = 11 */          
    i = (sramsize >> 4) & 0x07;
    i = ((i == 4) ? 3 : i) << 2;
    i |= 0x30;

    if (ringspeed == 16)
	i |= 2;
    if (dev->base_addr == 0xA24)
	i |= 1;
    outb(i, dev->base_addr);

    /* 0x40 will release the card for use */
    outb(0x40, dev->base_addr);

    return;
}

static struct pcmcia_device_id ibmtr_ids[] = {
	PCMCIA_DEVICE_PROD_ID12("3Com", "TokenLink Velocity PC Card", 0x41240e5b, 0x82c3734e),
	PCMCIA_DEVICE_PROD_ID12("IBM", "TOKEN RING", 0xb569a6e5, 0xbf8eed47),
	PCMCIA_DEVICE_NULL,
};
MODULE_DEVICE_TABLE(pcmcia, ibmtr_ids);

static struct pcmcia_driver ibmtr_cs_driver = {
	.owner		= THIS_MODULE,
	.drv		= {
		.name	= "ibmtr_cs",
	},
	.probe		= ibmtr_attach,
	.remove		= ibmtr_detach,
	.id_table       = ibmtr_ids,
	.suspend	= ibmtr_suspend,
	.resume		= ibmtr_resume,
};

static int __init init_ibmtr_cs(void)
{
	return pcmcia_register_driver(&ibmtr_cs_driver);
}

static void __exit exit_ibmtr_cs(void)
{
	pcmcia_unregister_driver(&ibmtr_cs_driver);
}

module_init(init_ibmtr_cs);
module_exit(exit_ibmtr_cs);
