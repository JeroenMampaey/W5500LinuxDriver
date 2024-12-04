#ifndef W5500_OPERATIONS_H
#define W5500_OPERATIONS_H

#include "w5500_netdev.h"

#define W5500_MTU 1514

extern int w5500_ctrl_check_err_code;
#define W5500_CTRL_CHECK_FOR_ERR(to_check, label) if((to_check)==-1){goto label;}
#define W5500_CTRL_IF_ERR(to_check) if((to_check)==-1)
#define W5500_CTRL_WHILE_ERR(to_check) while((to_check)==-1)
#define W5500_CTRL_ERR (w5500_ctrl_check_err_code)

#define W5500_CTRL_ERR_SPI_ERROR -1
#define W5500_CTRL_ERR_INVALID_SOCKET -2
#define W5500_CTRL_ERR_INCORRECT_CHIP_VERSION -3
#define W5500_CTRL_ERR_TX_BUFFERS_ARE_FULL -4
#define W5500_CTRL_ERR_KMALLOC_DURING_INIT_FAILED -5
#define W5500_CTRL_ERR_REQ_IRQ_DURING_INIT_FAILED -6
#define W5500_CTRL_ERR_RX_SIZE_ZERO -7
#define W5500_CTRL_ERR_RX_SIZE_TOO_LARGE -8

/*
    Functions return -1 if failed and will set w5500_ops_check_err_code to the error code

    Thus intended usage of these functions is:
            ...
            W5500_OPS_CHECK_FOR_ERR(read_common_register(...), on_error);
            ...
        on_error:
            int error_code = W5500_OPS_ERR;
            ...
*/

struct w5500_controller{
    struct spi_device* spi;
    struct gpio_desc* int_pin;
    spinlock_t receive_handler_spinlock;
    spinlock_t receive_spinlock;
    struct work_struct receive_work;
    struct work_struct reschedule_receive_work;
    struct workqueue_struct* w5500_receive_wq;
    DECLARE_KFIFO_PTR(receive_queue, char);
    void (*reception_callback)(void*);
    void* reception_callback_data;
};

int alloc_w5500_controller(struct w5500_controller** controller, 
    struct spi_device* spi, 
    struct gpio_desc* int_pin);
void free_w5500_controller(struct w5500_controller** controller);

int verify_w5500_chip_version(struct w5500_controller* controller);
int reset_w5500_for_MACRAW(struct w5500_controller* controller, const unsigned char mac[6]);
int stop_w5500(struct w5500_controller* controller);

int check_linkup(struct w5500_controller* controller, 
    bool* linkup);
int transmit_MACRAW(struct w5500_controller* controller, 
    unsigned char* data, 
    dma_addr_t data_dma,
    const unsigned short data_length);
int receive_MACRAW(struct w5500_controller* controller, 
    unsigned char* data,
    dma_addr_t data_dma, 
    unsigned short* data_length);
void on_packet_reception(struct w5500_controller* controller,
    void (*reception_callback)(void*), 
    void* reception_callback_data);

#endif