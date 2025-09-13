// copied from managed_components/garag__esp-idf-pn532/include/pn532.h
// fuck this duplication, but we need to patch it

#ifndef PN532_H
#define PN532_H

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "driver/gpio.h"
#include "pn532_driver.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define PN532_PREAMBLE                      (0x00)
#define PN532_STARTCODE1                    (0x00)
#define PN532_STARTCODE2                    (0xFF)
#define PN532_POSTAMBLE                     (0x00)

#define PN532_HOSTTOPN532                   (0xD4)
#define PN532_PN532TOHOST                   (0xD5)

#define PN532_COMMAND_DIAGNOSE              (0x00)
#define PN532_COMMAND_GETFIRMWAREVERSION    (0x02)
#define PN532_COMMAND_GETGENERALSTATUS      (0x04)
#define PN532_COMMAND_READREGISTER          (0x06)
#define PN532_COMMAND_WRITEREGISTER         (0x08)
#define PN532_COMMAND_READGPIO              (0x0C)
#define PN532_COMMAND_WRITEGPIO             (0x0E)
#define PN532_COMMAND_SETSERIALBAUDRATE     (0x10)
#define PN532_COMMAND_SETPARAMETERS         (0x12)
#define PN532_COMMAND_SAMCONFIGURATION      (0x14)
#define PN532_COMMAND_POWERDOWN             (0x16)
#define PN532_COMMAND_RFCONFIGURATION       (0x32)
#define PN532_COMMAND_RFREGULATIONTEST      (0x58)
#define PN532_COMMAND_INJUMPFORDEP          (0x56)
#define PN532_COMMAND_INJUMPFORPSL          (0x46)
#define PN532_COMMAND_INLISTPASSIVETARGET   (0x4A)
#define PN532_COMMAND_INATR                 (0x50)
#define PN532_COMMAND_INPSL                 (0x4E)
#define PN532_COMMAND_INDATAEXCHANGE        (0x40)
#define PN532_COMMAND_INCOMMUNICATETHRU     (0x42)
#define PN532_COMMAND_INDESELECT            (0x44)
#define PN532_COMMAND_INRELEASE             (0x52)
#define PN532_COMMAND_INSELECT              (0x54)
#define PN532_COMMAND_INAUTOPOLL            (0x60)
#define PN532_COMMAND_TGINITASTARGET        (0x8C)
#define PN532_COMMAND_TGSETGENERALBYTES     (0x92)
#define PN532_COMMAND_TGGETDATA             (0x86)
#define PN532_COMMAND_TGSETDATA             (0x8E)
#define PN532_COMMAND_TGSETMETADATA         (0x94)
#define PN532_COMMAND_TGGETINITIATORCOMMAND (0x88)
#define PN532_COMMAND_TGRESPONSETOINITIATOR (0x90)
#define PN532_COMMAND_TGGETTARGETSTATUS     (0x8A)

#define PN532_RESPONSE_INDATAEXCHANGE       (0x41)
#define PN532_RESPONSE_INCOMMUNICATETHRU    (0x43)
#define PN532_RESPONSE_INLISTPASSIVETARGET  (0x4B)

#define PN532_SPI_STATREAD                  (0x02)
#define PN532_SPI_DATAWRITE                 (0x01)
#define PN532_SPI_DATAREAD                  (0x03)
#define PN532_SPI_READY                     (0x01)

#define PN532_I2C_RAW_ADDRESS               (0x24)
#define PN532_I2C_ADDRESS                   (0x48)
#define PN532_I2C_READ_ADDRESS				(0x49)
#define PN532_I2C_READBIT                   (0x01)
#define PN532_I2C_BUSY                      (0x00)
#define PN532_I2C_READY                     (0x01)
#define PN532_I2C_READYTIMEOUT              (20)

#define PN532_BRTY_ISO14443A_106KBPS        (0x00)
#define PN532_BRTY_FELICA_212KBPS           (0x01)
#define PN532_BRTY_FELICA_424KBPS           (0x02)
#define PN532_BRTY_ISO14443B_106KBPS        (0x03)
#define PN532_BRTY_JEWEL_TAG_106KBPS        (0x04)

#define MIFARE_CMD_AUTH_A                   (0x60)
#define MIFARE_CMD_AUTH_B                   (0x61)
#define MIFARE_CMD_READ                     (0x30)
#define MIFARE_CMD_WRITE                    (0xA0)
#define MIFARE_CMD_TRANSFER                 (0xB0)
#define MIFARE_CMD_DECREMENT                (0xC0)
#define MIFARE_CMD_INCREMENT                (0xC1)
#define MIFARE_CMD_STORE                    (0xC2)
#define MIFARE_ULTRALIGHT_CMD_WRITE         (0xA2)

#define NDEF_URIPREFIX_NONE                 (0x00)

typedef enum {
    NTAG2XX_UNKNOWN,
    NTAG2XX_NTAG213,
    NTAG2XX_NTAG215,
    NTAG2XX_NTAG216
} NTAG2XX_MODEL;

esp_err_t pn532_get_firmware_version(pn532_io_handle_t io_handle, uint32_t *fw_version);
esp_err_t pn532_set_passive_activation_retries(pn532_io_handle_t io_handle, uint8_t maxRetries);
esp_err_t pn532_read_passive_target_id(pn532_io_handle_t io_handle,
                                       uint8_t baud_rate_and_card_type,
                                       uint8_t *uid,
                                       uint8_t *uid_length,
                                       int32_t timeout);
// Extended version that also returns ATQA (SENS_RES) and SAK (SEL_RES)
esp_err_t pn532_read_passive_target_id_ex(pn532_io_handle_t io_handle,
                                       uint8_t baud_rate_and_card_type,
                                       uint8_t *uid,
                                       uint8_t *uid_length,
                                       uint16_t *atqa,
                                       uint8_t *sak,
                                       int32_t timeout);
esp_err_t pn532_in_data_exchange(pn532_io_handle_t io_handle, const uint8_t *send_buffer, uint8_t send_buffer_length, uint8_t *response,
                                 uint8_t *response_length);
// Raw frame exchange with the PICC, used for special sequences (e.g., magic backdoor, custom auth flows)
esp_err_t pn532_in_communicate_thru(pn532_io_handle_t io_handle,
                                    const uint8_t *send_buffer,
                                    uint8_t send_buffer_length,
                                    uint8_t *response,
                                    uint8_t *response_length);
esp_err_t pn532_in_list_passive_target(pn532_io_handle_t io_handle);
esp_err_t ntag2xx_get_model(pn532_io_handle_t io_handle, NTAG2XX_MODEL *model);
esp_err_t ntag2xx_authenticate(pn532_io_handle_t io_handle, uint8_t page, uint8_t *key, uint8_t *uid, uint8_t uid_length);
esp_err_t ntag2xx_read_page(pn532_io_handle_t io_handle, uint8_t page, uint8_t *buffer, size_t read_len);
esp_err_t ntag2xx_write_page(pn532_io_handle_t io_handle, uint8_t page, const uint8_t *data);

// Additional NTAG/Ultralight helpers
esp_err_t ntag2xx_get_version(pn532_io_handle_t io_handle, uint8_t version_out[8]);
esp_err_t ntag2xx_read_signature(pn532_io_handle_t io_handle, uint8_t sig_out[32]);
esp_err_t ntag2xx_read_counter(pn532_io_handle_t io_handle, uint8_t counter_index, uint32_t *value_out);
esp_err_t ntag2xx_read_tearing(pn532_io_handle_t io_handle, uint8_t counter_index, uint8_t *tearing_out);

// Control verbose logging inside PN532 driver to speed up tight loops
void pn532_set_quiet(bool quiet);
// Tune internal wait_ready timeouts used by INDATAEXCHANGE and INCOMMUNICATETHRU
void pn532_set_indata_wait_timeout(int ms);
void pn532_set_thru_wait_timeout(int ms);
// Tune wait_ready timeout used by INLISTPASSIVETARGET
void pn532_set_inlist_wait_timeout(int ms);

#ifdef __cplusplus
}
#endif

#endif


