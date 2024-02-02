
#include <linux/platform_device.h>

static struct platform_device secgpio_dvs_device = {
	.name	= "secgpio_dvs",
	.id		= -1,
};

static struct platform_device *secgpio_dvs_devices[] __initdata = {
	&secgpio_dvs_device,
};

static int __init secgpio_dvs_device_init(void)
{
	return platform_add_devices(
		secgpio_dvs_devices, ARRAY_SIZE(secgpio_dvs_devices));
}
arch_initcall(secgpio_dvs_device_init);


