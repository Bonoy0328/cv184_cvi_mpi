// SPDX-License-Identifier: GPL-2.0
/* KY50 schematic LOCK = GPIOA21, active high. Bounded, retriggerable pulse.
 * Kernel timer closes the contact even when userspace stalls/crashes. */
#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/gpio/driver.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/machine.h>
#include <linux/workqueue.h>
#include <linux/mutex.h>
#include <linux/io.h>
#include <linux/uaccess.h>
static struct gpio_desc *relay;
static void __iomem *mux;
static u32 saved_mux;
static DEFINE_MUTEX(lock);
static bool opened,active;
static unsigned long deadline;
static void off(struct work_struct *work);
static DECLARE_DELAYED_WORK(timeout,off);
static void off(struct work_struct *work)
{
    unsigned long now;
    (void)work;mutex_lock(&lock);
    now=jiffies;
    /* A retrigger can race a timer already queued on this mutex. */
    if(active&&time_before(now,deadline)) {
        mod_delayed_work(system_wq,&timeout,deadline-now);
    } else if(active) {
        gpiod_set_value_cansleep(relay,0);active=false;
        pr_info("ky50_relay: OFF (pulse timer)\n");
    }
    mutex_unlock(&lock);
}
static int relay_open(struct inode *inode,struct file *file)
{
    int rc=0;(void)inode;(void)file;mutex_lock(&lock);
    if(opened) rc=-EBUSY;else opened=true;
    mutex_unlock(&lock);return rc;
}
static int relay_close(struct inode *inode,struct file *file)
{
    (void)inode;(void)file;cancel_delayed_work_sync(&timeout);
    mutex_lock(&lock);gpiod_set_value_cansleep(relay,0);active=false;opened=false;
    mutex_unlock(&lock);return 0;
}
static ssize_t relay_write(struct file *f,const char __user *buf,size_t n,loff_t *pos)
{
    char command[16];unsigned int seconds=5;bool legacy;int rc=n;(void)f;(void)pos;
    if(!n||n>=sizeof(command))return -EINVAL;
    if(copy_from_user(command,buf,n))return -EFAULT;
    command[n]=0;legacy=n==1&&command[0]=='1';
    if(!legacy) {
        size_t i;
        if(n<3||command[0]!='P'||command[1]!=' ')return -EINVAL;
        /* kstrtouint accepts a final newline. Reject embedded NUL/trailing junk. */
        for(i=2;i<n;i++)if((command[i]<'0'||command[i]>'9')&&!(i==n-1&&command[i]=='\n'))return -EINVAL;
        if(kstrtouint(command+2,10,&seconds)||seconds<1||seconds>60)return -EINVAL;
    }
    mutex_lock(&lock);
    if(legacy&&active)rc=-EBUSY;
    else {
        deadline=jiffies+msecs_to_jiffies(seconds*1000);
        if(!active)gpiod_set_value_cansleep(relay,1);
        active=true;
        mod_delayed_work(system_wq,&timeout,msecs_to_jiffies(seconds*1000));
        pr_info("ky50_relay: ON for %u seconds\n",seconds);
    }
    mutex_unlock(&lock);return rc;
}
static const struct file_operations ops={.owner=THIS_MODULE,.open=relay_open,
    .release=relay_close,.write=relay_write,.llseek=no_llseek};
static struct miscdevice dev={.minor=MISC_DYNAMIC_MINOR,.name="ky50-relay",.fops=&ops,.mode=0600};
static int match(struct gpio_chip *chip,void *data) { return !strcmp(chip->label,data); }
static int __init relay_init(void)
{
    struct gpio_chip *chip=gpiochip_find("3020000.gpio",match);
    int rc;
    if(!chip) return -ENODEV;
    relay=gpiochip_request_own_desc(chip,21,"ky50-relay",GPIO_ACTIVE_HIGH,GPIOD_OUT_LOW);
    if(IS_ERR(relay)) return PTR_ERR(relay);
    mux=ioremap(0x03001048,4);
    if(!mux) { rc=-ENOMEM;goto fail; }
    saved_mux=readl(mux);writel((saved_mux&~7U)|3,mux);
    rc=misc_register(&dev);if(!rc) return 0;
    writel(saved_mux,mux);iounmap(mux);mux=NULL;
fail: gpiochip_free_own_desc(relay);return rc;
}
static void __exit relay_exit(void)
{
    misc_deregister(&dev);cancel_delayed_work_sync(&timeout);gpiod_set_value_cansleep(relay,0);
    writel(saved_mux,mux);iounmap(mux);gpiochip_free_own_desc(relay);
}
module_init(relay_init);module_exit(relay_exit);
MODULE_VERSION("2.0");
MODULE_LICENSE("GPL");MODULE_DESCRIPTION("KY50 relay kernel-timed configurable pulse, 1-60 seconds");
