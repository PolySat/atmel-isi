/*
 * Copyright (c) 2007 Atmel Corporation
 *
 * Based on the bttv driver for Bt848 with respective copyright holders
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
//#define DEBUG			12
//#define VERBOSE		12
//#define VERBOSE_DEBUG		12

#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/ioctl.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/version.h>
#include <linux/videodev2.h>
#include <linux/wait.h>

#define ALLOCSIZE 4*1024*1024
// PAGE_SHIFT PAGE_SIZE I

static int __init atmel_isi_init_module(void)
{
   printk("Order: %d\n", get_order(ALLOCSIZE));
   return -ENOMEM;
}


static void __exit atmel_isi_exit(void)
{
}


module_init(atmel_isi_init_module);
module_exit(atmel_isi_exit);

MODULE_AUTHOR("Lars Häring <lharing@atmel.com>");
MODULE_DESCRIPTION("The V4L2 driver for atmel Linux");
MODULE_LICENSE("GPL");
MODULE_SUPPORTED_DEVICE("video");
