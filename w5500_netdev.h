#ifndef W5500_NETDEV_H
#define W5500_NETDEV_H

#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/spi/spi.h>
#include <linux/timer.h>
#include <linux/spinlock.h>
#include <linux/atomic.h>
#include <linux/gpio/consumer.h>
#include <linux/kfifo.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>

#define FAILURE_STATE_CTF_THRESHOLD 32
#define FAILURE_STATE_CRF_THRESHOLD 32
#define FAILURE_STATE_CLCWRD_THRESHOLD 2

struct reset_flags{
    /*
        For example, for the transmit flags:
        - bit 0 is set if the transmit_MACRAW is busy
        - bit 1 is set if somewhere in the code, a reset of the w5500_controller is requested

        transmit_MACRAW will only run if bit 1 is not set
        w5500_controller will be reset if for no flags, bit 1 is set
    */
    unsigned int transmit_flags;
    unsigned int receive_flags;
};

struct failure_state{
    unsigned int consecutive_transmit_failures;
    unsigned int consecutive_receive_failures;
    unsigned int consecutive_linkup_checks_without_receiving_data;
};

struct receive_data{
    unsigned char* data;
    dma_addr_t data_dma;
};

struct transmit_data{   
    unsigned char* data;
    dma_addr_t data_dma;
    unsigned short data_length;
};

struct w5500_netdev_priv{
    struct net_device* dev;
    struct spi_device* spi;
    struct gpio_desc* int_pin;
    struct w5500_controller* controller;
    struct workqueue_struct* w5500_transmit_wq;
    struct timer_list linkup_timer;
    struct work_struct linkup_work;
    struct work_struct transmit_work;
    struct work_struct reschedule_transmit_work;
    spinlock_t transmit_spinlock;
    struct net_device_ops my_netdev_ops;
    DECLARE_KFIFO_PTR(transmit_queue, struct transmit_data);
    DECLARE_KFIFO_PTR(transmit_buffers, struct transmit_data);
    struct receive_data receive_buffer;
    bool is_open;
    bool linkup;
    struct failure_state failure_state;
    struct reset_flags reset_flags;
};

int w5500_netdev_open(struct net_device *dev);
int w5500_netdev_release(struct net_device *dev);
int w5500_netdev_xmit(struct sk_buff *skb, struct net_device *dev);
struct net_device_stats *w5500_netdev_stats(struct net_device *dev);
void w5500_netdev_setup(struct net_device *dev);

#endif