#include "w5500_netdev.h"
#include "w5500_controller.h"

#include <linux/delay.h>

#define MAX_TX_QUEUE_SIZE 32
#define WAIT_IF_RESETTING(to_do, flags) \
({ \
    typeof(to_do) __result; \
    while (atomic_cmpxchg((atomic_t *)(flags), 0b00, 0b01) != 0b00) { \
        cpu_relax(); \
    } \
    __result = (to_do); \
    atomic_and(~1U, (atomic_t *)(flags)); \
    __result; \
})

/*
    Forward declarations for private functions for w5500_netdev
*/
static void w5500_linkup_timer_callback(struct timer_list *t);
static void w5500_linkup_check(struct work_struct *w);
static void w5500_transmit_task(struct work_struct *w);
static void w5500_reschedule_transmit_task(struct work_struct *w);
static void w5500_on_packet_reception_callback(void* callback_data);
static void w5500_try_reset_on_failure(struct net_device *dev);

int w5500_netdev_open(struct net_device *dev){
    printk("w5500_driver - open called\n");

    struct w5500_netdev_priv* priv = (struct w5500_netdev_priv*)netdev_priv(dev);
    if(priv->is_open){
        printk("w5500_driver - Network device already open\n");
        return 0;
    }

    W5500_CTRL_CHECK_FOR_ERR(alloc_w5500_controller(&priv->controller, priv->spi, priv->int_pin), open_err0);

    W5500_CTRL_CHECK_FOR_ERR(verify_w5500_chip_version(priv->controller), open_err1);
    W5500_CTRL_CHECK_FOR_ERR(reset_w5500_for_MACRAW(priv->controller, dev->dev_addr), open_err1);

    priv->w5500_transmit_wq = alloc_workqueue("w5500_transmit_wq", WQ_HIGHPRI, 1);
    if(!priv->w5500_transmit_wq){
        printk("w5500_driver - w5500_transmit_wq alloc failed!\n");
        goto open_err1;
    }

    int failed = kfifo_alloc(&priv->transmit_queue, MAX_TX_QUEUE_SIZE, GFP_KERNEL);
    if(failed){
        printk("w5500_driver - queue_create transmit_queue failed!\n");
        goto open_err2;
    }

    failed = kfifo_alloc(&priv->transmit_buffers, MAX_TX_QUEUE_SIZE, GFP_KERNEL);
    if(failed){
        printk("w5500_driver - queue_create transmit_buffers failed!\n");
        goto open_err3;
    }
    for(int i=0; i<MAX_TX_QUEUE_SIZE; i++){
        struct transmit_data tx_buffer;
        tx_buffer.data = dma_alloc_coherent(priv->spi->master->dma_tx->device->dev,
            sizeof(unsigned char)*2048, &tx_buffer.data_dma, GFP_KERNEL);
        if(!tx_buffer.data){
            printk("w5500_driver - Error allocating free buffer for transmit_buffers!\n");
            goto open_err4;
        }
        kfifo_put(&priv->transmit_buffers, tx_buffer);
    }

    priv->receive_buffer.data = dma_alloc_coherent(priv->spi->master->dma_rx->device->dev, 
        sizeof(unsigned char)*2048*16, &priv->receive_buffer.data_dma, GFP_KERNEL);
    if(!priv->receive_buffer.data){
        printk("w5500_driver - Error allocating receive buffer!\n");
        goto open_err4;
    }

    priv->linkup = false;
    priv->failure_state.consecutive_transmit_failures = 0;
    priv->failure_state.consecutive_receive_failures = 0;
    priv->failure_state.consecutive_linkup_checks_without_receiving_data = 0;
    spin_lock_init(&priv->transmit_spinlock);
    priv->reset_flags.transmit_flags = 0;
    priv->reset_flags.receive_flags = 0;
    INIT_WORK(&priv->linkup_work, w5500_linkup_check);
    INIT_WORK(&priv->transmit_work, w5500_transmit_task);
    INIT_WORK(&priv->reschedule_transmit_work, w5500_reschedule_transmit_task);
    timer_setup(&priv->linkup_timer, w5500_linkup_timer_callback, 0);
    mod_timer(&priv->linkup_timer, jiffies + msecs_to_jiffies(1000));
    on_packet_reception(priv->controller, w5500_on_packet_reception_callback, dev);

    // Start queue and set interface to DOWN
    printk("w5500_driver - We will now allow transmissions...\n");
    netif_start_queue(dev);
    netif_carrier_off(dev);

    priv->is_open = true;

    return 0;

open_err4:
    dmaengine_terminate_all(priv->spi->master->dma_tx);
    struct transmit_data tx_buffer;
    while(kfifo_get(&priv->transmit_buffers, &tx_buffer)){
        dma_free_coherent(priv->spi->master->dma_tx->device->dev, sizeof(unsigned char)*2048, 
            tx_buffer.data, tx_buffer.data_dma);
    }
    kfifo_free(&priv->transmit_buffers);

open_err3:
    kfifo_free(&priv->transmit_queue);

open_err2:
    flush_workqueue(priv->w5500_transmit_wq);
    destroy_workqueue(priv->w5500_transmit_wq);

open_err1:
    // Doesn't matter if this was not initialized first
    stop_w5500(priv->controller);
    free_w5500_controller(&priv->controller);

open_err0:
    printk("w5500_driver - Error %d initializing w5500\n", W5500_CTRL_ERR);
    return -1;
}

int w5500_netdev_release(struct net_device *dev){
    printk("w5500_driver - release called\n");

    struct w5500_netdev_priv* priv = (struct w5500_netdev_priv*)netdev_priv(dev);
    if(!priv->is_open){
        printk("w5500_driver - Network device already closed\n");
        return 0;
    }

    stop_w5500(priv->controller);
    
    netif_stop_queue(dev);
    del_timer_sync(&priv->linkup_timer);
    cancel_work_sync(&priv->linkup_work);
    flush_workqueue(priv->w5500_transmit_wq);
    destroy_workqueue(priv->w5500_transmit_wq);
    dmaengine_terminate_all(priv->spi->master->dma_tx);
    dmaengine_terminate_all(priv->spi->master->dma_rx);
    struct transmit_data tx_data;
    while(kfifo_get(&priv->transmit_queue, &tx_data)){
        dma_free_coherent(priv->spi->master->dma_tx->device->dev, sizeof(unsigned char)*2048, 
            tx_data.data, tx_data.data_dma);
    }
    kfifo_free(&priv->transmit_queue);
    while(kfifo_get(&priv->transmit_buffers, &tx_data)){
        dma_free_coherent(priv->spi->master->dma_tx->device->dev, sizeof(unsigned char)*2048, 
            tx_data.data, tx_data.data_dma);
    }
    kfifo_free(&priv->transmit_buffers);
    dma_free_coherent(priv->spi->master->dma_rx->device->dev, sizeof(unsigned char)*2048*16, 
        priv->receive_buffer.data, priv->receive_buffer.data_dma);
    
    free_w5500_controller(&priv->controller);

    priv->is_open = false;

    return 0;
}

int w5500_netdev_xmit(struct sk_buff *skb, struct net_device *dev){
    struct w5500_netdev_priv* priv = (struct w5500_netdev_priv*)netdev_priv(dev);
    struct net_device_stats* stats = &dev->stats;

    if(skb->data_len!=0 || skb_shinfo(skb)->nr_frags!=0){
        printk("w5500_driver - Error: packet has fragments which is unexpected\n");
        dev_kfree_skb(skb);
        atomic_long_inc((atomic_long_t*)&stats->tx_dropped);
        return 0;
    }

    unsigned int len = skb->len;
    if(len > 1514){
        dev_kfree_skb(skb);
        return 0;
    }
    else if(len < ETH_ZLEN){
        len = ETH_ZLEN;
    }
    
    struct transmit_data tx_data;
    if(!kfifo_get(&priv->transmit_buffers, &tx_data)){
        printk("w5500_driver - Error getting free buffer for transmit_buffers, this should not happen\n");
        unsigned int transmit_buffers_size = kfifo_len(&priv->transmit_buffers);
        unsigned int transmit_queue_size = kfifo_len(&priv->transmit_queue);
        printk("w5500_driver - transmit_buffers_size: %d, transmit_queue_size: %d\n", transmit_buffers_size, transmit_queue_size);
        dev_kfree_skb(skb);
        atomic_long_inc((atomic_long_t*)&stats->tx_errors);
        netif_stop_queue(dev);
		return -1;
    }

    memcpy(tx_data.data, skb->data, skb->len);
    if(len > skb->len){
        memset(tx_data.data + skb->len, 0, len - skb->len);
    }
    tx_data.data_length = len;

    unsigned long flags;
    spin_lock_irqsave(&priv->transmit_spinlock, flags);
    if(kfifo_is_empty(&priv->transmit_queue)){
        // If kfifo_is_empty, this means priv->reschedule_transmit_work should not be running anymore
        // This is guaranteed because of the workqueue having max_active = 1
        if(!queue_work(priv->w5500_transmit_wq, &priv->transmit_work)){
            // If queue_work returns false, this means transmit_work is still running
            // But since kfifo_is_empty, it's about to exit the while(!is_empty) loop
            // reschedule_transmit_work will requeue transmit_work once finished
            // We will then push to the kfifo making it non-empty till transmit_work is running again
            printk("w5500_driver - work could not be queued because it was still finishing up, scheduling reschedule work\n");
            queue_work(priv->w5500_transmit_wq, &priv->reschedule_transmit_work);
        }
    }

    if(!kfifo_put(&priv->transmit_queue, tx_data)){
        printk("w5500_driver - Transmit queue full before adding element, this should not happen!\n");
        atomic_long_inc((atomic_long_t*)&stats->tx_errors);
        netif_stop_queue(dev);
        if(!kfifo_put(&priv->transmit_buffers, tx_data)){
            printk("w5500_driver - Error putting buffer back into transmit_buffers, this definitely should not happen!!!!!\n");
            kfree(tx_data.data);
        }
        spin_unlock_irqrestore(&priv->transmit_spinlock, flags);
        return -1;
    }

    if(kfifo_is_full(&priv->transmit_queue)){
        printk("w5500_driver - Transmit queue full after adding element\n");
        netif_stop_queue(dev);
    }
    spin_unlock_irqrestore(&priv->transmit_spinlock, flags);
    
    dev_kfree_skb(skb);
    return 0;
}

struct net_device_stats* w5500_netdev_stats(struct net_device *dev){
    return &dev->stats;
}

void w5500_netdev_setup(struct net_device *dev){
    printk("w5500_driver - setup called\n");
    ether_setup(dev);
    eth_hw_addr_random(dev);

    struct w5500_netdev_priv* priv = (struct w5500_netdev_priv*)netdev_priv(dev);
    memset(priv, 0, sizeof(struct w5500_netdev_priv));
    priv->my_netdev_ops.ndo_open = w5500_netdev_open;
    priv->my_netdev_ops.ndo_stop = w5500_netdev_release;
    priv->my_netdev_ops.ndo_start_xmit = w5500_netdev_xmit;
    priv->my_netdev_ops.ndo_get_stats = w5500_netdev_stats;
    dev->netdev_ops = &priv->my_netdev_ops;
    priv->is_open = false;

    printk("w5500_driver - Successfully set up the network device\n");
    printk("w5500_driver - MAC address: %pM\n", dev->dev_addr);
}

static void w5500_linkup_timer_callback(struct timer_list *t){
    printk("w5500_driver - checking linkup\n");

    struct w5500_netdev_priv* priv = container_of(t, struct w5500_netdev_priv, linkup_timer);

    schedule_work(&priv->linkup_work);

    mod_timer(t, jiffies + msecs_to_jiffies(5000));
    return;
}

static void w5500_linkup_check(struct work_struct *w){
    struct w5500_netdev_priv* priv = container_of(w, struct w5500_netdev_priv, linkup_work);
    struct net_device* dev = priv->dev;

    atomic_inc((atomic_t*)&priv->failure_state.consecutive_linkup_checks_without_receiving_data);
    if(atomic_read((atomic_t*)&priv->failure_state.consecutive_linkup_checks_without_receiving_data) > FAILURE_STATE_CLCWRD_THRESHOLD){
        w5500_try_reset_on_failure(dev);
    }

    bool linkup;
    W5500_CTRL_CHECK_FOR_ERR(check_linkup(priv->controller, &linkup), linkup_check_err);

    if(linkup != priv->linkup){
        priv->linkup = linkup;
        switch(linkup){
            case true:
                printk("w5500_driver - Link up\n");
                netif_carrier_on(dev);
                break;
            case false:
                printk("w5500_driver - Link down\n");
                netif_carrier_off(dev);
                break;
        }
    }

    return;

linkup_check_err:
    printk("w5500_driver - Error %d checking linkup\n", W5500_CTRL_ERR);
    return;
}

static void w5500_transmit_task(struct work_struct *w){
    struct w5500_netdev_priv* priv = container_of(w, struct w5500_netdev_priv, transmit_work);
    struct net_device* dev = priv->dev;
    struct net_device_stats* stats = &dev->stats;
    struct transmit_data tx_data;

    // Not 100% sure spin_lock_irqsave is necessary here, documentation about ndo_start_xmit says:
    // "will be called with interrupts disabled by netconsole"
    unsigned long flags;
    spin_lock_irqsave(&priv->transmit_spinlock, flags);
    bool is_empty = kfifo_is_empty(&priv->transmit_queue);
    spin_unlock_irqrestore(&priv->transmit_spinlock, flags);

    while(!is_empty){
        if(!kfifo_peek(&priv->transmit_queue, &tx_data)){
            printk("w5500_driver - Error peeking transmit queue, this should not happen\n");
        }

        W5500_CTRL_IF_ERR(
            WAIT_IF_RESETTING(transmit_MACRAW(priv->controller, tx_data.data, tx_data.data_dma, tx_data.data_length), &priv->reset_flags.transmit_flags)
        ){
            printk("w5500_driver - Error %d transmitting data\n", W5500_CTRL_ERR);
            atomic_long_inc((atomic_long_t*)&stats->tx_errors);

            if(priv->failure_state.consecutive_transmit_failures++ > FAILURE_STATE_CTF_THRESHOLD){
                w5500_try_reset_on_failure(dev);
                priv->failure_state.consecutive_transmit_failures = FAILURE_STATE_CTF_THRESHOLD/2;
            }
        }
        else{
            atomic_long_inc((atomic_long_t*)&stats->tx_packets);
            atomic_long_add((long)tx_data.data_length, (atomic_long_t*)&stats->tx_bytes);
        
            priv->failure_state.consecutive_transmit_failures = 0;
        }

        spin_lock_irqsave(&priv->transmit_spinlock, flags);
        if(!kfifo_put(&priv->transmit_buffers, tx_data)){
            printk("w5500_driver - Error putting buffer back into transmit_buffers, this should not happen!\n");
            kfree(tx_data.data);
        }

        if(kfifo_is_full(&priv->transmit_queue)){
            printk("w5500_driver - transmit_queue, we should be waking now...\n");
            if(netif_queue_stopped(dev)){
                printk("w5500_driver - waking queue\n");
                netif_wake_queue(dev);
            }
        }

        if(!kfifo_get(&priv->transmit_queue, &tx_data)){
            printk("w5500_driver - Error getting from transmit queue, this should not happen\n");
        }
        is_empty = kfifo_is_empty(&priv->transmit_queue);
        spin_unlock_irqrestore(&priv->transmit_spinlock, flags);
    }
}

static void w5500_reschedule_transmit_task(struct work_struct *w){
    struct w5500_netdev_priv* priv = container_of(w, struct w5500_netdev_priv, reschedule_transmit_work);

    cancel_work_sync(&priv->transmit_work);
    queue_work(priv->w5500_transmit_wq, &priv->transmit_work);
}

static void w5500_on_packet_reception_callback(void* callback_data){
    struct net_device* dev = (struct net_device*)callback_data;
    struct net_device_stats* stats = &dev->stats;
    struct w5500_netdev_priv* priv = (struct w5500_netdev_priv*)netdev_priv(dev);

    atomic_set((atomic_t*)&priv->failure_state.consecutive_linkup_checks_without_receiving_data, 0);

    unsigned short data_length;
    W5500_CTRL_IF_ERR(
        WAIT_IF_RESETTING(receive_MACRAW(priv->controller, priv->receive_buffer.data, priv->receive_buffer.data_dma, &data_length), &priv->reset_flags.receive_flags)
    ){
        if(W5500_CTRL_ERR != W5500_CTRL_ERR_RX_SIZE_ZERO){
            printk("w5500_driver - Error %d receiving data\n", W5500_CTRL_ERR);
            atomic_long_inc((atomic_long_t*)&stats->rx_errors);
        }
    
        if(priv->failure_state.consecutive_receive_failures++ > FAILURE_STATE_CRF_THRESHOLD){
            w5500_try_reset_on_failure(dev);
            priv->failure_state.consecutive_receive_failures = FAILURE_STATE_CRF_THRESHOLD/2;
        }

        return;
    }
    else{
        priv->failure_state.consecutive_receive_failures = 0;
    }
    
    unsigned int offset = 0;
    unsigned int next_packet_size;
    while(
        (offset + 2 <= data_length)
        &&
        ((next_packet_size = (((unsigned int)priv->receive_buffer.data[offset]) << 8) + ((unsigned int)priv->receive_buffer.data[offset+1])) >= 2)
        &&
        (offset + next_packet_size <= (unsigned int)data_length)
    ){
        unsigned char* next_packet = priv->receive_buffer.data + offset + 2;
        offset += next_packet_size;
        next_packet_size -= 2;

        
        // Not sure why +2 is necessary, but it is done in the "Linux Device Drivers, Third Edition" book
        struct sk_buff* skb = dev_alloc_skb(next_packet_size + 2);
        if(!skb){
            if(printk_ratelimit()){
                printk("w5500_driver - Error allocating skb for received packet\n");
            }
            atomic_long_inc((atomic_long_t*)&stats->rx_dropped);
            continue;
        }
        memcpy(skb_put(skb, next_packet_size), next_packet, next_packet_size);
        skb->dev = dev;
        skb->protocol = eth_type_trans(skb, dev);
        skb->ip_summed = CHECKSUM_NONE;
        atomic_long_inc((atomic_long_t*)&stats->rx_packets);
        atomic_long_add((long)next_packet_size, (atomic_long_t*)&stats->rx_bytes);
        netif_rx(skb);
    }

    if(offset != data_length){
        printk("w5500_driver - Error: offset %d != data_length %d\n", offset, data_length);
    }
}

static void w5500_try_reset_on_failure(struct net_device *dev){
    printk("w5500_driver - Trying to perform a worst case reset on failure\n");
    struct w5500_netdev_priv* priv = (struct w5500_netdev_priv*)netdev_priv(dev);

    atomic_or(0b10, (atomic_t*)&priv->reset_flags.transmit_flags);
    atomic_or(0b10, (atomic_t*)&priv->reset_flags.receive_flags);
    
    while(atomic_read((atomic_t*)&priv->reset_flags.transmit_flags) & 0b01){
        cpu_relax();
    }
    while(atomic_read((atomic_t*)&priv->reset_flags.receive_flags) & 0b01){
        cpu_relax();
    }

    reset_w5500_for_MACRAW(priv->controller, dev->dev_addr);

    atomic_and(~0b10, (atomic_t*)&priv->reset_flags.transmit_flags);
    atomic_and(~0b10, (atomic_t*)&priv->reset_flags.receive_flags);
}
