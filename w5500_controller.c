#include "w5500_controller.h"

int w5500_ctrl_check_err_code;

#define W5500_CTRL_ABORT_ON_ERR(to_check) if(to_check==-1){return -1;}

/*
    Forward declarations for private functions for w5500_controller
*/
static int read_common_register(struct w5500_controller* controller, 
    unsigned short register_offset, 
    unsigned char* data, 
    const unsigned short data_length);
static int write_common_register(struct w5500_controller* controller, 
    unsigned short register_offset, 
    const unsigned char* data, 
    const unsigned short data_length);
static int read_socket_n_register(struct w5500_controller* controller, 
    unsigned int socket, 
    unsigned short register_offset, 
    unsigned char* data, 
    const unsigned short data_length);
static int write_socket_n_register(struct w5500_controller* controller, 
    unsigned int socket, 
    unsigned short register_offset, 
    const unsigned char* data, 
    const unsigned short data_length);
static int write_socket_n_tx_buffer(struct w5500_controller* controller, 
    unsigned int socket, 
    unsigned short buffer_offset,
    const unsigned char* data,
    dma_addr_t data_dma,
    const unsigned short data_length);
static int read_socket_n_rx_buffer(struct w5500_controller* controller,
    unsigned int socket,
    unsigned short buffer_offset,
    unsigned char* data,
    dma_addr_t data_dma, 
    const unsigned short data_length);
static void receive_task(struct work_struct *w);
static void reschedule_receive_task(struct work_struct *w);
static irqreturn_t irq_handler(int irq, void *dev_id);

int alloc_w5500_controller(struct w5500_controller** controller, struct spi_device* spi, struct gpio_desc* int_pin){    
    *controller = kmalloc(sizeof(struct w5500_controller), GFP_KERNEL);
    if(!*controller){
        w5500_ctrl_check_err_code = W5500_CTRL_ERR_KMALLOC_DURING_INIT_FAILED;
        goto init_err0;
    }

    (*controller)->spi = spi;
    (*controller)->int_pin = int_pin;
    (*controller)->reception_callback = NULL;

    unsigned int irq_number = gpiod_to_irq(int_pin);
    if(request_irq(irq_number, irq_handler, IRQF_TRIGGER_FALLING, "w5500_irq", *controller)){
        printk("w5500_driver - Error requesting interrupt for %d\n", irq_number);
        w5500_ctrl_check_err_code = W5500_CTRL_ERR_REQ_IRQ_DURING_INIT_FAILED;
        goto init_err1;
    }

    (*controller)->w5500_receive_wq = alloc_workqueue("w5500_receive_wq", WQ_HIGHPRI, 1);
    if(!(*controller)->w5500_receive_wq){
        printk("w5500_driver - w5500_receive_wq alloc failed!\n");
        goto init_err2;
    }

    int failed = kfifo_alloc(&(*controller)->receive_queue, 2, GFP_KERNEL);
    if(failed){
        printk("w5500_driver - queue_create receive_queue failed!\n");
        goto init_err3;
    }

    spin_lock_init(&(*controller)->receive_spinlock);
    spin_lock_init(&(*controller)->receive_handler_spinlock);
    INIT_WORK(&(*controller)->receive_work, receive_task);
    INIT_WORK(&(*controller)->reschedule_receive_work, reschedule_receive_task);

    return 0;

init_err3:
    flush_workqueue((*controller)->w5500_receive_wq);
    destroy_workqueue((*controller)->w5500_receive_wq);

init_err2:
    free_irq(irq_number, *controller);

init_err1:
    kfree(*controller);
    *controller = NULL;

init_err0:
    return -1;
}

void free_w5500_controller(struct w5500_controller** controller){
    unsigned int irq_number = gpiod_to_irq((*controller)->int_pin);
    free_irq(irq_number, *controller);
    cancel_work_sync(&(*controller)->receive_work);
    flush_workqueue((*controller)->w5500_receive_wq);
    destroy_workqueue((*controller)->w5500_receive_wq);
    kfifo_free(&(*controller)->receive_queue);
    kfree(*controller);
    *controller = NULL;
}

static int read_common_register(struct w5500_controller* controller, unsigned short register_offset, unsigned char* data, const unsigned short data_length){
    struct spi_device *spi = controller->spi;
    
    unsigned char offset_msb = (register_offset >> 8) & 0xFF;
    unsigned char offset_lsb = register_offset & 0xFF;

    char tx_buffer[3] = {offset_msb, offset_lsb, 0x00};
    struct spi_transfer transfer1 = {
        .tx_buf = tx_buffer,
        .rx_buf = NULL,
        .len = 3,
    };
    struct spi_transfer transfer2 = {
        .tx_buf = NULL,
        .rx_buf = data,
        .len = data_length,
    };
    struct spi_transfer transfers[2] = {transfer1, transfer2};
    int result = spi_sync_transfer(spi, transfers, 2);

    if(result){
        w5500_ctrl_check_err_code = W5500_CTRL_ERR_SPI_ERROR;
        return -1;
    }

    return 0;
}

static int write_common_register(struct w5500_controller* controller, unsigned short register_offset, const unsigned char* data, const unsigned short data_length){
    struct spi_device *spi = controller->spi;

    unsigned char offset_msb = (register_offset >> 8) & 0xFF;
    unsigned char offset_lsb = register_offset & 0xFF;

    char tx_buffer[3] = {offset_msb, offset_lsb, 0x04};
    struct spi_transfer transfer1 = {
        .tx_buf = tx_buffer,
        .rx_buf = NULL,
        .len = 3,
    };
    struct spi_transfer transfer2 = {
        .tx_buf = data,
        .rx_buf = NULL,
        .len = data_length,
    };
    struct spi_transfer transfers[2] = {transfer1, transfer2};
    int result = spi_sync_transfer(spi, transfers, 2);

    if(result){
        w5500_ctrl_check_err_code = W5500_CTRL_ERR_SPI_ERROR;
        return -1;
    }

    return 0;
}

static int read_socket_n_register(struct w5500_controller* controller, unsigned int socket, unsigned short register_offset, unsigned char* data, const unsigned short data_length){
    if(socket > 7){
        w5500_ctrl_check_err_code = W5500_CTRL_ERR_INVALID_SOCKET;
        return -1;
    }

    struct spi_device *spi = controller->spi;
    
    unsigned char offset_msb = (register_offset >> 8) & 0xFF;
    unsigned char offset_lsb = register_offset & 0xFF;

    char tx_buffer[3] = {offset_msb, offset_lsb, (char)(((0x01 + 4*socket) & 0x1F) << 3)};
    struct spi_transfer transfer1 = {
        .tx_buf = tx_buffer,
        .rx_buf = NULL,
        .len = 3,
    };
    struct spi_transfer transfer2 = {
        .tx_buf = NULL,
        .rx_buf = data,
        .len = data_length,
    };

    struct spi_transfer transfers[2] = {transfer1, transfer2};
    int result = spi_sync_transfer(spi, transfers, 2);

    if(result){
        w5500_ctrl_check_err_code = W5500_CTRL_ERR_SPI_ERROR;
        return -1;
    }

    return 0;
}

static int write_socket_n_register(struct w5500_controller* controller, unsigned int socket, unsigned short register_offset, const unsigned char* data, const unsigned short data_length){
    if(socket > 7){
        w5500_ctrl_check_err_code = W5500_CTRL_ERR_INVALID_SOCKET;
        return -1;
    }

    struct spi_device *spi = controller->spi;

    unsigned char offset_msb = (register_offset >> 8) & 0xFF;
    unsigned char offset_lsb = register_offset & 0xFF;

    char tx_buffer[3] = {offset_msb, offset_lsb, (char)((((0x01 + 4*socket) & 0x1F) << 3) | 0x04)};
    struct spi_transfer transfer1 = {
        .tx_buf = tx_buffer,
        .rx_buf = NULL,
        .len = 3,
    };
    struct spi_transfer transfer2 = {
        .tx_buf = data,
        .rx_buf = NULL,
        .len = data_length,
    };

    struct spi_transfer transfers[2] = {transfer1, transfer2};
    int result = spi_sync_transfer(spi, transfers, 2);

    if(result){
        w5500_ctrl_check_err_code = W5500_CTRL_ERR_SPI_ERROR;
        return -1;
    }

    return 0;
}

static int write_socket_n_tx_buffer(struct w5500_controller* controller, unsigned int socket, unsigned short buffer_offset, const unsigned char* data, dma_addr_t data_dma, const unsigned short data_length){
    if(socket > 7){
        w5500_ctrl_check_err_code = W5500_CTRL_ERR_INVALID_SOCKET;
        return -1;
    }

    struct spi_device *spi = controller->spi;

    unsigned char offset_msb = (buffer_offset >> 8) & 0xFF;
    unsigned char offset_lsb = buffer_offset & 0xFF;

    char tx_buffer[3] = {offset_msb, offset_lsb, (char)((((0x02 + 4*socket) & 0x1F) << 3) | 0x04)};
    struct spi_transfer transfer1 = {
        .tx_buf = tx_buffer,
        .rx_buf = NULL,
        .len = 3,
    };
    struct spi_transfer transfer2 = {
        .tx_buf = data,
        .tx_dma = data_dma,
        .rx_buf = NULL,
        .len = data_length,
    };

    struct spi_transfer transfers[2] = {transfer1, transfer2};
    struct spi_message m;
    spi_message_init_with_transfers(&m, transfers, 2);
    int result = spi_sync(spi, &m);

    if(result){
        w5500_ctrl_check_err_code = W5500_CTRL_ERR_SPI_ERROR;
        return -1;
    }

    return 0;
}

static int read_socket_n_rx_buffer(struct w5500_controller* controller, unsigned int socket, unsigned short buffer_offset, unsigned char* data, dma_addr_t data_dma, const unsigned short data_length){
    if(socket > 7){
        w5500_ctrl_check_err_code = W5500_CTRL_ERR_INVALID_SOCKET;
        return -1;
    }

    struct spi_device *spi = controller->spi;

    unsigned char offset_msb = (buffer_offset >> 8) & 0xFF;
    unsigned char offset_lsb = buffer_offset & 0xFF;

    char tx_buffer[3] = {offset_msb, offset_lsb, (char)(((0x03 + 4*socket) & 0x1F) << 3)};
    struct spi_transfer transfer1 = {
        .tx_buf = tx_buffer,
        .rx_buf = NULL,
        .len = 3,
    };
    struct spi_transfer transfer2 = {
        .tx_buf = NULL,
        .rx_buf = data,
        .rx_dma = data_dma,
        .len = data_length,
    };

    struct spi_transfer transfers[2] = {transfer1, transfer2};
    struct spi_message m;
    spi_message_init_with_transfers(&m, transfers, 2);
    int result = spi_sync(spi, &m);

    if(result){
        w5500_ctrl_check_err_code = W5500_CTRL_ERR_SPI_ERROR;
        return -1;
    }

    return 0;
}

int verify_w5500_chip_version(struct w5500_controller* controller){
    // Check if the chip version is indeed 0x04
    unsigned char chip_version[1] = {0x00};
    W5500_CTRL_ABORT_ON_ERR(read_common_register(controller, 0x0039, chip_version, 1));
    if(chip_version[0] != 0x04){
        w5500_ctrl_check_err_code = W5500_CTRL_ERR_INCORRECT_CHIP_VERSION;
        return -1;
    }

    return 0;
}

int reset_w5500_for_MACRAW(struct w5500_controller* controller, const unsigned char mac[6]){
    // First make sure sockets are all closed before making configuration changes
    unsigned char close_command[1] = {0x10};
    for(int i=0; i<8; i++){
        W5500_CTRL_ABORT_ON_ERR(write_socket_n_register(controller, i, 0x0001, close_command, 1));
    }

    // Set the mode register to 0x00, no special features
    unsigned char mode[1] = {0x00};
    W5500_CTRL_ABORT_ON_ERR(write_common_register(controller, 0x0000, mode, 1));

    // Set the source hardware address to the MAC address of the network device
    W5500_CTRL_ABORT_ON_ERR(write_common_register(controller, 0x0009, mac, 6));

    // Set interrupt mask to 0x00, we don't care about the common interrupts
    unsigned char interrupt_mask[1] = {0x00};
    W5500_CTRL_ABORT_ON_ERR(write_common_register(controller, 0x0016, interrupt_mask, 1));

    // Set interrupt register to 0xF0, clears all interrupts
    printk("w5500_driver - Clearing regular interrupts\n");
    unsigned char interrupt[1] = {0xF0};
    W5500_CTRL_ABORT_ON_ERR(write_common_register(controller, 0x0015, interrupt, 1));

    // Set socket interrupt mask to 0x01, we want to know about only socket 0 interrupts
    unsigned char socket_interrupt_mask[1] = {0x01};
    W5500_CTRL_ABORT_ON_ERR(write_common_register(controller, 0x0018, socket_interrupt_mask, 1));

    // We want some socket features: "MAC Filter Enable in MACRAW mode", "MACRAW"
    // Thus set mode to 0b10000100 <-> 0x84
    mode[0] = 0x84;
    W5500_CTRL_ABORT_ON_ERR(write_socket_n_register(controller, 0, 0x0000, mode, 1));
    mode[0] = 0x00;
    for(int i=1; i<8; i++){
        // Close all other sockets
        W5500_CTRL_ABORT_ON_ERR(write_socket_n_register(controller, i, 0x0000, mode, 1));
    }

    // MTU for MACRAW mode is 1514 bytes
    unsigned char mtu[2] = {(W5500_MTU >> 8) & 0xFF, W5500_MTU & 0xFF};
    W5500_CTRL_ABORT_ON_ERR(write_socket_n_register(controller, 0, 0x0012, mtu, 2));

    // Socket 0 wants all the RX and TX buffer space, namely 16KB
    unsigned char buffer_size[1] = {0x00};
    for(int i=1; i<8; i++){
        // All other sockets don't get any buffer space
        W5500_CTRL_ABORT_ON_ERR(write_socket_n_register(controller, i, 0x001E, buffer_size, 1));
        W5500_CTRL_ABORT_ON_ERR(write_socket_n_register(controller, i, 0x001F, buffer_size, 1));
    }
    buffer_size[0] = 0x10;
    W5500_CTRL_ABORT_ON_ERR(write_socket_n_register(controller, 0, 0x001E, buffer_size, 1));
    W5500_CTRL_ABORT_ON_ERR(write_socket_n_register(controller, 0, 0x001F, buffer_size, 1));

    // We are interested in interrupts when data is received
    interrupt_mask[0] = 0x04;
    W5500_CTRL_ABORT_ON_ERR(write_socket_n_register(controller, 0, 0x002C, interrupt_mask, 1));
    interrupt_mask[0] = 0x00;
    for(int i=1; i<8; i++){
        // Don't care about intterupts for other sockets
        W5500_CTRL_ABORT_ON_ERR(write_socket_n_register(controller, i, 0x002C, interrupt_mask, 1));
    }

    // Clear interrupts for socket 0
    printk("w5500_driver - Clearing all socket interrupts\n");
    unsigned char cleared_interrupts[1] = {0x1F};
    W5500_CTRL_ABORT_ON_ERR(write_socket_n_register(controller, 0, 0x0002, cleared_interrupts, 1));

    // Open socket 0
    unsigned char open_command[1] = {0x01};
    W5500_CTRL_ABORT_ON_ERR(write_socket_n_register(controller, 0, 0x0001, open_command, 1));

    return 0;
}

int stop_w5500(struct w5500_controller* controller){
    // Close all sockets
    unsigned char close_command[1] = {0x10};
    for(int i=0; i<8; i++){
        W5500_CTRL_ABORT_ON_ERR(write_socket_n_register(controller, i, 0x0001, close_command, 1));
    }

    return 0;
}

int transmit_MACRAW(struct w5500_controller* controller, unsigned char* data, dma_addr_t data_dma, const unsigned short data_length){
    // Check if the TX buffer of socket 0 is big enough
    unsigned char tx_free_size[2];
    W5500_CTRL_ABORT_ON_ERR(read_socket_n_register(controller, 0, 0x0020, tx_free_size, 2));
    unsigned short free_size = (((unsigned short)tx_free_size[0]) << 8) + ((unsigned short)tx_free_size[1]);
    if(free_size < data_length){
        w5500_ctrl_check_err_code = W5500_CTRL_ERR_TX_BUFFERS_ARE_FULL;
        return -1;
    }

    // Get the TX write pointer
    unsigned char tx_wr[2];
    W5500_CTRL_ABORT_ON_ERR(read_socket_n_register(controller, 0, 0x0024, tx_wr, 2));
    unsigned short tx_write_pointer = (((unsigned short)tx_wr[0]) << 8) + ((unsigned short)tx_wr[1]);

    // Write to the socker buffer with offset tx_write_pointer
    W5500_CTRL_ABORT_ON_ERR(write_socket_n_tx_buffer(controller, 0, tx_write_pointer, data, data_dma, data_length));

    // Update the TX write pointer
    tx_write_pointer += data_length;
    tx_wr[0] = (tx_write_pointer >> 8) & 0xFF;
    tx_wr[1] = tx_write_pointer & 0xFF;
    W5500_CTRL_ABORT_ON_ERR(write_socket_n_register(controller, 0, 0x0024, tx_wr, 2));

    // Transmit the data using the SEND command
    unsigned char send_command[1] = {0x20};
    W5500_CTRL_ABORT_ON_ERR(write_socket_n_register(controller, 0, 0x0001, send_command, 1));

    return 0;
}

int check_linkup(struct w5500_controller* controller, bool* linkup){
    unsigned char link_status[1];
    W5500_CTRL_ABORT_ON_ERR(read_common_register(controller, 0x002E, link_status, 1));
    *linkup = link_status[0] & 0x01;
    
    return 0;
}

static void receive_task(struct work_struct *w){
    struct w5500_controller* controller = container_of(w, struct w5500_controller, receive_work);
    char rx_data;
    unsigned long flags;

    spin_lock_irqsave(&controller->receive_spinlock, flags);
    bool is_empty = kfifo_is_empty(&controller->receive_queue);
    spin_unlock_irqrestore(&controller->receive_spinlock, flags);

    while(!is_empty){
        // Clear RECV interrupt for socket 0
        unsigned int sleep_ms = 100;
        unsigned char cleared_interrupts[1] = {0x04};
        W5500_CTRL_WHILE_ERR(write_socket_n_register(controller, 0, 0x0002, cleared_interrupts, 1)){
            // This is a real problem, as long as this fails, interrupts won't be reset
            // Thus sleep for a bit and try again, using exponential backoff
            if(sleep_ms < 12800){
                sleep_ms *= 2;
            }
            msleep(sleep_ms);
        }

        spin_lock(&controller->receive_handler_spinlock);
        void (*reception_callback)(void*) = controller->reception_callback;
        void* reception_callback_data = controller->reception_callback_data;
        spin_unlock(&controller->receive_handler_spinlock);

        if(reception_callback){
            reception_callback(reception_callback_data);
        }     

        spin_lock_irqsave(&controller->receive_spinlock, flags);
        if(!kfifo_get(&controller->receive_queue, &rx_data)){
            printk("w5500_driver - Error getting from receive queue, this should not happen\n");
        }
        is_empty = kfifo_is_empty(&controller->receive_queue);
        spin_unlock_irqrestore(&controller->receive_spinlock, flags);
    }
}

static void reschedule_receive_task(struct work_struct *w){
    struct w5500_controller* controller = container_of(w, struct w5500_controller, reschedule_receive_work);

    cancel_work_sync(&controller->receive_work);
    queue_work(controller->w5500_receive_wq, &controller->receive_work);
}

static irqreturn_t irq_handler(int irq, void *dev_id){
    struct w5500_controller* controller = (struct w5500_controller*)dev_id;

    spin_lock(&controller->receive_spinlock);
    if(kfifo_is_empty(&controller->receive_queue)){
        // If kfifo_is_empty, this means controller->reschedule_receive_work should not be running anymore
        // This is guaranteed because of the workqueue having max_active = 1
        if(!queue_work(controller->w5500_receive_wq, &controller->receive_work)){
            // If queue_work returns false, this means receive_work is still running
            // But since kfifo_is_empty, it's about to exit the while(!is_empty) loop
            // reschedule_receive_work will requeue receive_work once finished
            // We will then push to the kfifo making it non-empty till receive_work is running again
            printk("w5500_driver - work could not be queued because it was still finishing up, scheduling reschedule work\n");
            queue_work(controller->w5500_receive_wq, &controller->reschedule_receive_work);
        }
    }

    if(!kfifo_put(&controller->receive_queue, 'a')){
        // controller->receive_queue is full, but that's fine
        printk("w5500_driver - Receive queue full\n");
    }
    spin_unlock(&controller->receive_spinlock);

	return IRQ_HANDLED;
}

void on_packet_reception(struct w5500_controller* controller, void (*reception_callback)(void*), void* reception_callback_data){
    spin_lock(&controller->receive_handler_spinlock);
    controller->reception_callback = reception_callback;
    controller->reception_callback_data = reception_callback_data;
    spin_unlock(&controller->receive_handler_spinlock);
}


int receive_MACRAW(struct w5500_controller* controller, unsigned char* data, dma_addr_t data_dma, unsigned short* data_length){
    // Get the buffer size of the RX buffer
    unsigned char rx_free_size[2];
    W5500_CTRL_ABORT_ON_ERR(read_socket_n_register(controller, 0, 0x0026, rx_free_size, 2));
    *data_length = (((unsigned short)rx_free_size[0]) << 8) + ((unsigned short)rx_free_size[1]);
    if(*data_length == 0){
        w5500_ctrl_check_err_code = W5500_CTRL_ERR_RX_SIZE_ZERO;
        return -1;
    }
    else if(*data_length > 2048*16){
        w5500_ctrl_check_err_code = W5500_CTRL_ERR_RX_SIZE_TOO_LARGE;
        return -1;
    }

    // Get the RX read pointer
    unsigned char rx_rd[2];
    W5500_CTRL_ABORT_ON_ERR(read_socket_n_register(controller, 0, 0x0028, rx_rd, 2));
    unsigned short rx_read_pointer = (((unsigned short)rx_rd[0]) << 8) + ((unsigned short)rx_rd[1]);
    
    // Read the data from the RX buffer
    W5500_CTRL_ABORT_ON_ERR(read_socket_n_rx_buffer(controller, 0, rx_read_pointer, data, data_dma, *data_length));

    // Update the RX read pointer
    rx_read_pointer += *data_length;
    rx_rd[0] = (rx_read_pointer >> 8) & 0xFF;
    rx_rd[1] = rx_read_pointer & 0xFF;
    W5500_CTRL_ABORT_ON_ERR(write_socket_n_register(controller, 0, 0x0028, rx_rd, 2));

    // Receive the data using the RECV command
    unsigned char recv_command[1] = {0x40};
    W5500_CTRL_ABORT_ON_ERR(write_socket_n_register(controller, 0, 0x0001, recv_command, 1));

    return 0;
}