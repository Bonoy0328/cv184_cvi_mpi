// SPDX-License-Identifier: GPL-2.0
/* Compatibility bridge for the shipped KY50 kernel's codec_dac driver.
 * DT marks mute-gpio-l active-low, but the legacy driver ignores those flags.
 * During this module's lifetime it hands GPIOA15 control to this bridge by
 * disabling only its left amp GPIO slot. The codec retains its GPIO request;
 * no second gpio_request, unbind, register pokes or audio-driver replacement.
 * Load before opening PCM; do not unbind codec_dac while this module is loaded.
 * Must be built against the EXACT codec_common.h used by the running kernel.
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/miscdevice.h>
#include <linux/mutex.h>
#include <linux/gpio.h>
#include <linux/gpio/driver.h>
#include <linux/gpio/machine.h>
#include <linux/gpio/consumer.h>
#include <linux/io.h>
#include <linux/of_gpio.h>
#include <linux/fs.h>
#include "codec_common.h"
static struct device *codec;
static struct dac_obj *dac;
static int pin=-1,saved_level;
static void __iomem *lineout_mux;
static u32 saved_mux;
static struct gpio_desc *lineout;
static atomic_t opened=ATOMIC_INIT(0);
static int amp_open(struct inode *inode,struct file *file)
{
    (void)inode;(void)file;
    if(atomic_cmpxchg(&opened,0,1)) return -EBUSY;
    gpio_set_value(pin,0);return 0;
}
static int amp_close(struct inode *inode,struct file *file)
{
    (void)inode;(void)file;gpio_set_value(pin,1);atomic_set(&opened,0);return 0;
}
static const struct file_operations ops={.owner=THIS_MODULE,.open=amp_open,
    .release=amp_close,.llseek=no_llseek};
static struct miscdevice dev={.minor=MISC_DYNAMIC_MINOR,.name="ky50-amp",.fops=&ops,.mode=0600};
static int match_chip(struct gpio_chip *chip,void *data) { return !strcmp(chip->label,data); }
static int __init amp_init(void)
{
    enum of_gpio_flags flags;
    struct gpio_chip *chip;
    int rc;
    codec=bus_find_device_by_name(&platform_bus_type,NULL,"300a000.dac");
    if(!codec) return -ENODEV;
    device_lock(codec);
    if(!codec->driver||strcmp(codec->driver->name,"codec_dac")) { rc=-ENODEV;goto fail; }
    dac=dev_get_drvdata(codec);
    pin=of_get_named_gpio_flags(codec->of_node,"mute-gpio-l",0,&flags);
    if(!dac||pin<0||dac->amp_gpio_l!=pin||!(flags&OF_GPIO_ACTIVE_LOW)) { rc=-EINVAL;goto fail; }
    chip=gpiod_to_chip(gpio_to_desc(pin));
    if(!chip||strcmp(chip->label,"3020000.gpio")||pin!=chip->base+15||dac->id!=0) {
        rc=-EINVAL;goto fail;
    }
    /* LINEOUT is PAD_AUD_AOUTR / C24. AOUTL/C25 belongs to LCD reset. */
    chip=gpiochip_find("3022000.gpio",match_chip);
    if(!chip) { rc=-ENODEV;goto fail; }
    lineout=gpiochip_request_own_desc(chip,24,"ky50-lineout",GPIO_ACTIVE_HIGH,GPIOD_IN);
    if(IS_ERR(lineout)) { rc=PTR_ERR(lineout);lineout=NULL;goto fail; }
    lineout_mux=ioremap(0x030011cc,4);
    if(!lineout_mux) { rc=-ENOMEM;goto fail; }
    saved_mux=readl(lineout_mux);writel(saved_mux&~7U,lineout_mux);
    saved_level=gpio_get_value(pin);
    WRITE_ONCE(dac->amp_gpio_l,(u32)-EINVAL);
    gpio_set_value(pin,1);
    rc=misc_register(&dev);
    if(rc) { WRITE_ONCE(dac->amp_gpio_l,(u32)pin);gpio_set_value(pin,saved_level);goto fail; }
    device_unlock(codec);
    pr_info("ky50_amp: codec GPIOA15 handed off; LM4871 low=play high=shutdown\n");return 0;
fail:
    if(lineout_mux) { writel(saved_mux,lineout_mux);iounmap(lineout_mux);lineout_mux=NULL; }
    if(lineout) { gpiochip_free_own_desc(lineout);lineout=NULL; }
    device_unlock(codec);put_device(codec);codec=NULL;return rc;
}
static void __exit amp_exit(void)
{
    misc_deregister(&dev);
    device_lock(codec);
    gpio_set_value(pin,saved_level);WRITE_ONCE(dac->amp_gpio_l,(u32)pin);
    device_unlock(codec);put_device(codec);
    writel(saved_mux,lineout_mux);iounmap(lineout_mux);gpiochip_free_own_desc(lineout);
}
module_init(amp_init);module_exit(amp_exit);
MODULE_LICENSE("GPL");MODULE_DESCRIPTION("KY50 shipped-kernel LM4871 polarity compatibility bridge");
