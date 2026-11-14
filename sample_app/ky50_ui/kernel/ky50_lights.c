// SPDX-License-Identifier: GPL-2.0
/* KY50 V1.0 sheets 4/7: LED_PWM = GPIOA19, ILED-PWM = GPIOA8.
 * Both are active high. Use GPIO mux 3, never enable the PWM peripheral.
 * A single owner holds both lights; closing its fd turns both outputs off.
 */
#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/gpio/driver.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/machine.h>
#include <linux/mutex.h>
#include <linux/io.h>
#include <linux/uaccess.h>

static struct gpio_desc *lights[2]; /* white, infrared */
static void __iomem *mux[2];
static u32 saved_mux[2];
static bool opened, enabled[2];
static DEFINE_MUTEX(lock);

static void lights_off(void)
{
    int i;
    for (i = 0; i < 2; i++) {
        gpiod_set_value_cansleep(lights[i], 0);
        enabled[i] = false;
    }
}

static int lights_open(struct inode *inode, struct file *file)
{
    int rc = 0;
    mutex_lock(&lock);
    if (opened)
        rc = -EBUSY;
    else {
        lights_off();
        opened = true;
    }
    mutex_unlock(&lock);
    return rc;
}

static int lights_close(struct inode *inode, struct file *file)
{
    mutex_lock(&lock);
    lights_off();
    opened = false;
    mutex_unlock(&lock);
    pr_info("ky50_lights: both OFF (owner closed)\n");
    return 0;
}

/* Two-byte commands: w0/w1 for white, i0/i1 for infrared. */
static ssize_t lights_write(struct file *file, const char __user *buf,
                           size_t n, loff_t *pos)
{
    char command[2];
    int index, on;
    if (n != sizeof(command))
        return -EINVAL;
    if (copy_from_user(command, buf, sizeof(command)))
        return -EFAULT;
    if ((command[0] != 'w' && command[0] != 'i') ||
        (command[1] != '0' && command[1] != '1'))
        return -EINVAL;
    index = command[0] == 'i';
    on = command[1] == '1';
    mutex_lock(&lock);
    gpiod_set_value_cansleep(lights[index], on);
    enabled[index] = on;
    mutex_unlock(&lock);
    pr_info("ky50_lights: %s %s\n", index ? "IR" : "WHITE", on ? "ON" : "OFF");
    return n;
}

/* Reports commanded GPIO states, white then infrared, for diagnostics. */
static ssize_t lights_read(struct file *file, char __user *buf,
                          size_t n, loff_t *pos)
{
    char state[3];
    mutex_lock(&lock);
    state[0] = '0' + enabled[0];
    state[1] = '0' + enabled[1];
    state[2] = '\n';
    mutex_unlock(&lock);
    return simple_read_from_buffer(buf, n, pos, state, sizeof(state));
}

static const struct file_operations ops = {
    .owner = THIS_MODULE, .open = lights_open, .release = lights_close,
    .read = lights_read, .write = lights_write, .llseek = no_llseek,
};
static struct miscdevice dev = {
    .minor = MISC_DYNAMIC_MINOR, .name = "ky50-lights", .fops = &ops, .mode = 0600,
};
static int match(struct gpio_chip *chip, void *data)
{
    return !strcmp(chip->label, data);
}

static int __init lights_init(void)
{
    static const unsigned int pins[] = {19, 8};
    static const unsigned long addresses[] = {0x03001064, 0x03001020};
    static const char * const names[] = {"ky50-white-light", "ky50-ir-light"};
    struct gpio_chip *chip = gpiochip_find("3020000.gpio", match);
    int i, rc;
    if (!chip)
        return -ENODEV;
    /* Acquire both lines and mappings before changing either pin's mux. */
    for (i = 0; i < 2; i++) {
        lights[i] = gpiochip_request_own_desc(chip, pins[i], names[i],
                                            GPIO_ACTIVE_HIGH, GPIOD_OUT_LOW);
        if (IS_ERR(lights[i])) {
            rc = PTR_ERR(lights[i]);
            lights[i] = NULL;
            goto fail;
        }
        mux[i] = ioremap(addresses[i], 4);
        if (!mux[i]) {
            rc = -ENOMEM;
            goto fail;
        }
        saved_mux[i] = readl(mux[i]);
    }
    for (i = 0; i < 2; i++)
        writel((saved_mux[i] & ~7U) | 3, mux[i]);
    rc = misc_register(&dev);
    if (!rc) {
        pr_info("ky50_lights: WHITE GPIOA19, IR GPIOA8, high=ON, both OFF\n");
        return 0;
    }
    for (i = 0; i < 2; i++)
        writel(saved_mux[i], mux[i]);
fail:
    for (i = 0; i < 2; i++) {
        if (mux[i]) iounmap(mux[i]);
        if (lights[i]) gpiochip_free_own_desc(lights[i]);
    }
    return rc;
}

static void __exit lights_exit(void)
{
    int i;
    misc_deregister(&dev);
    lights_off();
    for (i = 0; i < 2; i++) {
        writel(saved_mux[i], mux[i]);
        iounmap(mux[i]);
        gpiochip_free_own_desc(lights[i]);
    }
}
module_init(lights_init);
module_exit(lights_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("KY50 white and infrared factory light GPIO switches");
