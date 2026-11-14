// SPDX-License-Identifier: GPL-2.0
/* NFS-only bridge for the existing disabled I2C1 node. No Flash/DT mutation.
 * Owns only TP SDA/SCL mux; LCD/TP reset remains with the display application.
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/io.h>
#include <linux/i2c.h>

static struct platform_device *bus;
static struct device_node *node;
static void __iomem *mux;
static u32 saved[2];

static void release_bus(void)
{
	if (bus) { platform_device_unregister(bus); bus = NULL; }
	if (mux) {
		writel(saved[0], mux); writel(saved[1], mux + 4);
		iounmap(mux); mux = NULL;
	}
	of_node_put(node); node = NULL;
}

static int __init ky50_i2c1_init(void)
{
	struct resource res[2] = {};
	struct i2c_adapter *existing;
	int rc, irq;
	existing = i2c_get_adapter(1);
	if (existing) { i2c_put_adapter(existing); return -EBUSY; }
	node = of_find_node_by_path("/i2c@04010000");
	if (!node) return -ENODEV;
	rc = of_address_to_resource(node, 0, &res[0]);
	if (rc) goto fail;
	irq = irq_of_parse_and_map(node, 0);
	if (!irq) { rc = -EINVAL; goto fail; }
	res[1].start = res[1].end = irq; res[1].flags = IORESOURCE_IRQ;
	mux = ioremap(0x030011a0, 8);
	if (!mux) { rc = -ENOMEM; goto fail; }
	saved[0] = readl(mux); saved[1] = readl(mux + 4);
	writel((saved[0] & ~7U) | 6, mux);
	writel((saved[1] & ~7U) | 6, mux + 4);
	bus = platform_device_alloc("i2c_designware", 1);
	if (!bus) { rc = -ENOMEM; goto fail; }
	bus->dev.of_node = of_node_get(node);
	rc = platform_device_add_resources(bus, res, ARRAY_SIZE(res));
	if (!rc) rc = platform_device_add(bus);
	if (rc) { platform_device_put(bus); bus = NULL; goto fail; }
	pr_info("ky50_i2c1: registered existing I2C1 with TP C20/C21 mux=6\n");
	return 0;
fail:
	release_bus(); return rc;
}
module_init(ky50_i2c1_init);
module_exit(release_bus);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("KY50 reversible NFS I2C1 bring-up");
