/*
 * 1. Both chips boot up and automatically start listening for peers
 * 2. Once connected, use 'commsend <command>' to send commands
 */

#include "core/esp_comm_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_mac.h"
#include <string.h>
#include <stdio.h>
#include "managers/views/terminal_screen.h"

#define COMM_BUFFER_SIZE 512
#define COMM_PACKET_SIZE 128
#define DISCOVERY_INTERVAL_MS 2000
#define HANDSHAKE_TIMEOUT_MS 5000
#define COMMAND_TIMEOUT_MS 1000

#define DEFAULT_TX_PIN GPIO_NUM_6
#define DEFAULT_RX_PIN GPIO_NUM_7
#define DEFAULT_BAUD_RATE 115200

static const char* TAG = "esp_comm_manager";

typedef enum {
    PACKET_TYPE_DISCOVERY = 0x01,
    PACKET_TYPE_HANDSHAKE_REQ = 0x02,
    PACKET_TYPE_HANDSHAKE_ACK = 0x03,
    PACKET_TYPE_COMMAND = 0x04,
    PACKET_TYPE_RESPONSE = 0x05,
    PACKET_TYPE_PING = 0x06,
    PACKET_TYPE_PONG = 0x07
} packet_type_t;

typedef enum {
    PARSE_STATE_IDLE = 0,
    PARSE_STATE_START_BYTE,
    PARSE_STATE_TYPE,
    PARSE_STATE_LENGTH,
    PARSE_STATE_DATA,
    PARSE_STATE_CHECKSUM
} packet_parse_state_t;

typedef struct {
    uint8_t start_byte;
    uint8_t type;
    uint8_t length;
    uint8_t data[COMM_PACKET_SIZE - 4];
} __attribute__((packed)) comm_packet_t;

typedef struct {
    gpio_num_t tx_pin;
    gpio_num_t rx_pin;
    uint32_t baud_rate;
    
    comm_state_t state;
    comm_role_t role;
    comm_peer_t peer;
    
    QueueHandle_t rx_byte_queue;
    QueueHandle_t rx_packet_queue;
    QueueHandle_t tx_queue;
    TaskHandle_t rx_task_handle;
    TaskHandle_t tx_task_handle;
    TaskHandle_t protocol_task_handle;
    TimerHandle_t discovery_timer;
    TimerHandle_t ping_timer;
    
    comm_command_callback_t command_callback;
    void* callback_user_data;
    
    char chip_name[32];
    uint8_t chip_id[6];
    
    bool initialized;
    bool is_executing_remote_cmd;
    
    packet_parse_state_t parse_state;
    comm_packet_t partial_packet;
    uint8_t data_bytes_received;
    uint8_t parse_buffer[COMM_PACKET_SIZE];
    uint8_t parse_buffer_pos;
} esp_comm_manager_t;

static esp_comm_manager_t* s_comm_manager = NULL;

static uint8_t calculate_checksum(const uint8_t* data, size_t len) {
    uint8_t checksum = 0;
    for (size_t i = 0; i < len; i++) {
        checksum ^= data[i];
    }
    return checksum;
}

static bool send_packet(const comm_packet_t* packet) {
    if (!s_comm_manager || !packet) return false;

    uint8_t send_buffer[COMM_PACKET_SIZE];
    send_buffer[0] = packet->start_byte;
    send_buffer[1] = packet->type;
    send_buffer[2] = packet->length;

    memcpy(&send_buffer[3], packet->data, packet->length);

    uint8_t checksum = calculate_checksum(send_buffer, 3 + packet->length);
    send_buffer[3 + packet->length] = checksum;

    size_t total_size = 4 + packet->length;
    uart_write_bytes(UART_NUM_1, send_buffer, total_size);

    // printf("Sent packet type: 0x%02x, length: %d, total size: %zu, checksum: 0x%02x\n", 
    //          packet->type, packet->length, total_size, checksum);
    return true;
}

static void rx_task(void* arg) {
    esp_comm_manager_t* comm = (esp_comm_manager_t*)arg;
    uint8_t rx_buffer[COMM_BUFFER_SIZE];

    while (1) {
        int len = uart_read_bytes(UART_NUM_1, rx_buffer, COMM_BUFFER_SIZE, pdMS_TO_TICKS(100));
        if (len <= 0) continue;

        for (int i = 0; i < len; ++i) {
            uint8_t byte = rx_buffer[i];
            
            switch (comm->parse_state) {
                case PARSE_STATE_IDLE:
                    if (byte == 0xAA) {
                        comm->parse_state = PARSE_STATE_START_BYTE;
                        comm->partial_packet.start_byte = byte;
                        comm->parse_buffer_pos = 0;
                        comm->parse_buffer[comm->parse_buffer_pos++] = byte;
                    }
                    break;
                    
                case PARSE_STATE_START_BYTE:
                    comm->partial_packet.type = byte;
                    comm->parse_buffer[comm->parse_buffer_pos++] = byte;
                    comm->parse_state = PARSE_STATE_TYPE;
                    break;
                    
                case PARSE_STATE_TYPE:
                    comm->partial_packet.length = byte;
                    comm->parse_buffer[comm->parse_buffer_pos++] = byte;
                    if (byte > COMM_PACKET_SIZE - 4) {
                        printf("W: Invalid packet length: %d\n", byte);
                        comm->parse_state = PARSE_STATE_IDLE;
                        break;
                    }
                    comm->data_bytes_received = 0;
                    comm->parse_state = (byte == 0) ? PARSE_STATE_CHECKSUM : PARSE_STATE_DATA;
                    break;
                    
                case PARSE_STATE_DATA:
                    comm->partial_packet.data[comm->data_bytes_received++] = byte;
                    comm->parse_buffer[comm->parse_buffer_pos++] = byte;
                    if (comm->data_bytes_received >= comm->partial_packet.length) {
                        comm->parse_state = PARSE_STATE_CHECKSUM;
                    }
                    break;
                    
                case PARSE_STATE_CHECKSUM:
                    {
                        uint8_t received_checksum = byte;
                        uint8_t calculated_checksum = calculate_checksum(comm->parse_buffer, comm->parse_buffer_pos);
                        
                        if (received_checksum == calculated_checksum) {
                            if (xQueueSend(comm->rx_packet_queue, &comm->partial_packet, pdMS_TO_TICKS(10)) != pdPASS) {
                                printf("W: Packet queue full, dropped packet type 0x%02x\n", comm->partial_packet.type);
                            }
                        } else {
                            printf("W: Checksum mismatch: expected 0x%02x, got 0x%02x\n", calculated_checksum, received_checksum);
                        }
                        comm->parse_state = PARSE_STATE_IDLE;
                    }
                    break;
                    
                default:
                    comm->parse_state = PARSE_STATE_IDLE;
                    break;
            }
        }
    }
}

static void send_discovery_packet(void) {
    if (!s_comm_manager) return;
    
    comm_packet_t packet = {0};
    packet.start_byte = 0xAA;
    packet.type = PACKET_TYPE_DISCOVERY;
    packet.length = 38;
    
    memcpy(packet.data, s_comm_manager->chip_id, 6);
    strncpy((char*)packet.data + 6, s_comm_manager->chip_name, 31);
    ((char*)packet.data)[37] = '\0';
    
    // printf("Discovery packet: start=0x%02x, type=0x%02x, len=%d\n", 
    //          packet.start_byte, packet.type, packet.length);
    
    send_packet(&packet);
}

static void send_handshake_request(const char* peer_name) {
    if (!s_comm_manager) return;
    
    comm_packet_t packet = {0};
    packet.start_byte = 0xAA;
    packet.type = PACKET_TYPE_HANDSHAKE_REQ;
    packet.length = 32;
    
    strncpy((char*)packet.data, peer_name, 32);
    
    send_packet(&packet);
}

static void send_handshake_ack(void) {
    if (!s_comm_manager) return;
    
    comm_packet_t packet = {0};
    packet.start_byte = 0xAA;
    packet.type = PACKET_TYPE_HANDSHAKE_ACK;
    packet.length = 0;
    
    send_packet(&packet);
}

static void protocol_task(void* arg) {
    esp_comm_manager_t* comm = (esp_comm_manager_t*)arg;
    comm_packet_t packet;
    
    while(1) {
        if (xQueueReceive(comm->rx_packet_queue, &packet, pdMS_TO_TICKS(100)) == pdPASS) {
            // printf("Protocol task processing packet type: 0x%02x\n", packet.type);
            
            switch(packet.type) {
                case PACKET_TYPE_DISCOVERY:
                    if (comm->state == COMM_STATE_SCANNING) {
                        memcpy(comm->peer.chip_id, packet.data, 6);
                        strncpy(comm->peer.chip_name, (char*)packet.data + 6, 32);
                        comm->peer.chip_name[31] = '\0';
                        printf("I: Discovered peer: %s\n", comm->peer.chip_name);

                        // Auto-connect based on name comparison to avoid race conditions
                        if (strcmp(comm->chip_name, comm->peer.chip_name) > 0) {
                            printf("I: Peer has smaller name, I will initiate connection.\n");
                            esp_comm_manager_connect_to_peer(comm->peer.chip_name);
                        }
                    }
                    break;
                    
                case PACKET_TYPE_HANDSHAKE_REQ:
                    if (comm->state == COMM_STATE_SCANNING || comm->state == COMM_STATE_IDLE) {
                        char requested_name[32];
                        strncpy(requested_name, (char*)packet.data, 32);
                        requested_name[31] = '\0';
                        if (strcmp(requested_name, comm->chip_name) == 0) {
                            comm->state = COMM_STATE_HANDSHAKE;
                            comm->role = COMM_ROLE_SLAVE;
                            send_handshake_ack();
                            comm->state = COMM_STATE_CONNECTED;
                            printf("I: Handshake complete - slave role\n");
                        }
                    }
                    break;
                    
                case PACKET_TYPE_HANDSHAKE_ACK:
                    if (comm->state == COMM_STATE_HANDSHAKE && comm->role == COMM_ROLE_MASTER) {
                        comm->state = COMM_STATE_CONNECTED;
                        printf("I: Handshake complete - master role\n");
                    }
                    break;
                    
                case PACKET_TYPE_COMMAND:
                    if (comm->state == COMM_STATE_CONNECTED && comm->command_callback) {
                        char command[32] = {0};
                        char data[COMM_PACKET_SIZE - 32] = {0};
                        
                        strncpy(command, (char*)packet.data, 32);
                        if (packet.length > 32) {
                            strncpy(data, (char*)packet.data + 32, packet.length - 32);
                        }
                        
                        comm->command_callback(command, data, comm->callback_user_data);
                    }
                    break;
                    
                case PACKET_TYPE_PING:
                    if (comm->state == COMM_STATE_CONNECTED) {
                        comm_packet_t pong = {0};
                        pong.start_byte = 0xAA;
                        pong.type = PACKET_TYPE_PONG;
                        pong.length = 0;
                        
                        send_packet(&pong);
                    }
                    break;
                    
                case PACKET_TYPE_RESPONSE:
                    if (comm->state == COMM_STATE_CONNECTED) {
                        char response_data[COMM_PACKET_SIZE - 3];
                        memcpy(response_data, packet.data, packet.length);
                        response_data[packet.length] = '\0';
                        printf("%s", response_data);
                    }
                    break;
                    
                default:
                    printf("W: Unknown packet type: 0x%02x\n", packet.type);
                    break;
            }
        }
    }
}

static void discovery_timer_callback(TimerHandle_t xTimer) {
    if (s_comm_manager && s_comm_manager->state == COMM_STATE_SCANNING) {
        send_discovery_packet();
    }
}

void esp_comm_manager_init_with_defaults(void) {
    esp_comm_manager_init(DEFAULT_TX_PIN, DEFAULT_RX_PIN, DEFAULT_BAUD_RATE);
}

void esp_comm_manager_init(gpio_num_t tx_pin, gpio_num_t rx_pin, uint32_t baud_rate) {
    if (s_comm_manager) {
        printf("W: Already initialized\n");
        return;
    }

    s_comm_manager = malloc(sizeof(esp_comm_manager_t));
    if (!s_comm_manager) {
        printf("E: Failed to allocate memory\n");
        return;
    }
    
    memset(s_comm_manager, 0, sizeof(esp_comm_manager_t));

    s_comm_manager->tx_pin = tx_pin;
    s_comm_manager->rx_pin = rx_pin;
    s_comm_manager->baud_rate = baud_rate;
    s_comm_manager->state = COMM_STATE_IDLE;
    s_comm_manager->role = COMM_ROLE_MASTER;
    s_comm_manager->is_executing_remote_cmd = false;
    s_comm_manager->parse_state = PARSE_STATE_IDLE;
    s_comm_manager->data_bytes_received = 0;
    s_comm_manager->parse_buffer_pos = 0;

    esp_read_mac(s_comm_manager->chip_id, ESP_MAC_WIFI_STA);
    snprintf(s_comm_manager->chip_name, sizeof(s_comm_manager->chip_name), 
             "ESP_%02X%02X%02X", 
             s_comm_manager->chip_id[3], 
             s_comm_manager->chip_id[4], 
             s_comm_manager->chip_id[5]);

    uart_config_t uart_config = {
        .baud_rate = baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_driver_install(UART_NUM_1, COMM_BUFFER_SIZE * 2, 0, 0, NULL, 0);
    uart_param_config(UART_NUM_1, &uart_config);
    uart_set_pin(UART_NUM_1, tx_pin, rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    s_comm_manager->rx_byte_queue = xQueueCreate(256, sizeof(uint8_t));
    s_comm_manager->rx_packet_queue = xQueueCreate(16, sizeof(comm_packet_t));
    s_comm_manager->tx_queue = xQueueCreate(16, sizeof(comm_packet_t));

    xTaskCreate(rx_task, "comm_rx_task", 4096, s_comm_manager, 12, &s_comm_manager->rx_task_handle);
    xTaskCreate(protocol_task, "comm_protocol_task", 8192, s_comm_manager, 10, &s_comm_manager->protocol_task_handle);
    
    s_comm_manager->discovery_timer = xTimerCreate("discovery_timer", 
                                                   pdMS_TO_TICKS(DISCOVERY_INTERVAL_MS),
                                                   pdTRUE, NULL, discovery_timer_callback);
    
    s_comm_manager->initialized = true;
    
    s_comm_manager->state = COMM_STATE_SCANNING;
    xTimerStart(s_comm_manager->discovery_timer, 0);

    printf("I: ESP Comm Manager initialized as '%s' on TX:%d RX:%d at %lu baud - Auto-listening for peers\n", 
             s_comm_manager->chip_name, tx_pin, rx_pin, (unsigned long)baud_rate);
}

bool esp_comm_manager_set_pins(gpio_num_t tx_pin, gpio_num_t rx_pin) {
    if (!s_comm_manager || !s_comm_manager->initialized) {
        printf("E: Not initialized\n");
        return false;
    }
    
    if (s_comm_manager->state != COMM_STATE_IDLE) {
        printf("W: Cannot change pins while connected or scanning\n");
        return false;
    }
    
    s_comm_manager->tx_pin = tx_pin;
    s_comm_manager->rx_pin = rx_pin;

    uart_set_pin(UART_NUM_1, tx_pin, rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    
    printf("I: Changed pins to TX:%d RX:%d\n", tx_pin, rx_pin);
    return true;
}

bool esp_comm_manager_start_discovery(void) {
    if (!s_comm_manager || !s_comm_manager->initialized) {
        printf("E: Not initialized\n");
        return false;
    }
    
    if (s_comm_manager->state != COMM_STATE_IDLE) {
        printf("W: Already in discovery or connected\n");
        return false;
    }
    
    s_comm_manager->state = COMM_STATE_SCANNING;
    xTimerStart(s_comm_manager->discovery_timer, 0);
    
    printf("I: Started discovery as '%s'\n", s_comm_manager->chip_name);
    return true;
}

bool esp_comm_manager_connect_to_peer(const char* peer_name) {
    if (!s_comm_manager || !s_comm_manager->initialized || !peer_name) {
        printf("E: Invalid parameters\n");
        return false;
    }
    
    if (s_comm_manager->state != COMM_STATE_SCANNING) {
        printf("W: Not in scanning state\n");
        return false;
    }
    
    xTimerStop(s_comm_manager->discovery_timer, 0);
    s_comm_manager->state = COMM_STATE_HANDSHAKE;
    s_comm_manager->role = COMM_ROLE_MASTER;
    
    send_handshake_request(peer_name);
    
    printf("I: Connecting to peer: %s\n", peer_name);
    return true;
}

bool esp_comm_manager_send_command(const char* command, const char* data) {
    if (!s_comm_manager || !s_comm_manager->initialized || !command) {
        printf("E: Invalid parameters\n");
        return false;
    }
    
    if (s_comm_manager->state != COMM_STATE_CONNECTED) {
        printf("W: Not connected\n");
        return false;
    }
    
    comm_packet_t packet = {0};
    packet.start_byte = 0xAA;
    packet.type = PACKET_TYPE_COMMAND;
    
    strncpy((char*)packet.data, command, 32);
    packet.length = 32;
    
    if (data) {
        size_t data_len = strlen(data);
        size_t max_data_len = COMM_PACKET_SIZE - 32 - 4; // Account for header
        if (data_len > max_data_len) {
            data_len = max_data_len;
        }
        strncpy((char*)packet.data + 32, data, data_len);
        packet.length += data_len;
    }
    
    bool result = send_packet(&packet);
    if (result) {
        printf("I: Sent command: %s\n", command);
    }
    
    return result;
}

bool esp_comm_manager_send_response(const char* data) {
    if (!s_comm_manager || !s_comm_manager->initialized || !data) {
        printf("E: Invalid parameters for send_response\n");
        return false;
    }
    
    if (s_comm_manager->state != COMM_STATE_CONNECTED) {
        printf("W: Not connected, can't send response\n");
        return false;
    }
    
    comm_packet_t packet = {0};
    packet.start_byte = 0xAA;
    packet.type = PACKET_TYPE_RESPONSE;
    
    size_t data_len = strlen(data);
    size_t max_data_len = COMM_PACKET_SIZE - 4; 
    if (data_len > max_data_len) {
        data_len = max_data_len;
    }
    strncpy((char*)packet.data, data, data_len);
    packet.length = data_len;
    
    return send_packet(&packet);
}

bool esp_comm_manager_is_connected(void) {
    return s_comm_manager && s_comm_manager->state == COMM_STATE_CONNECTED;
}

comm_state_t esp_comm_manager_get_state(void) {
    return s_comm_manager ? s_comm_manager->state : COMM_STATE_ERROR;
}

void esp_comm_manager_set_command_callback(comm_command_callback_t callback, void* user_data) {
    if (s_comm_manager) {
        s_comm_manager->command_callback = callback;
        s_comm_manager->callback_user_data = user_data;
    }
}

void esp_comm_manager_set_remote_command_flag(bool is_remote) {
    if (s_comm_manager) {
        s_comm_manager->is_executing_remote_cmd = is_remote;
    }
}

bool esp_comm_manager_is_remote_command(void) {
    return s_comm_manager && s_comm_manager->is_executing_remote_cmd;
}

void esp_comm_manager_disconnect(void) {
    if (s_comm_manager) {
        xTimerStop(s_comm_manager->discovery_timer, 0);
        s_comm_manager->state = COMM_STATE_IDLE;
        printf("I: Disconnected\n");
    }
}

void esp_comm_manager_deinit(void) {
    if (!s_comm_manager) return;

    uart_driver_delete(UART_NUM_1);
    
    if (s_comm_manager->discovery_timer) {
        xTimerDelete(s_comm_manager->discovery_timer, 0);
    }
    
    if (s_comm_manager->rx_task_handle) {
        vTaskDelete(s_comm_manager->rx_task_handle);
    }
    if (s_comm_manager->protocol_task_handle) {
        vTaskDelete(s_comm_manager->protocol_task_handle);
    }
    if (s_comm_manager->rx_byte_queue) {
        vQueueDelete(s_comm_manager->rx_byte_queue);
    }
    if (s_comm_manager->rx_packet_queue) {
        vQueueDelete(s_comm_manager->rx_packet_queue);
    }
    if (s_comm_manager->tx_queue) {
        vQueueDelete(s_comm_manager->tx_queue);
    }
    
    free(s_comm_manager);
    s_comm_manager = NULL;
    printf("I: ESP Comm Manager de-initialized\n");
} 