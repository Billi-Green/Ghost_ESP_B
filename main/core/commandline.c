// command.c

#include "core/commandline.h"
#include "core/callbacks.h"
#include "esp_sntp.h"
#include "managers/ap_manager.h"
#include "managers/ble_manager.h"
#include "managers/dial_manager.h"
#include "managers/rgb_manager.h"
#include "managers/settings_manager.h"
#include "managers/wifi_manager.h"
#include "managers/sd_card_manager.h"
#include "vendor/pcap.h"
#include "vendor/printer.h"
#include <esp_timer.h>
#include <managers/gps_manager.h>
#include <managers/views/terminal_screen.h>
#include <netdb.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <vendor/dial_client.h>
#include "esp_wifi.h"
#include "managers/default_portal.h"
#include <time.h>

static Command *command_list_head = NULL;
TaskHandle_t VisualizerHandle = NULL;

// Forward declarations for command handlers
#ifndef CONFIG_IDF_TARGET_ESP32S2
void handle_list_airtags_cmd(int argc, char **argv);
void handle_select_airtag(int argc, char **argv);
void handle_spoof_airtag(int argc, char **argv);
void handle_stop_spoof(int argc, char **argv);
#endif

#define MAX_PORTAL_PATH_LEN 128 // reasonable i guess?

void command_init() { command_list_head = NULL; }

void register_command(const char *name, CommandFunction function) {
    // Check if the command already exists
    Command *current = command_list_head;
    while (current != NULL) {
        if (strcmp(current->name, name) == 0) {
            // Command already registered
            return;
        }
        current = current->next;
    }

    // Create a new command
    Command *new_command = (Command *)malloc(sizeof(Command));
    if (new_command == NULL) {
        // Handle memory allocation failure
        return;
    }
    new_command->name = strdup(name);
    new_command->function = function;
    new_command->next = command_list_head;
    command_list_head = new_command;
}

void unregister_command(const char *name) {
    Command *current = command_list_head;
    Command *previous = NULL;

    while (current != NULL) {
        if (strcmp(current->name, name) == 0) {
            // Found the command to remove
            if (previous == NULL) {
                command_list_head = current->next;
            } else {
                previous->next = current->next;
            }
            free(current->name);
            free(current);
            return;
        }
        previous = current;
        current = current->next;
    }
}

CommandFunction find_command(const char *name) {
    Command *current = command_list_head;
    while (current != NULL) {
        if (strcmp(current->name, name) == 0) {
            return current->function;
        }
        current = current->next;
    }
    return NULL;
}

void cmd_wifi_scan_start(int argc, char **argv) {
    if (argc > 1) {
        int seconds = atoi(argv[1]);
        wifi_manager_start_scan_with_time(seconds);
    } else {
        wifi_manager_start_scan();
    }
    wifi_manager_print_scan_results_with_oui();
}

void cmd_wifi_scan_stop(int argc, char **argv) {
    wifi_manager_stop_monitor_mode();
    pcap_file_close();
    printf("WiFi scan stopped.\n");
    TERMINAL_VIEW_ADD_TEXT("WiFi scan stopped.\n");
}

void cmd_wifi_scan_results(int argc, char **argv) {
    printf("WiFi scan results displaying with OUI matching.\n");
    TERMINAL_VIEW_ADD_TEXT("WiFi scan results displaying with OUI matching.\n");
    wifi_manager_print_scan_results_with_oui();
}

void handle_list(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "-a") == 0) {
        cmd_wifi_scan_results(argc, argv);
        return;
    } else if (argc > 1 && strcmp(argv[1], "-s") == 0) {
        wifi_manager_list_stations();
        printf("Listed Stations...\n");
        TERMINAL_VIEW_ADD_TEXT("Listed Stations...\n");
        return;
    }
#ifndef CONFIG_IDF_TARGET_ESP32S2
    else if (argc > 1 && strcmp(argv[1], "-airtags") == 0) {
        ble_list_airtags();
        return;
    }
#endif
    else {
        printf("Usage: list -a (for Wi-Fi scan results)\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: list -a (for Wi-Fi scan results)\n");
    }
}

void handle_beaconspam(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "-r") == 0) {
        printf("Starting Random beacon spam...\n");
        TERMINAL_VIEW_ADD_TEXT("Starting Random beacon spam...\n");
        wifi_manager_start_beacon(NULL);
        return;
    }

    if (argc > 1 && strcmp(argv[1], "-rr") == 0) {
        printf("Starting Rickroll beacon spam...\n");
        TERMINAL_VIEW_ADD_TEXT("Starting Rickroll beacon spam...\n");
        wifi_manager_start_beacon("RICKROLL");
        return;
    }

    if (argc > 1 && strcmp(argv[1], "-l") == 0) {
        printf("Starting AP List beacon spam...\n");
        TERMINAL_VIEW_ADD_TEXT("Starting AP List beacon spam...\n");
        wifi_manager_start_beacon("APLISTMODE");
        return;
    }

    if (argc > 1) {
        wifi_manager_start_beacon(argv[1]);
        return;
    } else {
        printf("Usage: beaconspam -r (for Beacon Spam Random)\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: beaconspam -r (for Beacon Spam Random)\n");
    }
}

void handle_stop_spam(int argc, char **argv) {
    wifi_manager_stop_beacon();
    printf("Beacon Spam Stopped...\n");
    TERMINAL_VIEW_ADD_TEXT("Beacon Spam Stopped...\n");
}

void handle_sta_scan(int argc, char **argv) {
    wifi_manager_start_station_scan();
}

void handle_attack_cmd(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "-d") == 0) {
        printf("Deauthentication starting...\n");
        TERMINAL_VIEW_ADD_TEXT("Deauthentication starting...\n");
        wifi_manager_deauth_station();
        return;
    } else {
        printf("Usage: attack -d (for deauthing access points or selected station)\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: attack -d (for deauthing access points or selected station)\n");
    }
}

void handle_stop_deauth(int argc, char **argv) {
    wifi_manager_stop_deauth();
    wifi_manager_stop_deauth_station();
    printf("Deauthing Stopped....\n");
    TERMINAL_VIEW_ADD_TEXT("Deauthing Stopped....\n");
}

void handle_select_cmd(int argc, char **argv) {
    if (argc != 3) {
        printf("Usage: select -a <number> or select -s <number>\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: select -a <number> or select -s <number>\n");
        return;
    }

    if (strcmp(argv[1], "-a") == 0) {
        char *endptr;
        int num = (int)strtol(argv[2], &endptr, 10);
        if (*endptr == '\0') {
            wifi_manager_select_ap(num);
        } else {
            printf("Error: is not a valid number.\n");
            TERMINAL_VIEW_ADD_TEXT("Error: is not a valid number.\n");
        }
    } else if (strcmp(argv[1], "-s") == 0) {
        char *endptr;
        int num = (int)strtol(argv[2], &endptr, 10);
        if (*endptr == '\0') {
            wifi_manager_select_station(num);
        } else {
            printf("Error: is not a valid number.\n");
            TERMINAL_VIEW_ADD_TEXT("Error: is not a valid number.\n");
        }
#ifndef CONFIG_IDF_TARGET_ESP32S2
    } else if (strcmp(argv[1], "-airtag") == 0) {
        char *endptr;
        int num = (int)strtol(argv[2], &endptr, 10);
        if (*endptr == '\0') {
            ble_select_airtag(num);
        } else {
            printf("Error: '%s' is not a valid number.\n", argv[2]);
            TERMINAL_VIEW_ADD_TEXT("Error: '%s' is not a valid number.\n", argv[2]);
        }
#endif
    } else {
        printf("Invalid option. Usage: select -a <number> or select -s <number>\n");
        TERMINAL_VIEW_ADD_TEXT("Invalid option. Usage: select -a <number> or select -s <number>\n");
    }
}

void discover_task(void *pvParameter) {
    DIALClient client;
    DIALManager manager;

    if (dial_client_init(&client) == ESP_OK) {

        dial_manager_init(&manager, &client);

        explore_network(&manager);

        dial_client_deinit(&client);
    } else {
        printf("Failed to init DIAL client.\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to init DIAL client.\n");
    }

    vTaskDelete(NULL);
}

void handle_stop_flipper(int argc, char **argv) {
    wifi_manager_stop_deauth();
#ifndef CONFIG_IDF_TARGET_ESP32S2
    ble_stop();
#endif
    if (buffer_offset > 0) { // Only flush if there's data in buffer
        csv_flush_buffer_to_file();
    }
    csv_file_close();                  // Close any open CSV files
    gps_manager_deinit(&g_gpsManager); // Clean up GPS if active
    wifi_manager_stop_monitor_mode();  // Stop any active monitoring
    wifi_manager_stop_deauth_station();
    wifi_manager_stop_deauth();
    wifi_manager_stop_dhcpstarve();
    printf("Stopped activities.\nClosed files.\n");
    TERMINAL_VIEW_ADD_TEXT("Stopped activities.\nClosed files.\n");
}

void handle_dial_command(int argc, char **argv) {
    // Usage: dial [device_name]
    if (argc > 2) {
        printf("Usage: %s [device_name]\n", argv[0]);
        TERMINAL_VIEW_ADD_TEXT("Usage: %s [device_name]\n", argv[0]);
        return;
    }
    // If a device name is provided, set it before discovery
    if (argc == 2) {
        dial_manager_set_device_name(argv[1]);
    }
    xTaskCreate(&discover_task, "discover_task", 10240, NULL, 5, NULL);
}

void handle_wifi_connection(int argc, char **argv) {
    const char *ssid;
    const char *password;
    if (argc == 1) {
        // No args: use saved NVS credentials
        ssid = settings_get_sta_ssid(&G_Settings);
        password = settings_get_sta_password(&G_Settings);
        if (ssid == NULL || strlen(ssid) == 0) {
            printf("No saved SSID. Usage: %s \"<SSID>\" [\"<PASSWORD>\"]\n", argv[0]);
            TERMINAL_VIEW_ADD_TEXT("No saved SSID. Usage: %s \"<SSID>\" [\"<PASSWORD>\"]\n", argv[0]);
            return;
        }
        printf("Connecting using saved credentials: %s\n", ssid);
        TERMINAL_VIEW_ADD_TEXT("Connecting using saved credentials: %s\n", ssid);
    } else {
        char ssid_buffer[128] = {0};
        char password_buffer[128] = {0};
        int i = 1;
        // SSID parsing
        if (argv[1][0] == '"') {
            char *dest = ssid_buffer;
            bool found_end = false;
            strncpy(dest, &argv[1][1], sizeof(ssid_buffer) - 1);
            dest += strlen(&argv[1][1]);
            if (argv[1][strlen(argv[1]) - 1] == '"') {
                ssid_buffer[strlen(ssid_buffer) - 1] = '\0';
                found_end = true;
            }
            i = 2;
            while (!found_end && i < argc) {
                *dest++ = ' ';
                if (strchr(argv[i], '"')) {
                    size_t len = strchr(argv[i], '"') - argv[i];
                    strncpy(dest, argv[i], len);
                    dest[len] = '\0';
                    found_end = true;
                } else {
                    strncpy(dest, argv[i], sizeof(ssid_buffer) - (dest - ssid_buffer) - 1);
                    dest += strlen(argv[i]);
                }
                i++;
            }
            if (!found_end) {
                printf("Error: Missing closing quote for SSID\n");
                TERMINAL_VIEW_ADD_TEXT("Error: Missing closing quote for SSID\n");
                return;
            }
            ssid = ssid_buffer;
        } else {
            ssid = argv[1];
            i = 2;
        }
        // Password parsing
        if (i < argc) {
            if (argv[i][0] == '"') {
                char *dest = password_buffer;
                bool found_end = false;
                strncpy(dest, &argv[i][1], sizeof(password_buffer) - 1);
                dest += strlen(&argv[i][1]);
                if (argv[i][strlen(argv[i]) - 1] == '"') {
                    password_buffer[strlen(password_buffer) - 1] = '\0';
                    found_end = true;
                }
                i++;
                while (!found_end && i < argc) {
                    *dest++ = ' ';
                    if (strchr(argv[i], '"')) {
                        size_t len = strchr(argv[i], '"') - argv[i];
                        strncpy(dest, argv[i], len);
                        dest[len] = '\0';
                        found_end = true;
                    } else {
                        strncpy(dest, argv[i], sizeof(password_buffer) - (dest - password_buffer) - 1);
                        dest += strlen(argv[i]);
                    }
                    i++;
                }
                if (!found_end) {
                    printf("Error: Missing closing quote for password\n");
                    TERMINAL_VIEW_ADD_TEXT("Error: Missing closing quote for password\n");
                    return;
                }
                password = password_buffer;
            } else {
                password = argv[i];
            }
        } else {
            password = "";
        }
        // Save provided credentials to NVS
        settings_set_sta_ssid(&G_Settings, ssid);
        settings_set_sta_password(&G_Settings, password);
        settings_save(&G_Settings);
    }
    wifi_manager_connect_wifi(ssid, password);

    if (VisualizerHandle == NULL) {
#ifdef WITH_SCREEN
        xTaskCreate(screen_music_visualizer_task, "udp_server", 4096, NULL, 5, &VisualizerHandle);
#else
        xTaskCreate(animate_led_based_on_amplitude, "udp_server", 4096, NULL, 5, &VisualizerHandle);
#endif
    }

#ifdef CONFIG_HAS_RTC_CLOCK
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_init();
#endif
}

#ifndef CONFIG_IDF_TARGET_ESP32S2

void handle_ble_scan_cmd(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "-f") == 0) {
        printf("Starting Find the Flippers.\n");
        TERMINAL_VIEW_ADD_TEXT("Starting Find the Flippers.\n");
        ble_start_find_flippers();
        return;
    }

    if (argc > 1 && strcmp(argv[1], "-ds") == 0) {
        printf("Starting BLE Spam Detector.\n");
        TERMINAL_VIEW_ADD_TEXT("Starting BLE Spam Detector.\n");
        ble_start_blespam_detector();
        return;
    }

    if (argc > 1 && strcmp(argv[1], "-a") == 0) {
        printf("Starting AirTag Scanner.\n");
        TERMINAL_VIEW_ADD_TEXT("Starting AirTag Scanner.\n");
        ble_start_airtag_scanner();
        return;
    }

    if (argc > 1 && strcmp(argv[1], "-r") == 0) {
        printf("Scanning for Raw Packets\n");
        TERMINAL_VIEW_ADD_TEXT("Scanning for Raw Packets\n");
        ble_start_raw_ble_packetscan();
        return;
    }

    if (argc > 1 && strcmp(argv[1], "-s") == 0) {
        printf("Stopping BLE Scan.\n");
        TERMINAL_VIEW_ADD_TEXT("Stopping BLE Scan.\n");
        ble_stop();
        return;
    }

    printf("Invalid Command Syntax.\n");
    TERMINAL_VIEW_ADD_TEXT("Invalid Command Syntax.\n");
}

#endif

void handle_start_portal(int argc, char **argv) {
    if (argc < 3 || argc > 4) { // Accept 3 or 4 arguments
        printf("Usage: %s <FilePath> <AP_SSID> [PSK]\n", argv[0]);
        TERMINAL_VIEW_ADD_TEXT("Usage: %s <FilePath> <AP_SSID> [PSK]\n", argv[0]);
        TERMINAL_VIEW_ADD_TEXT("PSK is optional for an open AP.\n");
        return;
    }
    const char *url = argv[1];
    const char *ap_ssid = argv[2];
    const char *psk = (argc == 4) ? argv[3] : ""; // Set PSK to empty if not provided
    if (strlen(url) >= MAX_PORTAL_PATH_LEN) {
        printf("Error: Provided Path is too long.\n");
        TERMINAL_VIEW_ADD_TEXT("Error: Path too long.\n");
        return;
    }
    char final_url_or_path[MAX_PORTAL_PATH_LEN];
    strcpy(final_url_or_path, url);

    // Only prepend /mnt/ if it's not the default portal and doesn't already start with /mnt/
    if (strcmp(url, "default") != 0 && strncmp(final_url_or_path, "/mnt/", 5) != 0) {
        const char *prefix = "/mnt/";
        size_t prefix_len = strlen(prefix);
        size_t current_len = strlen(final_url_or_path);
        if (current_len + prefix_len >= MAX_PORTAL_PATH_LEN) {
            printf("Error: Path too long after prepending %s.\n", prefix);
            TERMINAL_VIEW_ADD_TEXT("Error: Path too long.\n");
            return;
        }
        memmove(final_url_or_path + prefix_len, final_url_or_path, current_len + 1);
        memcpy(final_url_or_path, prefix, prefix_len);
        printf("Prepended %s to path: %s\n", prefix, final_url_or_path);
        TERMINAL_VIEW_ADD_TEXT("Prepended %s: %s\n", prefix, final_url_or_path);
    }
    const char *domain = settings_get_portal_domain(&G_Settings);
    printf("Starting portal with AP_SSID: %s, PSK: %s, Domain: %s\n", ap_ssid, psk, domain ? domain : "(default)");
    TERMINAL_VIEW_ADD_TEXT("Starting portal...\nAP: %s\nPSK: %s\nDomain: %s\n", ap_ssid, (strlen(psk) > 0 ? psk : "<Open>"), domain ? domain : "(default)");
    wifi_manager_start_evil_portal(final_url_or_path, NULL, psk, ap_ssid, domain);
}

bool ip_str_to_bytes(const char *ip_str, uint8_t *ip_bytes) {
    int ip[4];
    if (sscanf(ip_str, "%d.%d.%d.%d", &ip[0], &ip[1], &ip[2], &ip[3]) == 4) {
        for (int i = 0; i < 4; i++) {
            if (ip[i] < 0 || ip[i] > 255)
                return false;
            ip_bytes[i] = (uint8_t)ip[i];
        }
        return true;
    }
    return false;
}

bool mac_str_to_bytes(const char *mac_str, uint8_t *mac_bytes) {
    int mac[6];
    if (sscanf(mac_str, "%x:%x:%x:%x:%x:%x", &mac[0], &mac[1], &mac[2], &mac[3], &mac[4],
               &mac[5]) == 6) {
        for (int i = 0; i < 6; i++) {
            if (mac[i] < 0 || mac[i] > 255)
                return false;
            mac_bytes[i] = (uint8_t)mac[i];
        }
        return true;
    }
    return false;
}

void encrypt_tp_link_command(const char *input, uint8_t *output, size_t len) {
    uint8_t key = 171;
    for (size_t i = 0; i < len; i++) {
        output[i] = input[i] ^ key;
        key = output[i];
    }
}

void decrypt_tp_link_response(const uint8_t *input, char *output, size_t len) {
    uint8_t key = 171;
    for (size_t i = 0; i < len; i++) {
        output[i] = input[i] ^ key;
        key = input[i];
    }
}

void handle_tp_link_test(int argc, char **argv) {
    if (argc != 2) {
        printf("Usage: tp_link_test <on|off|loop>\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: tp_link_test <on|off|loop>\n");
        return;
    }

    bool isloop = false;

    if (strcmp(argv[1], "loop") == 0) {
        isloop = true;
    } else if (strcmp(argv[1], "on") != 0 && strcmp(argv[1], "off") != 0) {
        printf("Invalid argument. Use 'on', 'off', or 'loop'.\n");
        return;
    }

    struct sockaddr_in dest_addr;
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_addr.s_addr = inet_addr("255.255.255.255");
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(9999);

    int iterations = isloop ? 10 : 1;

    for (int i = 0; i < iterations; i++) {
        const char *command;
        if (isloop) {
            command = (i % 2 == 0) ? "{\"system\":{\"set_relay_state\":{\"state\":1}}}" : // "on"
                          "{\"system\":{\"set_relay_state\":{\"state\":0}}}";             // "off"
        } else {

            command = (strcmp(argv[1], "on") == 0)
                          ? "{\"system\":{\"set_relay_state\":{\"state\":1}}}"
                          : "{\"system\":{\"set_relay_state\":{\"state\":0}}}";
        }

        uint8_t encrypted_command[128];
        memset(encrypted_command, 0, sizeof(encrypted_command));

        size_t command_len = strlen(command);
        if (command_len >= sizeof(encrypted_command)) {
            printf("Command too large to encrypt\n");
            return;
        }

        encrypt_tp_link_command(command, encrypted_command, command_len);

        int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock < 0) {
            printf("Failed to create socket: errno %d\n", errno);
            return;
        }

        int broadcast = 1;
        setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));

        int err = sendto(sock, encrypted_command, command_len, 0, (struct sockaddr *)&dest_addr,
                         sizeof(dest_addr));
        if (err < 0) {
            printf("Error occurred during sending: errno %d\n", errno);
            TERMINAL_VIEW_ADD_TEXT("Error occurred during sending: errno %d\n", errno);
            close(sock);
            return;
        }

        printf("Broadcast message sent: %s\n", command);
        TERMINAL_VIEW_ADD_TEXT("Broadcast message sent: %s\n", command);

        struct timeval timeout = {2, 0};
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        uint8_t recv_buf[128];
        socklen_t addr_len = sizeof(dest_addr);
        int len = recvfrom(sock, recv_buf, sizeof(recv_buf) - 1, 0, (struct sockaddr *)&dest_addr,
                           &addr_len);
        if (len < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                printf("No response from any device\n");
                TERMINAL_VIEW_ADD_TEXT("No response from any device\n");
            } else {
                printf("Error receiving response: errno %d\n", errno);
                TERMINAL_VIEW_ADD_TEXT("Error receiving response: errno %d\n", errno);
            }
        } else {
            recv_buf[len] = 0;
            char decrypted_response[128];
            decrypt_tp_link_response(recv_buf, decrypted_response, len);
            decrypted_response[len] = 0;
            printf("Response: %s\n", decrypted_response);
        }

        close(sock);

        if (isloop && i < 9) {
            vTaskDelay(pdMS_TO_TICKS(700));
        }
    }
}

void handle_ip_lookup(int argc, char **argv) {
    printf("Starting IP lookup...\n");
    TERMINAL_VIEW_ADD_TEXT("Starting IP lookup...\n");
    wifi_manager_start_ip_lookup();
}

void handle_capture_scan(int argc, char **argv) {
    if (argc != 2) {
        printf("Error: Incorrect number of arguments.\n");
        TERMINAL_VIEW_ADD_TEXT("Error: Incorrect number of arguments.\n");
        return;
    }

    char *capturetype = argv[1];

    if (capturetype == NULL || capturetype[0] == '\0') {
        printf("Error: Capture Type cannot be empty.\n");
        TERMINAL_VIEW_ADD_TEXT("Error: Capture Type cannot be empty.\n");
        return;
    }

    if (strcmp(capturetype, "-probe") == 0) {
        printf("Starting probe request\npacket capture...\n");
        TERMINAL_VIEW_ADD_TEXT("Starting probe request\npacket capture...\n");
        int err = pcap_file_open("probescan", PCAP_CAPTURE_WIFI);

        if (err != ESP_OK) {
            printf("Error: pcap failed to open\n");
            TERMINAL_VIEW_ADD_TEXT("Error: pcap failed to open\n");
            return;
        }
        wifi_manager_start_monitor_mode(wifi_probe_scan_callback);
    }

    if (strcmp(capturetype, "-deauth") == 0) {
        int err = pcap_file_open("deauthscan", PCAP_CAPTURE_WIFI);

        if (err != ESP_OK) {
            printf("Error: pcap failed to open\n");
            TERMINAL_VIEW_ADD_TEXT("Error: pcap failed to open\n");
            return;
        }
        wifi_manager_start_monitor_mode(wifi_deauth_scan_callback);
    }

    if (strcmp(capturetype, "-beacon") == 0) {
        printf("Starting beacon\npacket capture...\n");
        TERMINAL_VIEW_ADD_TEXT("Starting beacon\npacket capture...\n");
        int err = pcap_file_open("beaconscan", PCAP_CAPTURE_WIFI);

        if (err != ESP_OK) {
            printf("Error: pcap failed to open\n");
            TERMINAL_VIEW_ADD_TEXT("Error: pcap failed to open\n");
            return;
        }
        wifi_manager_start_monitor_mode(wifi_beacon_scan_callback);
    }

    if (strcmp(capturetype, "-raw") == 0) {
        printf("Starting raw\npacket capture...\n");
        TERMINAL_VIEW_ADD_TEXT("Starting raw\npacket capture...\n");
        int err = pcap_file_open("rawscan", PCAP_CAPTURE_WIFI);

        if (err != ESP_OK) {
            printf("Error: pcap failed to open\n");
            TERMINAL_VIEW_ADD_TEXT("Error: pcap failed to open\n");
            return;
        }
        wifi_manager_start_monitor_mode(wifi_raw_scan_callback);
    }

    if (strcmp(capturetype, "-eapol") == 0) {
        printf("Starting EAPOL\npacket capture...\n");
        TERMINAL_VIEW_ADD_TEXT("Starting EAPOL\npacket capture...\n");
        int err = pcap_file_open("eapolscan", PCAP_CAPTURE_WIFI);

        if (err != ESP_OK) {
            printf("Error: pcap failed to open\n");
            TERMINAL_VIEW_ADD_TEXT("Error: pcap failed to open\n");
            return;
        }
        wifi_manager_start_monitor_mode(wifi_eapol_scan_callback);
    }

    if (strcmp(capturetype, "-pwn") == 0) {
        printf("Starting PWN\npacket capture...\n");
        TERMINAL_VIEW_ADD_TEXT("Starting PWN\npacket capture...\n");
        int err = pcap_file_open("pwnscan", PCAP_CAPTURE_WIFI);

        if (err != ESP_OK) {
            printf("Error: pcap failed to open\n");
            TERMINAL_VIEW_ADD_TEXT("Error: pcap failed to open\n");
            return;
        }
        wifi_manager_start_monitor_mode(wifi_pwn_scan_callback);
    }

    if (strcmp(capturetype, "-wps") == 0) {
        printf("Starting WPS\npacket capture...\n");
        TERMINAL_VIEW_ADD_TEXT("Starting WPS\npacket capture...\n");
        int err = pcap_file_open("wpsscan", PCAP_CAPTURE_WIFI);

        should_store_wps = 0;

        if (err != ESP_OK) {
            printf("Error: pcap failed to open\n");
            TERMINAL_VIEW_ADD_TEXT("Error: pcap failed to open\n");
            return;
        }
        wifi_manager_start_monitor_mode(wifi_wps_detection_callback);
    }

    if (strcmp(capturetype, "-stop") == 0) {
        printf("Stopping packet capture...\n");
        TERMINAL_VIEW_ADD_TEXT("Stopping packet capture...\n");
        wifi_manager_stop_monitor_mode();
#ifndef CONFIG_IDF_TARGET_ESP32S2
        ble_stop();
        ble_stop_skimmer_detection();
#endif
        pcap_file_close();
    }
#ifndef CONFIG_IDF_TARGET_ESP32S2
    if (strcmp(capturetype, "-ble") == 0) {
        printf("Starting BLE packet capture...\n");
        TERMINAL_VIEW_ADD_TEXT("Starting BLE packet capture...\n");
        ble_start_capture();
    }

    if (strcmp(capturetype, "-skimmer") == 0) {
        printf("Skimmer detection started.\n");
        TERMINAL_VIEW_ADD_TEXT("Skimmer detection started.\n");
        int err = pcap_file_open("skimmer_scan", PCAP_CAPTURE_BLUETOOTH);
        if (err != ESP_OK) {
            printf("Warning: PCAP capture failed to start\n");
            TERMINAL_VIEW_ADD_TEXT("Warning: PCAP capture failed to start\n");
        } else {
            printf("PCAP capture started\nMonitoring devices\n");
            TERMINAL_VIEW_ADD_TEXT("PCAP capture started\nMonitoring devices\n");
        }
        // Start skimmer detection
        ble_start_skimmer_detection();

    }
#endif
}

void stop_portal(int argc, char **argv) {
    wifi_manager_stop_evil_portal();
    printf("Stopping evil portal...\n");
    TERMINAL_VIEW_ADD_TEXT("Stopping evil portal...\n");
}

void handle_reboot(int argc, char **argv) {
    printf("Rebooting system...\n");
    TERMINAL_VIEW_ADD_TEXT("Rebooting system...\n");
    esp_restart();
}

void handle_startwd(int argc, char **argv) {
    bool stop_flag = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0) {
            stop_flag = true;
            break;
        }
    }

    if (stop_flag) {
        gps_manager_deinit(&g_gpsManager);
        wifi_manager_stop_monitor_mode();
        csv_flush_buffer_to_file();
        csv_file_close();
        printf("Wardriving stopped.\n");
        TERMINAL_VIEW_ADD_TEXT("Wardriving stopped.\n");
    } else {
        gps_manager_init(&g_gpsManager);
        if (sd_card_exists("/mnt/ghostesp/gps")) {
            esp_err_t err = csv_file_open("wardriving");
            if (err != ESP_OK) {
                printf("Failed to open CSV for wardriving\n");
                TERMINAL_VIEW_ADD_TEXT("Failed to open CSV for wardriving\n");
            }
        }
        wifi_manager_start_monitor_mode(wardriving_scan_callback);
        printf("Wardriving started.\n");
        TERMINAL_VIEW_ADD_TEXT("Wardriving started.\n");
    }
}

void handle_timezone_cmd(int argc, char **argv) {
    if (argc != 2) {
        printf("Usage: timezone <TZ_STRING>\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: timezone <TZ_STRING>\n");
        return;
    }
    const char *tz = argv[1];
    settings_set_timezone_str(&G_Settings, tz);
    settings_save(&G_Settings);
    setenv("TZ", tz, 1);
    tzset();
    printf("Timezone set to: %s\n", tz);
    TERMINAL_VIEW_ADD_TEXT("Timezone set to: %s\n", tz);
}

void handle_scan_ports(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage:\n");
        printf("scanports local [-C/-A/start_port-end_port]\n");
        printf("scanports [IP] [-C/-A/start_port-end_port]\n");
        return;
    }

    bool is_local = strcmp(argv[1], "local") == 0;
    const char *target_ip = NULL;
    const char *port_arg = NULL;

    // Parse arguments based on whether it's a local scan
    if (is_local) {
        if (argc < 3) {
            printf("Missing port argument for local scan\n");
            return;
        }
        port_arg = argv[2];
    } else {
        if (argc < 3) {
            printf("Missing port argument for IP scan\n");
            return;
        }
        target_ip = argv[1];
        port_arg = argv[2];
    }

    if (is_local) {
        wifi_manager_scan_subnet();
        return;
    }

    host_result_t result;
    if (strcmp(port_arg, "-C") == 0) {
        scan_ports_on_host(target_ip, &result);
        if (result.num_open_ports > 0) {
            printf("Open ports on %s:\n", target_ip);
            for (int i = 0; i < result.num_open_ports; i++) {
                printf("Port %d\n", result.open_ports[i]);
            }
        }
    } else {
        int start_port, end_port;
        if (strcmp(port_arg, "-A") == 0) {
            start_port = 1;
            end_port = 65535;
        } else if (sscanf(port_arg, "%d-%d", &start_port, &end_port) != 2 || start_port < 1 ||
                   end_port > 65535 || start_port > end_port) {
            printf("Invalid port range\n");
            return;
        }
        scan_ip_port_range(target_ip, start_port, end_port);
    }
}

void handle_crash(int argc, char **argv) {
    int *ptr = NULL;
    *ptr = 42;
}

void handle_help(int argc, char **argv) {
    printf("\n Ghost ESP Commands:\n\n");
    TERMINAL_VIEW_ADD_TEXT("\n Ghost ESP Commands:\n\n");

    printf("help\n");
    printf("    Description: Display this help message.\n");
    printf("    Usage: help\n\n");
    TERMINAL_VIEW_ADD_TEXT("help\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Display this help message.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: help\n\n");

    printf("scanap\n");
    printf("    Description: Start a Wi-Fi access point (AP) scan.\n");
    printf("    Usage: scanap [seconds]\n\n");
    TERMINAL_VIEW_ADD_TEXT("scanap\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Start a Wi-Fi access point (AP) scan.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: scanap [seconds]\n\n");

    printf("scansta\n");
    printf("    Description: Start scanning for Wi-Fi stations (hops channels).\n");
    printf("    Usage: scansta\n\n");
    TERMINAL_VIEW_ADD_TEXT("scansta\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Start scanning for Wi-Fi stations (hops channels).\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: scansta\n\n");

    printf("stopscan\n");
    printf("    Description: Stop any ongoing Wi-Fi scan.\n");
    printf("    Usage: stopscan\n\n");
    TERMINAL_VIEW_ADD_TEXT("stopscan\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Stop any ongoing Wi-Fi scan.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: stopscan\n\n");

    printf("attack\n");
    printf("    Description: Launch an attack (e.g., deauthentication attack).\n");
    printf("    Usage: attack -d\n");
    printf("    Arguments:\n");
    printf("        -d  : Start deauth attack\n\n");
    TERMINAL_VIEW_ADD_TEXT("attack\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Launch an attack (e.g., deauthentication attack).\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: attack -d\n");
    TERMINAL_VIEW_ADD_TEXT("    Arguments:\n");
    TERMINAL_VIEW_ADD_TEXT("        -d  : Start deauth attack\n\n");

    printf("list\n");
    printf("    Description: List Wi-Fi scan results or connected stations.\n");
    printf("    Usage: list -a | list -s | list -airtags\n");
    printf("    Arguments:\n");
    printf("        -a  : Show access points from Wi-Fi scan\n");
    printf("        -s  : List connected stations\n");
    printf("        -airtags: List discovered AirTags\n\n");
    TERMINAL_VIEW_ADD_TEXT("list\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: List Wi-Fi scan results or connected stations.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: list -a | list -s | list -airtags\n");
    TERMINAL_VIEW_ADD_TEXT("    Arguments:\n");
    TERMINAL_VIEW_ADD_TEXT("        -a  : Show access points from Wi-Fi scan\n");
    TERMINAL_VIEW_ADD_TEXT("        -s  : List connected stations\n");
    TERMINAL_VIEW_ADD_TEXT("        -airtags: List discovered AirTags\n\n");

    printf("beaconspam\n");
    printf("    Description: Start beacon spam with different modes.\n");
    printf("    Usage: beaconspam [OPTION]\n");
    printf("    Arguments:\n");
    printf("        -r   : Start random beacon spam\n");
    printf("        -rr  : Start Rickroll beacon spam\n");
    printf("        -l   : Start AP List beacon spam\n");
    printf("        [SSID]: Use specified SSID for beacon spam\n\n");
    TERMINAL_VIEW_ADD_TEXT("beaconspam\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Start beacon spam with different modes.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: beaconspam [OPTION]\n");
    TERMINAL_VIEW_ADD_TEXT("    Arguments:\n");
    TERMINAL_VIEW_ADD_TEXT("        -r   : Start random beacon spam\n");
    TERMINAL_VIEW_ADD_TEXT("        -rr  : Start Rickroll beacon spam\n");
    TERMINAL_VIEW_ADD_TEXT("        -l   : Start AP List beacon spam\n");
    TERMINAL_VIEW_ADD_TEXT("        [SSID]: Use specified SSID for beacon spam\n\n");

    printf("stopspam\n");
    printf("    Description: Stop ongoing beacon spam.\n");
    printf("    Usage: stopspam\n\n");
    TERMINAL_VIEW_ADD_TEXT("stopspam\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Stop ongoing beacon spam.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: stopspam\n\n");

    printf("stopdeauth\n");
    printf("    Description: Stop ongoing deauthentication attack.\n");
    printf("    Usage: stopdeauth\n\n");
    TERMINAL_VIEW_ADD_TEXT("stopdeauth\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Stop ongoing deauthentication attack.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: stopdeauth\n\n");

    printf("select\n");
    printf("    Description: Select an access point, station, or AirTag by index from the scan "
           "results.\n");
    printf("    Usage: select -a <num> | select -s <num> | select -airtag <num>\n");
    printf("    Arguments:\n");
    printf("        -a      : AP selection index\n");
    printf("        -s      : Station selection index\n");
    printf("        -airtag : AirTag selection index\n\n");
    TERMINAL_VIEW_ADD_TEXT("select\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Select an access point, station, or AirTag by index "
                           "from the scan results.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: select -a <num> | select -s <num> | select -airtag <num>\n");
    TERMINAL_VIEW_ADD_TEXT("    Arguments:\n");
    TERMINAL_VIEW_ADD_TEXT("        -a      : AP selection index\n");
    TERMINAL_VIEW_ADD_TEXT("        -s      : Station selection index\n");
    TERMINAL_VIEW_ADD_TEXT("        -airtag : AirTag selection index\n\n");

    printf("startportal\n");
    printf("    Description: Start an Evil Portal using a local file or the default embedded page.\n");
    printf("                 /mnt/ prefix is added automatically to file paths if missing.\n");
    printf("    Usage: startportal [FilePath] [AP_SSID] [PSK]\n");
    printf("           PSK is optional for an open network.\n");
    printf("    Use 'default' as the file path for the default Evil Portal.");
    TERMINAL_VIEW_ADD_TEXT("startportal\n");
    TERMINAL_VIEW_ADD_TEXT("    Desc: Start Evil Portal.\n");
    TERMINAL_VIEW_ADD_TEXT("          Use 'default' as the file path for the default Evil Portal.\n");
    TERMINAL_VIEW_ADD_TEXT("          /mnt/ added to paths automatically.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: startportal [FilePath] [AP_SSID] [PSK]\n");
    TERMINAL_VIEW_ADD_TEXT("           PSK is optional for an open network.\n");


    printf("stopportal\n");
    printf("    Description: Stop Evil Portal\n");
    printf("    Usage: stopportal\n\n");
    TERMINAL_VIEW_ADD_TEXT("stopportal\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Stop Evil Portal\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: stopportal\n\n");

#ifndef CONFIG_IDF_TARGET_ESP32S2
    printf("blescan\n");
    printf("    Description: Handle BLE scanning with various modes.\n");
    printf("    Usage: blescan [OPTION]\n");
    printf("    Arguments:\n");
    printf("        -f   : Start 'Find the Flippers' mode\n");
    printf("        -ds  : Start BLE spam detector\n");
    printf("        -a   : Start AirTag scanner\n");
    printf("        -r   : Scan for raw BLE packets\n");
    printf("        -s   : Stop BLE scanning\n\n");
    TERMINAL_VIEW_ADD_TEXT("blescan\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Handle BLE scanning with various modes.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: blescan [OPTION]\n");
    TERMINAL_VIEW_ADD_TEXT("    Arguments:\n");
    printf("        -f   : Start 'Find the Flippers' mode\n");
    TERMINAL_VIEW_ADD_TEXT("        -ds  : Start BLE spam detector\n");
    TERMINAL_VIEW_ADD_TEXT("        -a   : Start AirTag scanner\n");
    TERMINAL_VIEW_ADD_TEXT("        -r   : Scan for raw BLE packets\n");
    TERMINAL_VIEW_ADD_TEXT("        -s   : Stop BLE scanning\n\n");
#endif

    printf("capture\n");
    printf("    Description: Start a WiFi Capture (Requires SD Card or Flipper)\n");
    printf("    Usage: capture [OPTION]\n");
    printf("    Arguments:\n");
    printf("        -probe   : Start Capturing Probe Packets\n");
    printf("        -beacon  : Start Capturing Beacon Packets\n");
    printf("        -deauth   : Start Capturing Deauth Packets\n");
    printf("        -raw   :   Start Capturing Raw Packets\n");
    printf("        -wps   :   Start Capturing WPS Packets and there Auth Type");
    printf("        -pwn   :   Start Capturing Pwnagotchi Packets");
    printf("        -stop   : Stops the active capture\n\n");
    TERMINAL_VIEW_ADD_TEXT("capture\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Start a WiFi Capture (Requires SD Card or Flipper)\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: capture [OPTION]\n");
    TERMINAL_VIEW_ADD_TEXT("    Arguments:\n");
    TERMINAL_VIEW_ADD_TEXT("        -probe   : Start Capturing Probe Packets\n");
    TERMINAL_VIEW_ADD_TEXT("        -beacon  : Start Capturing Beacon Packets\n");
    TERMINAL_VIEW_ADD_TEXT("        -deauth   : Start Capturing Deauth Packets\n");
    TERMINAL_VIEW_ADD_TEXT("        -raw   :   Start Capturing Raw Packets\n");
    TERMINAL_VIEW_ADD_TEXT("        -wps   :   Start Capturing WPS Packets and there Auth Type");
    TERMINAL_VIEW_ADD_TEXT("        -pwn   :   Start Capturing Pwnagotchi Packets");
    TERMINAL_VIEW_ADD_TEXT("        -stop   : Stops the active capture\n\n");

    printf("connect\n");
    printf("    Description: Connects to Specific WiFi Network and saves credentials.\n");
    printf("    Usage: connect <SSID> [Password]\n");
    TERMINAL_VIEW_ADD_TEXT("connect\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Connects to Specific WiFi Network and saves credentials.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: connect <SSID> [Password]\n");

    printf("dialconnect\n");
    printf("    Description: Cast a Random Youtube Video on all Smart TV's on "
           "your LAN (Requires You to Run Connect First)\n");
    printf("    Usage: dialconnect\n");
    TERMINAL_VIEW_ADD_TEXT("dialconnect\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Cast a Random Youtube Video on all Smart TV's on your "
                           "LAN (Requires You to Run Connect First)\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: dialconnect\n");

    printf("powerprinter\n");
    printf("    Description: Print Custom Text to a Printer on your LAN "
           "(Requires You to Run Connect First)\n");
    printf("    Usage: powerprinter <Printer IP> <Text> <FontSize> <alignment>\n");
    printf("    aligment options: CM = Center Middle, TL = Top Left, TR = Top "
           "Right, BR = Bottom Right, BL = Bottom Left\n\n");
    TERMINAL_VIEW_ADD_TEXT("powerprinter\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Print Custom Text to a Printer on "
                           "your LAN (Requires You to Run Connect First)\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: powerprinter <Printer IP> <Text> <FontSize> <alignment>\n");
    TERMINAL_VIEW_ADD_TEXT("    aligment options: CM = Center Middle, TL = Top Left, TR = Top "
                           "Right, BR = Bottom Right, BL = Bottom Left\n\n");

    printf("blewardriving\n");
    printf("    Description: Start/Stop BLE wardriving with GPS logging\n");
    printf("    Usage: blewardriving [-s]\n");
    printf("    Arguments:\n");
    printf("        -s  : Stop BLE wardriving\n\n");
    TERMINAL_VIEW_ADD_TEXT("blewardriving\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Start/Stop BLE wardriving with GPS logging\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: blewardriving [-s]\n");
    TERMINAL_VIEW_ADD_TEXT("    Arguments:\n");
    TERMINAL_VIEW_ADD_TEXT("        -s  : Stop BLE wardriving\n\n");

    printf("pineap\n");
    printf("    Description: Start/Stop detecting WiFi Pineapples.\n");
    printf("    Usage: pineap [-s]\n");
    printf("    Arguments:\n");
    printf("        -s  : Stop PineAP detection\n\n");
    TERMINAL_VIEW_ADD_TEXT("pineap\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Start/Stop detecting WiFi Pineapples.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: pineap [-s]\n");
    TERMINAL_VIEW_ADD_TEXT("    Arguments:\n");
    TERMINAL_VIEW_ADD_TEXT("        -s  : Stop PineAP detection\n\n");

    printf("Port Scanner\n");
    printf("    Description: Scan ports on local subnet or specific IP\n");
    printf("    Usage: scanports local [-C/-A/start_port-end_port]\n");
    printf("           scanports [IP] [-C/-A/start_port-end_port]\n");
    printf("    Arguments:\n");
    printf("        -C  : Scan common ports only\n");
    printf("        -A  : Scan all ports (1-65535)\n");
    printf("        start_port-end_port : Custom port range (e.g. 80-443)\n\n");
    TERMINAL_VIEW_ADD_TEXT("Port Scanner\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Scan ports on local subnet or specific IP\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: scanports local [-C/-A/start_port-end_port]\n");
    TERMINAL_VIEW_ADD_TEXT("           scanports [IP] [-C/-A/start_port-end_port]\n");
    TERMINAL_VIEW_ADD_TEXT("    Arguments:\n");
    TERMINAL_VIEW_ADD_TEXT("        -C  : Scan common ports only\n");
    TERMINAL_VIEW_ADD_TEXT("        -A  : Scan all ports (1-65535)\n");
    TERMINAL_VIEW_ADD_TEXT("        start_port-end_port : Custom port range (e.g. 80-443)\n\n");

    printf("congestion\n");
    printf("    Description: Display Wi-Fi channel congestion chart.\n");
    printf("    Usage: congestion\n\n");
    TERMINAL_VIEW_ADD_TEXT("congestion\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Display Wi-Fi channel congestion chart.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: congestion\n\n");

    printf("apcred\n");
    printf("    Description: Change or reset the GhostNet AP credentials\n");
    printf("    Usage: apcred <ssid> <password>\n");
    printf("           apcred -r (reset to defaults)\n");
    printf("    Arguments:\n");
    printf("        <ssid>     : New SSID for the AP\n");
    printf("        <password> : New password (min 8 characters)\n");
    printf("        -r        : Reset to default (GhostNet/GhostNet)\n\n");
    TERMINAL_VIEW_ADD_TEXT("apcred\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Change or reset the GhostNet AP credentials\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: apcred <ssid> <password>\n");
    TERMINAL_VIEW_ADD_TEXT("           apcred -r (reset to defaults)\n");
    TERMINAL_VIEW_ADD_TEXT("    Arguments:\n");
    TERMINAL_VIEW_ADD_TEXT("        <ssid>     : New SSID for the AP\n");
    TERMINAL_VIEW_ADD_TEXT("        <password> : New password (min 8 characters)\n");
    TERMINAL_VIEW_ADD_TEXT("        -r        : Reset to default (GhostNet/GhostNet)\n\n");

    printf("rgbmode\n");
    printf("    Description: Control LED effects (rainbow, police, strobe, off)\n");
    printf("    Usage: rgbmode <rainbow|police|strobe|off|color>\n");
    TERMINAL_VIEW_ADD_TEXT("rgbmode\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Control LED effects (rainbow, police, strobe, off)\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: rgbmode <rainbow|police|strobe|off|color>\n");

    printf("setrgbpins\n");
    printf("    Description: Change RGB LED pins\n");
    printf("    Usage: setrgbpins <red> <green> <blue>\n");
    printf("           (use same value for all pins for single-pin LED strips)\n\n");
    TERMINAL_VIEW_ADD_TEXT("setrgbpins\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Change RGB LED pins\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: setrgbpins <red> <green> <blue>\n");
    TERMINAL_VIEW_ADD_TEXT("           (use same value for all pins for single-pin LED strips)\n\n");

    // SD Card Commands Help Text
    printf("\n-- SD Card Pin Configuration --\n");
    printf("Note: SD Card mode (MMC vs SPI) is set at compile time (sdkconfig).\n");
    printf("These commands configure pins for the *active* mode.\n");
    printf("Changing the mode requires recompiling firmware.\n");
    TERMINAL_VIEW_ADD_TEXT("\n-- SD Card Pin Configuration --\n");
    TERMINAL_VIEW_ADD_TEXT("Note: SD Card mode (MMC vs SPI) is set at compile time (sdkconfig).\n");
    TERMINAL_VIEW_ADD_TEXT("These commands configure pins for the *active* mode.\n");
    TERMINAL_VIEW_ADD_TEXT("Changing the mode requires recompiling firmware.\n");

    printf("sd_config\n");
    printf("    Description: Show the currently configured GPIO pins for both SDMMC and SPI modes.\n");
    printf("    Usage: sd_config\n\n");
    TERMINAL_VIEW_ADD_TEXT("sd_config\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Show current SD GPIO pin configuration.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: sd_config\n\n");

    printf("sd_pins_mmc\n");
    printf("    Description: Set GPIO pins for SDMMC mode (1 or 4 bit). Requires restart/reinit.\n");
    printf("                 Only effective if firmware compiled for SDMMC mode.\n");
    printf("    Usage: sd_pins_mmc <clk> <cmd> <d0> <d1> <d2> <d3>\n");
    printf("    Example: sd_pins_mmc 19 18 20 21 22 23\n\n");
    TERMINAL_VIEW_ADD_TEXT("sd_pins_mmc\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Set GPIO pins for SDMMC mode. Requires restart.\n");
    TERMINAL_VIEW_ADD_TEXT("                 Only effective if firmware compiled for SDMMC.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: sd_pins_mmc <clk> <cmd> <d0> <d1> <d2> <d3>\n\n");

    printf("sd_pins_spi\n");
    printf("    Description: Set GPIO pins for SPI mode. Requires restart/reinit.\n");
    printf("                 Only effective if firmware compiled for SPI mode.\n");
    printf("    Usage: sd_pins_spi <cs> <clk> <miso> <mosi>\n");
    printf("    Example: sd_pins_spi 5 18 19 23\n\n");
    TERMINAL_VIEW_ADD_TEXT("sd_pins_spi\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Set GPIO pins for SPI mode. Requires restart.\n");
    TERMINAL_VIEW_ADD_TEXT("                 Only effective if firmware compiled for SPI.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: sd_pins_spi <cs> <clk> <miso> <mosi>\n\n");

    printf("sd_save_config\n");
    printf("    Description: Save the current SD pin configuration (both modes) to the SD card.\n");
    printf("                 Requires SD card to be mounted.\n");
    printf("    Usage: sd_save_config\n\n");
    TERMINAL_VIEW_ADD_TEXT("sd_save_config\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Save current SD pin config to SD card.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: sd_save_config\n\n");

    printf("scanall\n");
    printf("    Description: Perform combined AP and Station scan, display results.\n");
    printf("    Usage: scanall [seconds]\n\n");
    TERMINAL_VIEW_ADD_TEXT("scanall\n");
    TERMINAL_VIEW_ADD_TEXT("    Desc: Combined AP/STA scan & display.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: scanall [seconds]\n\n");

    printf("timezone\n");
    printf("    Description: Set the display timezone for the clock view.\n");
    printf("    Usage: timezone <TZ_STRING>\n\n");
    TERMINAL_VIEW_ADD_TEXT("timezone\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Set the display timezone for the clock view.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: timezone <TZ_STRING>\n\n");

    printf("beaconadd\n");
    printf("    Description: Add an SSID to the beacon spam list.\n");
    printf("    Usage: beaconadd <SSID>\n\n");
    TERMINAL_VIEW_ADD_TEXT("beaconadd\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Add an SSID to the beacon spam list.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: beaconadd <SSID>\n\n");

    printf("beaconremove\n");
    printf("    Description: Remove an SSID from the beacon spam list.\n");
    printf("    Usage: beaconremove <SSID>\n\n");
    TERMINAL_VIEW_ADD_TEXT("beaconremove\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Remove an SSID from the beacon spam list.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: beaconremove <SSID>\n\n");

    printf("beaconclear\n");
    printf("    Description: Clear the beacon spam list.\n");
    printf("    Usage: beaconclear\n\n");
    TERMINAL_VIEW_ADD_TEXT("beaconclear\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Clear the beacon spam list.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: beaconclear\n\n");

    printf("beaconshow\n");
    printf("    Description: Show the current beacon spam list.\n");
    printf("    Usage: beaconshow\n\n");
    TERMINAL_VIEW_ADD_TEXT("beaconshow\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Show the current beacon spam list.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: beaconshow\n\n");

    printf("beaconspamlist\n");
    printf("    Description: Start beacon spamming using the beacon spam list.\n");
    printf("    Usage: beaconspamlist\n\n");
    TERMINAL_VIEW_ADD_TEXT("beaconspamlist\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: Start beacon spamming using the beacon spam list.\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: beaconspamlist\n\n");

    printf("dhcpstarve\n");
    printf("    Description: DHCP starvation flood attack\n");
    printf("    Usage: dhcpstarve start [threads]\n");
    printf("           dhcpstarve stop\n");
    printf("           dhcpstarve display\n\n");
    TERMINAL_VIEW_ADD_TEXT("dhcpstarve\n");
    TERMINAL_VIEW_ADD_TEXT("    Description: DHCP starvation flood attack\n");
    TERMINAL_VIEW_ADD_TEXT("    Usage: dhcpstarve start [threads]\n");
    TERMINAL_VIEW_ADD_TEXT("           dhcpstarve stop\n");
    TERMINAL_VIEW_ADD_TEXT("           dhcpstarve display\n\n");
}

void handle_capture(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: capture [-probe|-beacon|-deauth|-raw|-ble]\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: capture [-probe|-beacon|-deauth|-raw|-ble]\n");
        return;
    }
#ifndef CONFIG_IDF_TARGET_ESP32S2
    if (strcmp(argv[1], "-ble") == 0) {
        printf("Starting BLE packet capture...\n");
        TERMINAL_VIEW_ADD_TEXT("Starting BLE packet capture...\n");
        ble_start_capture();
    }
#endif
}

void handle_gps_info(int argc, char **argv) {
    bool stop_flag = false;
    static TaskHandle_t gps_info_task_handle = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0) {
            stop_flag = true;
            break;
        }
    }

    if (stop_flag) {
        if (gps_info_task_handle != NULL) {
            vTaskDelete(gps_info_task_handle);
            gps_info_task_handle = NULL;
            gps_manager_deinit(&g_gpsManager);
            printf("GPS info display stopped.\n");
            TERMINAL_VIEW_ADD_TEXT("GPS info display stopped.\n");
        }
    } else {
        if (gps_info_task_handle == NULL) {
            gps_manager_init(&g_gpsManager);

            // Wait a brief moment for GPS initialization
            vTaskDelay(pdMS_TO_TICKS(100));

            // Start the info display task
            xTaskCreate(gps_info_display_task, "gps_info", 4096, NULL, 1, &gps_info_task_handle);
            printf("GPS info started.\n");
            TERMINAL_VIEW_ADD_TEXT("GPS info started.\n");
        }
    }
}


#ifndef CONFIG_IDF_TARGET_ESP32S2
void handle_ble_wardriving(int argc, char **argv) {
    bool stop_flag = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0) {
            stop_flag = true;
            break;
        }
    }

    if (stop_flag) {
        ble_stop();
        gps_manager_deinit(&g_gpsManager);
        if (buffer_offset > 0) { // Only flush if there's data in buffer
            csv_flush_buffer_to_file();
        }
        csv_file_close();
        printf("BLE wardriving stopped.\n");
        TERMINAL_VIEW_ADD_TEXT("BLE wardriving stopped.\n");
    } else {
        if (!g_gpsManager.isinitilized) {
            gps_manager_init(&g_gpsManager);
        }

        // Open CSV file for BLE wardriving
        esp_err_t err = csv_file_open("ble_wardriving");
        if (err != ESP_OK) {
            printf("Failed to open CSV file for BLE wardriving\n");
            return;
        }

        ble_register_handler(ble_wardriving_callback);
        ble_start_scanning();
        printf("BLE wardriving started.\n");
        TERMINAL_VIEW_ADD_TEXT("BLE wardriving started.\n");
    }
}
#endif

void handle_pineap_detection(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "-s") == 0) {
        printf("Stopping PineAP detection...\n");
        TERMINAL_VIEW_ADD_TEXT("Stopping PineAP detection...\n");
        stop_pineap_detection();
        wifi_manager_stop_monitor_mode();
        pcap_file_close();
        return;
    }
    // Open PCAP file for logging detections
    int err = pcap_file_open("pineap_detection", PCAP_CAPTURE_WIFI);
    if (err != ESP_OK) {
        printf("Warning: Failed to open PCAP file for logging\n");
        TERMINAL_VIEW_ADD_TEXT("Warning: Failed to open PCAP file for logging\n");
    }

    // Start PineAP detection with channel hopping
    start_pineap_detection();
    wifi_manager_start_monitor_mode(wifi_pineap_detector_callback);

    printf("Monitoring for Pineapples\n");
    TERMINAL_VIEW_ADD_TEXT("Monitoring for Pineapples\n");
}


void handle_apcred(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: apcred <ssid> <password>\n");
        printf("       apcred -r (reset to defaults)\n");
        TERMINAL_VIEW_ADD_TEXT("Usage:\napcred <ssid> <password>\n");
        TERMINAL_VIEW_ADD_TEXT("apcred -r\n");
        return;
    }
                
    // Check for reset flag
    if (argc == 2 && strcmp(argv[1], "-r") == 0) {
        // Set empty strings to trigger default values
        settings_set_ap_ssid(&G_Settings, "");
        settings_set_ap_password(&G_Settings, "");
        settings_save(&G_Settings);
        ap_manager_stop_services();
        esp_err_t err = ap_manager_start_services();
        if (err != ESP_OK) {
            printf("Error resetting AP: %s\n", esp_err_to_name(err));
            TERMINAL_VIEW_ADD_TEXT("Error resetting AP:\n%s\n", esp_err_to_name(err));
            return;
        }

        printf("AP credentials reset to defaults (SSID: GhostNet, Password: GhostNet)\n");
        TERMINAL_VIEW_ADD_TEXT("AP reset to defaults:\nSSID: GhostNet\nPSK: GhostNet\n");
        return;
    }

    if (argc != 3) {
        printf("Error: Incorrect number of arguments.\n");
        TERMINAL_VIEW_ADD_TEXT("Error: Bad args\n");
        return;
    }

    const char *new_ssid = argv[1];
    const char *new_password = argv[2];

    if (strlen(new_password) < 8) {
        printf("Error: Password must be at least 8 characters\n");
        TERMINAL_VIEW_ADD_TEXT("Error: Password must\nbe 8+ chars\n");
        return;
    }

    // immediate AP reconfiguration
    wifi_config_t ap_config = {
        .ap = {
            .ssid_len = strlen(new_ssid),
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK
        },
    };
    strcpy((char *)ap_config.ap.ssid, new_ssid);
    strcpy((char *)ap_config.ap.password, new_password);
    
    // Force the new config immediately
    esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    settings_set_ap_ssid(&G_Settings, new_ssid);
    settings_set_ap_password(&G_Settings, new_password);
    settings_save(&G_Settings);

    const char *saved_ssid = settings_get_ap_ssid(&G_Settings);
    const char *saved_password = settings_get_ap_password(&G_Settings);
    if (strcmp(saved_ssid, new_ssid) != 0 || strcmp(saved_password, new_password) != 0) {
        printf("Error: Failed to save AP credentials\n");
        TERMINAL_VIEW_ADD_TEXT("Error: Failed to\nsave credentials\n");
        return;
    }

    ap_manager_stop_services();
    esp_err_t err = ap_manager_start_services();
    if (err != ESP_OK) {
        printf("Error restarting AP: %s\n", esp_err_to_name(err));
        TERMINAL_VIEW_ADD_TEXT("Error restart AP:\n%s\n", esp_err_to_name(err));
        return;
    }

    printf("AP credentials updated - SSID: %s, Password: %s\n", saved_ssid, saved_password);
    TERMINAL_VIEW_ADD_TEXT("AP updated:\nSSID: %s\n", saved_ssid);
}

void handle_rgb_mode(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: rgbmode <rainbow|police|strobe|off|color>\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: rgbmode <rainbow|police|strobe|off|color>\n");
        return;
    }

    // Cancel any currently running LED effect task.
    if (rgb_effect_task_handle != NULL) {
        vTaskDelete(rgb_effect_task_handle);
        rgb_effect_task_handle = NULL;
    }

    // Check for built-in modes first.
    if (strcasecmp(argv[1], "rainbow") == 0) {
        xTaskCreate(rainbow_task, "rainbow_effect", 4096, &rgb_manager, 5, &rgb_effect_task_handle);
        printf("Rainbow mode activated\n");
        TERMINAL_VIEW_ADD_TEXT("Rainbow mode activated\n");
    } else if (strcasecmp(argv[1], "police") == 0) {
        xTaskCreate(police_task, "police_effect", 4096, &rgb_manager, 5, &rgb_effect_task_handle);
        printf("Police mode activated\n");
        TERMINAL_VIEW_ADD_TEXT("Police mode activated\n");
    } else if (strcasecmp(argv[1], "strobe") == 0) {
        printf("SEIZURE WARNING\nPLEASE EXIT NOW IF\nYOU ARE SENSITIVE\n");
        vTaskDelay(pdMS_TO_TICKS(2000));
        xTaskCreate(strobe_task, "strobe_effect", 4096, &rgb_manager, 5, &rgb_effect_task_handle);
        printf("Strobe mode activated\n");
        TERMINAL_VIEW_ADD_TEXT("Strobe mode activated\n");
    } else if (strcasecmp(argv[1], "off") == 0) {
        rgb_manager_set_color(&rgb_manager, -1, 0, 0, 0, false);
        if (!rgb_manager.is_separate_pins) {
            led_strip_clear(rgb_manager.strip);
            led_strip_refresh(rgb_manager.strip);
        }
        printf("RGB disabled\n");
        TERMINAL_VIEW_ADD_TEXT("RGB disabled\n");
    } else {
        // Otherwise, treat the argument as a color name.
        typedef struct {
            const char *name;
            uint8_t r;
            uint8_t g;
            uint8_t b;
        } color_t;
        static const color_t supported_colors[] = {
            { "red",    255, 0,   0 },
            { "green",  0,   255, 0 },
            { "blue",   0,   0,   255 },
            { "yellow", 255, 255, 0 },
            { "purple", 128, 0,   128 },
            { "cyan",   0,   255, 255 },
            { "orange", 255, 165, 0 },
            { "white",  255, 255, 255 },
            { "pink",   255, 192, 203 }
        };
        const int num_colors = sizeof(supported_colors) / sizeof(supported_colors[0]);
        int found = 0;
        uint8_t r, g, b;
        for (int i = 0; i < num_colors; i++) {
            // Use case-insensitive compare.
            if (strcasecmp(argv[1], supported_colors[i].name) == 0) {
                r = supported_colors[i].r;
                g = supported_colors[i].g;
                b = supported_colors[i].b;
                found = 1;
                break;
            }
        }
        if (!found) {
            printf("Unknown color '%s'. Supported colors: red, green, blue, yellow, purple, cyan, orange, white, pink.\n", argv[1]);
            TERMINAL_VIEW_ADD_TEXT("Unknown color '%s'. Supported colors: red, green, blue, yellow, purple, cyan, orange, white, pink.\n", argv[1]);
            return;
        }
        // Set each LED to the selected static color.
        for (int i = 0; i < rgb_manager.num_leds; i++) {
            rgb_manager_set_color(&rgb_manager, i, r, g, b, false);
        }
        led_strip_refresh(rgb_manager.strip);
        printf("Static color mode activated: %s\n", argv[1]);
        TERMINAL_VIEW_ADD_TEXT("Static color mode activated: %s\n", argv[1]);
    }
}

void handle_setrgb(int argc, char **argv) {
    if (argc != 4) {
        printf("Usage: setrgbpins <red> <green> <blue>\n");
        printf("           (use same value for all pins for single-pin LED strips)\n\n");
        return;
    }
    gpio_num_t red_pin = (gpio_num_t)atoi(argv[1]);
    gpio_num_t green_pin = (gpio_num_t)atoi(argv[2]);
    gpio_num_t blue_pin = (gpio_num_t)atoi(argv[3]);

    esp_err_t ret;
    if (red_pin == green_pin && green_pin == blue_pin) {
        rgb_manager_deinit(&rgb_manager);
        ret = rgb_manager_init(&rgb_manager, red_pin, 1, LED_PIXEL_FORMAT_GRB, LED_MODEL_WS2812,
                                GPIO_NUM_NC, GPIO_NUM_NC, GPIO_NUM_NC);
        if (ret == ESP_OK) {
            settings_set_rgb_data_pin(&G_Settings, red_pin);
            settings_set_rgb_separate_pins(&G_Settings, -1, -1, -1);
            settings_save(&G_Settings);
            printf("Single-pin RGB configured on GPIO %d and saved.\n", red_pin);
        }
    } else {
        rgb_manager_deinit(&rgb_manager);
        ret = rgb_manager_init(&rgb_manager, GPIO_NUM_NC, 1, LED_PIXEL_FORMAT_GRB, LED_MODEL_WS2812,
                               red_pin, green_pin, blue_pin);
        if (ret == ESP_OK) {
            settings_set_rgb_data_pin(&G_Settings, -1);
            settings_set_rgb_separate_pins(&G_Settings, red_pin, green_pin, blue_pin);
            settings_save(&G_Settings);
            printf("RGB pins updated to R:%d G:%d B:%d and saved.\n", red_pin, green_pin, blue_pin);
        }
    }
}

void handle_sd_config(int argc, char **argv) {
  sd_card_print_config();
}

void handle_sd_pins_mmc(int argc, char **argv) {
  if (argc != 7) {
    printf("Usage: sd_pins_mmc <clk> <cmd> <d0> <d1> <d2> <d3>\n");
    printf("Sets pins for SDMMC mode (only effective if compiled for MMC).\n");
    printf("Example: sd_pins_mmc 19 18 20 21 22 23\n");
    return;
  }
  
  int clk = atoi(argv[1]);
  int cmd = atoi(argv[2]);
  int d0 = atoi(argv[3]);
  int d1 = atoi(argv[4]);
  int d2 = atoi(argv[5]);
  int d3 = atoi(argv[6]);
  
  if (clk < 0 || cmd < 0 || d0 < 0 || d1 < 0 || d2 < 0 || d3 < 0 ||
      clk > 40 || cmd > 40 || d0 > 40 || d1 > 40 || d2 > 40 || d3 > 40) {
    printf("Invalid GPIO pins. Pins must be between 0 and 40.\n");
    return;
  }
  
  sd_card_set_mmc_pins(clk, cmd, d0, d1, d2, d3);
}

void handle_sd_pins_spi(int argc, char **argv) {
  if (argc != 5) {
    printf("Usage: sd_pins_spi <cs> <clk> <miso> <mosi>\n");
    printf("Sets pins for SPI mode (only effective if compiled for SPI).\n");
    printf("Example: sd_pins_spi 5 18 19 23\n");
    return;
  }
  
  int cs = atoi(argv[1]);
  int clk = atoi(argv[2]);
  int miso = atoi(argv[3]);
  int mosi = atoi(argv[4]);
  
  if (cs < 0 || clk < 0 || miso < 0 || mosi < 0 ||
      cs > 40 || clk > 40 || miso > 40 || mosi > 40) {
    printf("Invalid GPIO pins. Pins must be between 0 and 40.\n");
    return;
  }
  
  sd_card_set_spi_pins(cs, clk, miso, mosi);
}

void handle_sd_save_config(int argc, char **argv) {
  sd_card_save_config();
}

void handle_congestion_cmd(int argc, char **argv) {
    wifi_manager_start_scan();

    uint16_t ap_count = 0;
    wifi_ap_record_t *ap_records = NULL;

    wifi_manager_get_scan_results_data(&ap_count, &ap_records);

    if (ap_count == 0 || ap_records == NULL) {
        printf("No APs found during scan.\n");
        TERMINAL_VIEW_ADD_TEXT("No APs found during scan.\n");
        return;
    }

    int unique_count = 0;
    int *channels = malloc(ap_count * sizeof(int));
    int *counts = malloc(ap_count * sizeof(int));
    int max_count = 0;
    for (int i = 0; i < ap_count; i++) {
        int ch = ap_records[i].primary;
        if (ch <= 0) continue;
        int idx = -1;
        for (int j = 0; j < unique_count; j++) {
            if (channels[j] == ch) { idx = j; break; }
        }
        if (idx >= 0) {
            counts[idx]++;
        } else {
            channels[unique_count] = ch;
            counts[unique_count] = 1;
            idx = unique_count++;
        }
        if (counts[idx] > max_count) {
            max_count = counts[idx];
        }
    }
    for (int i = 0; i < unique_count - 1; i++) {
        for (int j = i + 1; j < unique_count; j++) {
            if (channels[i] > channels[j]) {
                int tmp_ch = channels[i]; channels[i] = channels[j]; channels[j] = tmp_ch;
                int tmp_cnt = counts[i]; counts[i] = counts[j]; counts[j] = tmp_cnt;
            }
        }
    }

    printf("\nChannel Congestion:\n\n");
    TERMINAL_VIEW_ADD_TEXT("\nChannel Congestion:\n\n");
    const char* header = "+----+-------+------------+\n";
    const char* separator = "+----+-------+------------+\n";
    const char* row_format = "| %2d | %5d | %s |\n";
    const char* footer = "+----+-------+------------+\n";

    printf("%s", header);
    TERMINAL_VIEW_ADD_TEXT("%s", header);
    printf("| CH | Count | Bar        |\n");
    TERMINAL_VIEW_ADD_TEXT("| CH | Count | Bar        |\n");
    printf("%s", separator);
    TERMINAL_VIEW_ADD_TEXT("%s", separator);

    const int max_bar_length = 10;
    char display_bar[max_bar_length * 4]; // Generous buffer: 3 bytes/block + 1 space/pad + null

    for (int i = 0; i < unique_count; i++) {
        int ch = channels[i];
        int cnt = counts[i];
        int bar_length = 0;
        if (max_count > 0) {
            bar_length = (int)(((float)cnt / max_count) * max_bar_length);
            if (bar_length == 0 && cnt > 0) bar_length = 1;
        }
        char *ptr = display_bar;
        for (int j = 0; j < bar_length; ++j) {
            *ptr++ = '#';
        }
        int spaces_needed = max_bar_length - bar_length;
        for (int j = 0; j < spaces_needed; ++j) {
            *ptr++ = ' ';
        }
        *ptr = '\0';
        printf(row_format, ch, cnt, display_bar);
        TERMINAL_VIEW_ADD_TEXT(row_format, ch, cnt, display_bar);
    }
    free(channels);
    free(counts);
    printf("%s", footer);
    TERMINAL_VIEW_ADD_TEXT("%s", footer);
}

// Forward declaration for the new print function
void wifi_manager_scanall_chart();

void handle_scanall(int argc, char **argv) {
    int total_seconds = 10; // Default total duration: 10 seconds
    if (argc > 1) {
        char *endptr;
        long sec = strtol(argv[1], &endptr, 10);
        if (*endptr == '\0' && sec > 0) {
            total_seconds = (int)sec;
        } else {
            printf("Invalid duration: '%s'. Using default %d seconds.\n", argv[1], total_seconds);
            TERMINAL_VIEW_ADD_TEXT("Invalid duration: '%s'. Using default %d seconds.\n", argv[1], total_seconds);
        }
    }

    int ap_scan_seconds = total_seconds / 2;
    int sta_scan_seconds = total_seconds - ap_scan_seconds; // Use remaining time

    printf("Starting combined scan (%d sec AP, %d sec STA)...\n", ap_scan_seconds, sta_scan_seconds);
    TERMINAL_VIEW_ADD_TEXT("Starting combined scan (%ds AP, %ds STA)...\n", ap_scan_seconds, sta_scan_seconds);

    // 1. Perform AP Scan
    printf("--- Starting AP Scan (%d seconds) ---\n", ap_scan_seconds);
    TERMINAL_VIEW_ADD_TEXT("--- Starting AP Scan (%ds) ---\n", ap_scan_seconds);
    wifi_manager_start_scan_with_time(ap_scan_seconds);
    // Results are now in scanned_aps and ap_count

    // 2. Perform Station Scan
    printf("--- Starting Station Scan (%d seconds) ---\n", sta_scan_seconds);
    TERMINAL_VIEW_ADD_TEXT("--- Starting STA Scan (%ds) ---\n", sta_scan_seconds);
    station_count = 0; // Reset station list before new scan
    wifi_manager_start_station_scan(); // Starts monitor mode + channel hopping
    printf("Station scan running for %d seconds...\n", sta_scan_seconds);
    TERMINAL_VIEW_ADD_TEXT("Station scan running for %ds...\n", sta_scan_seconds);
    vTaskDelay(pdMS_TO_TICKS(sta_scan_seconds * 1000));
    wifi_manager_stop_monitor_mode(); // Stops monitor mode + channel hopping
    // Results are now in station_ap_list and station_count

    printf("--- Scan Complete ---\n");
    TERMINAL_VIEW_ADD_TEXT("--- Scan Complete ---\n");

    // 3. Print Combined Results
    wifi_manager_scanall_chart();

    // Ensure AP mode is restored if it was stopped
    ap_manager_start_services(); // Restore AP for WebUI
}

// Helper function to simplify calling list airtags
#ifndef CONFIG_IDF_TARGET_ESP32S2
void handle_list_airtags_cmd(int argc, char **argv) {
    ble_list_airtags();
}
#endif

// Select AirTag handler
#ifndef CONFIG_IDF_TARGET_ESP32S2
void handle_select_airtag(int argc, char **argv) {
    if (argc != 2) {
        printf("Usage: selectairtag <number>\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: selectairtag <number>\n");
        return;
    }

    char *endptr;
    int num = (int)strtol(argv[1], &endptr, 10);
    if (*endptr == '\0') {
        ble_select_airtag(num);
    } else {
        printf("Error: '%s' is not a valid number.\n", argv[1]);
        TERMINAL_VIEW_ADD_TEXT("Error: '%s' is not a valid number.\n", argv[1]);
    }
}
#endif

// Spoof AirTag handler
#ifndef CONFIG_IDF_TARGET_ESP32S2
void handle_spoof_airtag(int argc, char **argv) {
    ble_start_spoofing_selected_airtag();
}
#endif

// Stop Spoof handler
#ifndef CONFIG_IDF_TARGET_ESP32S2
void handle_stop_spoof(int argc, char **argv) {
    ble_stop_spoofing();
}
#endif

// Handlers for Flipper commands
#ifndef CONFIG_IDF_TARGET_ESP32S2
void handle_list_flippers_cmd(int argc, char **argv) {
    ble_list_flippers();
}

void handle_select_flipper_cmd(int argc, char **argv) {
    if (argc != 2) {
        printf("Usage: selectflipper <index>\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: selectflipper <index>\n");
        return;
    }
    char *endptr;
    int num = (int)strtol(argv[1], &endptr, 10);
    if (*endptr == '\0') {
        ble_select_flipper(num);
    } else {
        printf("Error: '%s' is not a valid number.\n", argv[1]);
        TERMINAL_VIEW_ADD_TEXT("Error: '%s' is not a valid number.\n", argv[1]);
    }
}
#endif

// New beacon list command handlers
void handle_beaconadd(int argc, char **argv) {
    if (argc != 2) {
        printf("Usage: beaconadd <SSID>\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: beaconadd <SSID>\n");
        return;
    }
    wifi_manager_add_beacon_ssid(argv[1]);
}

void handle_beaconremove(int argc, char **argv) {
    if (argc != 2) {
        printf("Usage: beaconremove <SSID>\n");
        TERMINAL_VIEW_ADD_TEXT("Usage: beaconremove <SSID>\n");
        return;
    }
    wifi_manager_remove_beacon_ssid(argv[1]);
}

void handle_beaconclear(int argc, char **argv) {
    wifi_manager_clear_beacon_list();
}

void handle_beaconshow(int argc, char **argv) {
    wifi_manager_show_beacon_list();
}

void handle_beaconspamlist(int argc, char **argv) {
    wifi_manager_start_beacon_list();
}

void handle_dhcpstarve_cmd(int argc, char **argv) {
    if (argc < 2) {
        wifi_manager_dhcpstarve_help();
    } else if (strcmp(argv[1], "start") == 0) {
        int thr = (argc >= 3) ? atoi(argv[2]) : 1;
        wifi_manager_start_dhcpstarve(thr);
    } else if (strcmp(argv[1], "stop") == 0) {
        wifi_manager_stop_dhcpstarve();
    } else if (strcmp(argv[1], "display") == 0) {
        wifi_manager_dhcpstarve_display();
    } else {
        wifi_manager_dhcpstarve_help();
    }
}

void register_commands() {
    register_command("help", handle_help);
    register_command("scanap", cmd_wifi_scan_start);
    register_command("scansta", handle_sta_scan);
    register_command("scanlocal", handle_ip_lookup);
    register_command("stopscan", cmd_wifi_scan_stop);
    register_command("attack", handle_attack_cmd);
    register_command("list", handle_list);
    register_command("beaconspam", handle_beaconspam);
    // Register new beacon list commands
    register_command("beaconadd", handle_beaconadd);
    register_command("beaconremove", handle_beaconremove);
    register_command("beaconclear", handle_beaconclear);
    register_command("beaconshow", handle_beaconshow);
    register_command("beaconspamlist", handle_beaconspamlist);
    register_command("stopspam", handle_stop_spam);
    register_command("stopdeauth", handle_stop_deauth);
    register_command("select", handle_select_cmd);
    register_command("capture", handle_capture_scan);
    register_command("startportal", handle_start_portal);
    register_command("stopportal", stop_portal);
    register_command("connect", handle_wifi_connection);
    register_command("dialconnect", handle_dial_command);
    register_command("powerprinter", handle_printer_command);
    register_command("tplinktest", handle_tp_link_test);
    register_command("stop", handle_stop_flipper);
    register_command("reboot", handle_reboot);
    register_command("startwd", handle_startwd);
    register_command("gpsinfo", handle_gps_info);
    register_command("scanports", handle_scan_ports);
    register_command("congestion", handle_congestion_cmd);
#ifndef CONFIG_IDF_TARGET_ESP32S2
    register_command("blescan", handle_ble_scan_cmd);
    register_command("blewardriving", handle_ble_wardriving);
    // AirTag Commands
    register_command("listairtags", handle_list_airtags_cmd);
    register_command("selectairtag", handle_select_airtag);
    register_command("spoofairtag", handle_spoof_airtag);
    register_command("stopspoof", handle_stop_spoof);
#endif
#ifdef DEBUG
    register_command("crash", handle_crash); // For Debugging
#endif
    register_command("pineap", handle_pineap_detection);
    register_command("apcred", handle_apcred);
    register_command("rgbmode", handle_rgb_mode);
    register_command("setrgbpins", handle_setrgb);
    // SD Card Pin configuration commands
    register_command("sd_config", handle_sd_config);
    register_command("sd_pins_mmc", handle_sd_pins_mmc);
    register_command("sd_pins_spi", handle_sd_pins_spi);
    register_command("sd_save_config", handle_sd_save_config);
    register_command("scanall", handle_scanall);
    register_command("timezone", handle_timezone_cmd);
#ifndef CONFIG_IDF_TARGET_ESP32S2
    register_command("listflippers", handle_list_flippers_cmd);
    register_command("selectflipper", handle_select_flipper_cmd);
#endif
    register_command("dhcpstarve", handle_dhcpstarve_cmd);
    printf("Registered Commands\n");
    TERMINAL_VIEW_ADD_TEXT("Registered Commands\n");
}
