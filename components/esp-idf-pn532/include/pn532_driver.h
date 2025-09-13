#ifndef PN532_DRIVER_H
#define PN532_DRIVER_H

#include "esp_err.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C"
{
#endif
struct pn532_io_t;
typedef struct pn532_io_t pn532_io_t;
typedef struct pn532_io_t * pn532_io_handle_t;

#define PN532_HOST_TO_PN532                 (0xD4)

// timeouts in milliseconds used by public API calls
#ifndef PN532_WRITE_TIMEOUT
#define PN532_WRITE_TIMEOUT 100
#endif
#ifndef PN532_READ_TIMEOUT
#define PN532_READ_TIMEOUT 100
#endif

extern const uint8_t ACK_FRAME[];
extern const uint8_t NACK_FRAME[];

struct pn532_io_t {
    esp_err_t (*pn532_init_io)(pn532_io_handle_t io_handle);
    void (*pn532_release_io)(pn532_io_handle_t io_handle);
    void (*pn532_release_driver)(pn532_io_handle_t io_handle);
    esp_err_t (*pn532_read)(pn532_io_handle_t io_handle, uint8_t *read_buffer, size_t read_size, int xfer_timeout_ms);
    esp_err_t (*pn532_write)(pn532_io_handle_t io_handle, const uint8_t *write_buffer, size_t write_size, int xfer_timeout_ms);
    esp_err_t (*pn532_init_extra)(pn532_io_handle_t io_handle);
    esp_err_t (*pn532_is_ready)(pn532_io_handle_t io_handle);

    gpio_num_t reset;
    gpio_num_t irq;
    bool isSAMConfigDone;
#ifdef CONFIG_ENABLE_IRQ_ISR
    QueueHandle_t IRQQueue;
#endif
    void * driver_data;
};

esp_err_t pn532_init(pn532_io_handle_t io_handle);
void pn532_release(pn532_io_handle_t io_handle);
void pn532_delete_driver(pn532_io_handle_t io_handle);
void pn532_reset(pn532_io_handle_t io_handle);
esp_err_t pn532_write_command(pn532_io_handle_t io_handle, const uint8_t *cmd, uint8_t cmdlen, int timeout);
esp_err_t pn532_read_data(pn532_io_handle_t io_handle, uint8_t *buffer, uint8_t length, int32_t timeout);
esp_err_t pn532_wait_ready(pn532_io_handle_t io_handle, int32_t timeout);
esp_err_t pn532_SAM_config(pn532_io_handle_t io_handle);
esp_err_t pn532_send_command_wait_ack(pn532_io_handle_t io_handle, const uint8_t *cmd, uint8_t cmd_length, int32_t timeout);
esp_err_t pn532_read_ack(pn532_io_handle_t io_handle);

#ifdef __cplusplus
}
#endif

#endif //PN532_DRIVER_H


