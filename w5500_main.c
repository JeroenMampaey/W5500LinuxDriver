#include <linux/module.h>
#include <linux/of.h>
#include "w5500_netdev.h"
#include <linux/interrupt.h>
#include <linux/gpio/consumer.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("JeroenMampaey");
MODULE_DESCRIPTION("A simple network device for a W5500 and Raspberry-Pi");
MODULE_VERSION("1.0");

/*
  Table which matches against the device tree "compatible" property
*/
static const struct of_device_id w5500_dt_ids[] = {
    {.compatible = "wiznet,w5500"},
    {}
};
MODULE_DEVICE_TABLE(of, w5500_dt_ids);

/*
  Function called when a w5500 compatible device is found
*/
static int w5500_probe(struct spi_device *spi){
    printk("w5500_driver - w5500 compatible device found\n");

    spi->mode = SPI_MODE_0;
    int result = spi_setup(spi);
    if(result){
        printk("w5500_driver - Error %d setting up SPI\n", result);
        return result;
    }

    struct gpio_desc* int_pin = gpiod_get(&spi->dev, "int", GPIOD_IN);
	if(IS_ERR(int_pin)){
		printk("w5500_driver - Error could not setup the GPIO\n");
		return -1 * IS_ERR(int_pin);
	}
    
    struct net_device* w5500_netdev = alloc_netdev_mqs(sizeof(struct w5500_netdev_priv), "w5500", NET_NAME_ENUM, w5500_netdev_setup, 1, 1);
    if(!w5500_netdev){
        printk("w5500_driver - Failed to allocate network device\n");
        gpiod_put(int_pin);
        return -ENOMEM;
    }
    struct w5500_netdev_priv* priv = netdev_priv(w5500_netdev);
    priv->dev = w5500_netdev;
    priv->spi = spi;
    priv->int_pin = int_pin;
    spi_set_drvdata(spi, w5500_netdev);
    
    result = register_netdev(w5500_netdev);
    if(result){
        printk("w5500_driver - Error %d initializing card\n", result);
        free_netdev(w5500_netdev);
        gpiod_put(int_pin);
        return result;
    }
    
    printk("w5500_driver - Successfully allocated and registered the network device\n");
    return 0;
}

/*
  Function called when a w5500 compatible device is removed
*/
static void w5500_remove(struct spi_device *spi) {
    printk("w5500_driver - w5500 compatible device removed, cleaning up...\n");

    struct net_device* w5500_netdev = spi_get_drvdata(spi);
    if(w5500_netdev){
        struct w5500_netdev_priv* priv = netdev_priv(w5500_netdev);
        struct gpio_desc* int_pin = priv->int_pin;
        unregister_netdev(w5500_netdev);
        free_netdev(w5500_netdev);
        gpiod_put(int_pin);
    }
}

/*
  Define and register the SPI driver structure
*/
static struct spi_driver w5500_driver = {
    .driver = {
        .name = "w5500",
        .of_match_table = w5500_dt_ids,
        .owner = THIS_MODULE,
    },
    .probe = w5500_probe,
    .remove = w5500_remove
};
module_spi_driver(w5500_driver);