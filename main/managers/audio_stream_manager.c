#include "managers/audio_stream_manager.h"

#ifdef CONFIG_HAS_AUDIO_PLAYER
#include "core/esp_comm_manager.h"
#include "managers/sd_card_manager.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

static const char *TAG = "AudioStream";

#define MAX_MP3_FILES     64
#define MAX_FILENAME_LEN  64
#define STREAM_CHUNK_SIZE 512
#define STREAM_READ_BURST_SIZE (8 * 1024)
#define STREAM_SEND_WAIT_MS 100
#define STREAM_TASK_STACK 4096
#define STREAM_TASK_PRIO  18
#define STREAM_INTER_PACKET_DELAY_MS 3
#define STREAM_EXTRA_DELAY_EVERY_PACKETS 32

typedef struct {
    char filenames[MAX_MP3_FILES][MAX_FILENAME_LEN];
    int file_count;
    int current_index;
    audio_stream_state_t state;
    size_t stream_offset;
    uint32_t stream_id;
    TaskHandle_t stream_task;
    SemaphoreHandle_t mutex;
    bool sd_available;
    bool initialized;
} audio_stream_ctx_t;

static audio_stream_ctx_t s_ctx = {0};
static StackType_t *s_stream_task_stack = NULL;
static StaticTask_t *s_stream_task_tcb = NULL;

static void audio_stream_task(void *arg);
static TaskHandle_t create_audio_stream_task(uint32_t stream_id);
static int scan_mp3_files(void);
static bool is_mp3_file(const char *filename);
static bool audio_sd_begin(bool *display_was_suspended, bool *did_mount, bool ensure_dirs);
static void audio_sd_end(bool display_was_suspended, bool did_mount);

static bool audio_sd_should_jit(void)
{
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
    return strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething") == 0 ||
           strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething2") == 0;
#else
    return false;
#endif
}

static bool audio_sd_begin(bool *display_was_suspended, bool *did_mount, bool ensure_dirs)
{
    if (display_was_suspended) *display_was_suspended = false;
    if (did_mount) *did_mount = false;

    if (audio_sd_should_jit()) {
        esp_err_t mount_err = sd_card_mount_for_flush(display_was_suspended);
        if (mount_err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to mount SD card for audio: %s", esp_err_to_name(mount_err));
            return false;
        }
        if (did_mount) *did_mount = true;
    }

    if (ensure_dirs) {
        esp_err_t dir_err = sd_card_setup_directory_structure();
        if (dir_err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to ensure audio directory structure: %s", esp_err_to_name(dir_err));
        }
    }

    return true;
}

static void audio_sd_end(bool display_was_suspended, bool did_mount)
{
    if (did_mount) {
        sd_card_unmount_after_flush(display_was_suspended);
    }
}

static bool is_mp3_file(const char *filename)
{
    size_t len = strlen(filename);
    if (len < 4) return false;
    return (strcasecmp(filename + len - 4, ".mp3") == 0);
}

static TaskHandle_t create_audio_stream_task(uint32_t stream_id)
{
    if (!s_stream_task_stack) {
        s_stream_task_stack = (StackType_t *)heap_caps_malloc(STREAM_TASK_STACK * sizeof(StackType_t),
                                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!s_stream_task_tcb) {
        s_stream_task_tcb = (StaticTask_t *)heap_caps_malloc(sizeof(StaticTask_t),
                                                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }

    if (s_stream_task_stack && s_stream_task_tcb) {
        TaskHandle_t handle = xTaskCreateStatic(audio_stream_task, "audio_stream",
                                                STREAM_TASK_STACK, (void *)(uintptr_t)stream_id,
                                                STREAM_TASK_PRIO, s_stream_task_stack, s_stream_task_tcb);
        if (handle) {
            ESP_LOGI(TAG, "Audio stream task stack allocated from PSRAM: %d bytes",
                     (int)(STREAM_TASK_STACK * sizeof(StackType_t)));
        }
        return handle;
    }

    TaskHandle_t handle = NULL;
    if (xTaskCreate(audio_stream_task, "audio_stream", STREAM_TASK_STACK,
                    (void *)(uintptr_t)stream_id, STREAM_TASK_PRIO, &handle) == pdPASS) {
        ESP_LOGW(TAG, "Audio stream task using internal stack fallback");
        return handle;
    }

    return NULL;
}

static int scan_mp3_files(void)
{
    s_ctx.file_count = 0;

    bool display_was_suspended = false;
    bool did_mount = false;
    s_ctx.sd_available = false;
    if (!audio_sd_begin(&display_was_suspended, &did_mount, true)) {
        return 0;
    }
    s_ctx.sd_available = true;

    const char *path = CONFIG_AUDIO_FILES_PATH;
    DIR *dir = opendir(path);
    if (!dir) {
        ESP_LOGW(TAG, "Could not open audio dir '%s', trying to create", path);
        /* Try to create the directory */
        if (mkdir(path, 0755) != 0) {
            ESP_LOGW(TAG, "Could not create audio dir '%s'", path);
        }
        dir = opendir(path);
        if (!dir) {
            audio_sd_end(display_was_suspended, did_mount);
            return 0;
        }
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && s_ctx.file_count < MAX_MP3_FILES) {
        bool is_file = false;
        if (entry->d_type == DT_REG) {
            is_file = true;
        } else if (entry->d_type == DT_UNKNOWN) {
            /* FAT32 doesn't populate d_type; fall back to stat */
            char fullpath[512];
            snprintf(fullpath, sizeof(fullpath), "%s/%s", path, entry->d_name);
            struct stat st;
            if (stat(fullpath, &st) == 0 && S_ISREG(st.st_mode)) {
                is_file = true;
            }
        }
        if (is_file && is_mp3_file(entry->d_name)) {
            strncpy(s_ctx.filenames[s_ctx.file_count], entry->d_name, MAX_FILENAME_LEN - 1);
            s_ctx.filenames[s_ctx.file_count][MAX_FILENAME_LEN - 1] = '\0';
            s_ctx.file_count++;
        }
    }
    closedir(dir);
    audio_sd_end(display_was_suspended, did_mount);

    /* Sort alphabetically by filename */
    for (int i = 0; i < s_ctx.file_count - 1; i++) {
        for (int j = i + 1; j < s_ctx.file_count; j++) {
            if (strcasecmp(s_ctx.filenames[i], s_ctx.filenames[j]) > 0) {
                char temp[MAX_FILENAME_LEN];
                memcpy(temp, s_ctx.filenames[i], MAX_FILENAME_LEN);
                memcpy(s_ctx.filenames[i], s_ctx.filenames[j], MAX_FILENAME_LEN);
                memcpy(s_ctx.filenames[j], temp, MAX_FILENAME_LEN);
            }
        }
    }

    ESP_LOGI(TAG, "Found %d MP3 files in '%s'", s_ctx.file_count, path);
    return s_ctx.file_count;
}

esp_err_t audio_stream_manager_init(void)
{
    if (s_ctx.initialized) {
        return ESP_OK;
    }

    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.mutex = xSemaphoreCreateMutex();
    if (!s_ctx.mutex) {
        return ESP_ERR_NO_MEM;
    }

    scan_mp3_files();

    s_ctx.initialized = true;
    ESP_LOGI(TAG, "Audio stream manager initialized (%d files)", s_ctx.file_count);
    return ESP_OK;
}

void audio_stream_manager_deinit(void)
{
    if (!s_ctx.initialized) return;

    audio_stream_manager_stop();

    for (int i = 0; i < 10; i++) {
        xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
        bool task_gone = (s_ctx.stream_task == NULL);
        xSemaphoreGive(s_ctx.mutex);
        if (task_gone) break;
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
    if (s_ctx.mutex) {
        vSemaphoreDelete(s_ctx.mutex);
        s_ctx.mutex = NULL;
    }
    memset(&s_ctx, 0, sizeof(s_ctx));
}

audio_stream_state_t audio_stream_manager_get_state(void)
{
    return s_ctx.state;
}

int audio_stream_manager_get_file_count(void)
{
    return s_ctx.file_count;
}

const char *audio_stream_manager_get_filename(int index)
{
    if (index < 0 || index >= s_ctx.file_count) return NULL;
    return s_ctx.filenames[index];
}

int audio_stream_manager_get_current_index(void)
{
    return s_ctx.current_index;
}

esp_err_t audio_stream_manager_play(int index)
{
    if (!s_ctx.initialized) return ESP_ERR_INVALID_STATE;
    if (index < 0 || index >= s_ctx.file_count) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);

    /* Stop existing stream task */
    s_ctx.state = AUDIO_STREAM_STATE_STOPPED;
    s_ctx.stream_id++;
    if (s_ctx.stream_task) {
        xSemaphoreGive(s_ctx.mutex);
        for (int i = 0; i < 10 && s_ctx.stream_task; ++i) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
        if (s_ctx.stream_task) {
            ESP_LOGW(TAG, "Previous audio stream task still running, clearing reference");
            s_ctx.stream_task = NULL;
        }
    }

    s_ctx.current_index = index;
    s_ctx.stream_offset = 0;
    uint32_t stream_id = s_ctx.stream_id;

    /* Build full path */
    char path[128];
    snprintf(path, sizeof(path), "%s/%s", CONFIG_AUDIO_FILES_PATH, s_ctx.filenames[index]);

    bool display_was_suspended = false;
    bool did_mount = false;
    if (!audio_sd_begin(&display_was_suspended, &did_mount, false)) {
        xSemaphoreGive(s_ctx.mutex);
        return ESP_FAIL;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open '%s'", path);
        audio_sd_end(display_was_suspended, did_mount);
        xSemaphoreGive(s_ctx.mutex);
        return ESP_FAIL;
    }
    fclose(f);
    audio_sd_end(display_was_suspended, did_mount);

    ESP_LOGI(TAG, "Playing: %s", s_ctx.filenames[index]);

    (void)esp_comm_manager_send_command("audio", "start");

    s_ctx.state = AUDIO_STREAM_STATE_PLAYING;

    s_ctx.stream_task = create_audio_stream_task(stream_id);
    if (!s_ctx.stream_task) {
        s_ctx.state = AUDIO_STREAM_STATE_IDLE;
        xSemaphoreGive(s_ctx.mutex);
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreGive(s_ctx.mutex);
    return ESP_OK;
}

esp_err_t audio_stream_manager_pause(void)
{
    if (!s_ctx.initialized) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
    if (s_ctx.state == AUDIO_STREAM_STATE_PLAYING) {
        s_ctx.state = AUDIO_STREAM_STATE_PAUSED;
        ESP_LOGI(TAG, "Paused");
    }
    xSemaphoreGive(s_ctx.mutex);
    return ESP_OK;
}

esp_err_t audio_stream_manager_resume(void)
{
    if (!s_ctx.initialized) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
    if (s_ctx.state == AUDIO_STREAM_STATE_PAUSED) {
        s_ctx.state = AUDIO_STREAM_STATE_PLAYING;
        ESP_LOGI(TAG, "Resumed");
    }
    xSemaphoreGive(s_ctx.mutex);
    return ESP_OK;
}

esp_err_t audio_stream_manager_stop(void)
{
    if (!s_ctx.initialized) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);

    s_ctx.state = AUDIO_STREAM_STATE_STOPPED;
    s_ctx.stream_id++;

    (void)esp_comm_manager_send_command("audio", "stop");

    xSemaphoreGive(s_ctx.mutex);

    /* Wait for stream task to exit */
    if (s_ctx.stream_task) {
        /* Give task time to notice stop and exit */
        for (int i = 0; i < 10 && s_ctx.stream_task; ++i) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

    ESP_LOGI(TAG, "Stopped");
    return ESP_OK;
}

esp_err_t audio_stream_manager_next(void)
{
    if (!s_ctx.initialized || s_ctx.file_count == 0) return ESP_ERR_INVALID_STATE;
    int next = s_ctx.current_index + 1;
    if (next >= s_ctx.file_count) next = 0;
    return audio_stream_manager_play(next);
}

esp_err_t audio_stream_manager_prev(void)
{
    if (!s_ctx.initialized || s_ctx.file_count == 0) return ESP_ERR_INVALID_STATE;
    int prev = s_ctx.current_index - 1;
    if (prev < 0) prev = s_ctx.file_count - 1;
    return audio_stream_manager_play(prev);
}

bool audio_stream_manager_is_initialized(void)
{
    return s_ctx.initialized;
}

bool audio_stream_manager_sd_available(void)
{
    return s_ctx.sd_available;
}

static void audio_stream_task(void *arg)
{
    uint32_t stream_id = (uint32_t)(uintptr_t)arg;
    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    uint8_t *stream_buf = heap_caps_malloc(STREAM_READ_BURST_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!stream_buf) {
        stream_buf = malloc(STREAM_READ_BURST_SIZE);
    }
    if (!stream_buf) {
        ESP_LOGE(TAG, "Failed to allocate stream buffer");
        xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
        s_ctx.state = AUDIO_STREAM_STATE_STOPPED;
        if (s_ctx.stream_id == stream_id && s_ctx.stream_task == self) {
            s_ctx.stream_task = NULL;
        }
        xSemaphoreGive(s_ctx.mutex);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Stream task started");
    bool waiting_logged = false;
    bool first_chunk_logged = false;
    uint32_t packet_count = 0;

    while (1) {
        xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);

        if (s_ctx.state == AUDIO_STREAM_STATE_STOPPED || s_ctx.stream_id != stream_id) {
            xSemaphoreGive(s_ctx.mutex);
            break;
        }

        if (s_ctx.state == AUDIO_STREAM_STATE_PAUSED) {
            xSemaphoreGive(s_ctx.mutex);
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (!esp_comm_manager_is_connected()) {
            if (!waiting_logged) {
                ESP_LOGW(TAG, "Waiting for GhostLink connection before streaming audio");
                waiting_logged = true;
            }
            xSemaphoreGive(s_ctx.mutex);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        waiting_logged = false;

        int index = s_ctx.current_index;
        size_t offset = s_ctx.stream_offset;
        char filename[MAX_FILENAME_LEN];
        if (index < 0 || index >= s_ctx.file_count) {
            s_ctx.state = AUDIO_STREAM_STATE_STOPPED;
            xSemaphoreGive(s_ctx.mutex);
            break;
        }
        strncpy(filename, s_ctx.filenames[index], sizeof(filename) - 1);
        filename[sizeof(filename) - 1] = '\0';
        xSemaphoreGive(s_ctx.mutex);

        char path[128];
        snprintf(path, sizeof(path), "%s/%s", CONFIG_AUDIO_FILES_PATH, filename);

        bool display_was_suspended = false;
        bool did_mount = false;
        size_t bytes_read = 0;
        bool read_ok = false;

        if (audio_sd_begin(&display_was_suspended, &did_mount, false)) {
            FILE *f = fopen(path, "rb");
            if (f) {
                if (fseek(f, (long)offset, SEEK_SET) == 0) {
                    bytes_read = fread(stream_buf, 1, STREAM_READ_BURST_SIZE, f);
                    read_ok = true;
                }
                fclose(f);
            } else {
                ESP_LOGE(TAG, "Failed to open '%s'", path);
            }
            audio_sd_end(display_was_suspended, did_mount);
        }

        if (!read_ok) {
            xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
            s_ctx.state = AUDIO_STREAM_STATE_STOPPED;
            if (s_ctx.stream_id == stream_id && s_ctx.stream_task == self) {
                s_ctx.stream_task = NULL;
            }
            xSemaphoreGive(s_ctx.mutex);
            break;
        }

        xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
        if (s_ctx.state == AUDIO_STREAM_STATE_STOPPED || s_ctx.stream_id != stream_id ||
            s_ctx.current_index != index || s_ctx.stream_offset != offset) {
            xSemaphoreGive(s_ctx.mutex);
            continue;
        }

        if (bytes_read == 0) {
            /* End of file - signal stop and exit cleanly */
            ESP_LOGI(TAG, "End of file reached");
            s_ctx.state = AUDIO_STREAM_STATE_STOPPED;
            s_ctx.stream_id++;
            bool is_self = (s_ctx.stream_task == self);
            if (is_self) {
                s_ctx.stream_task = NULL;
            }
            xSemaphoreGive(s_ctx.mutex);

            (void)esp_comm_manager_send_command("audio", "stop");

            free(stream_buf);
            stream_buf = NULL;

            vTaskDelete(NULL);
            return;
        }

        /* Send buffered chunks after unmounting so LVGL/display SPI gets time between SD bursts. */
        if (bytes_read > 0) {
            size_t sent_total = 0;
            xSemaphoreGive(s_ctx.mutex);

            while (sent_total < bytes_read) {
                xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
                bool should_stop = s_ctx.state == AUDIO_STREAM_STATE_STOPPED ||
                                   s_ctx.stream_id != stream_id ||
                                   s_ctx.current_index != index ||
                                   s_ctx.stream_offset != offset;
                xSemaphoreGive(s_ctx.mutex);
                if (should_stop) {
                    break;
                }

                size_t send_len = bytes_read - sent_total;
                if (send_len > STREAM_CHUNK_SIZE) {
                    send_len = STREAM_CHUNK_SIZE;
                }
                bool sent = esp_comm_manager_send_stream_wait(COMM_STREAM_CHANNEL_AUDIO,
                                                              stream_buf + sent_total,
                                                              send_len,
                                                              STREAM_SEND_WAIT_MS);
                if (sent && !first_chunk_logged) {
                    ESP_LOGI(TAG, "Audio stream packets started (%d byte SD read bursts, %d byte packets, %dms inter-packet)",
                             (int)STREAM_READ_BURST_SIZE, (int)STREAM_CHUNK_SIZE, (int)STREAM_INTER_PACKET_DELAY_MS);
                    first_chunk_logged = true;
                }
                if (!sent) {
                    ESP_LOGW(TAG, "GhostLink stream send failed");
                    xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
                    s_ctx.state = AUDIO_STREAM_STATE_STOPPED;
                    xSemaphoreGive(s_ctx.mutex);
                    break;
                }

                sent_total += send_len;
                packet_count++;

                /* Pace near the observed decoder consumption rate. */
                vTaskDelay(pdMS_TO_TICKS(STREAM_INTER_PACKET_DELAY_MS));
                if ((packet_count % STREAM_EXTRA_DELAY_EVERY_PACKETS) == 0) {
                    vTaskDelay(pdMS_TO_TICKS(1));
                }
            }

            xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
            if (sent_total != bytes_read || s_ctx.state == AUDIO_STREAM_STATE_STOPPED ||
                s_ctx.stream_id != stream_id || s_ctx.current_index != index ||
                s_ctx.stream_offset != offset) {
                xSemaphoreGive(s_ctx.mutex);
                break;
            }
            s_ctx.stream_offset = offset + bytes_read;
        }

        xSemaphoreGive(s_ctx.mutex);
    }

    ESP_LOGI(TAG, "Stream task exiting");
    free(stream_buf);
    xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
    if (s_ctx.stream_id == stream_id && s_ctx.stream_task == self) {
        s_ctx.stream_task = NULL;
    }
    xSemaphoreGive(s_ctx.mutex);
    vTaskDelete(NULL);
}

#else /* !CONFIG_HAS_AUDIO_PLAYER */

esp_err_t audio_stream_manager_init(void) { return ESP_ERR_NOT_SUPPORTED; }
void audio_stream_manager_deinit(void) {}
audio_stream_state_t audio_stream_manager_get_state(void) { return AUDIO_STREAM_STATE_IDLE; }
int audio_stream_manager_get_file_count(void) { return 0; }
const char *audio_stream_manager_get_filename(int index) { (void)index; return NULL; }
int audio_stream_manager_get_current_index(void) { return -1; }
esp_err_t audio_stream_manager_play(int index) { (void)index; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t audio_stream_manager_pause(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t audio_stream_manager_resume(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t audio_stream_manager_stop(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t audio_stream_manager_next(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t audio_stream_manager_prev(void) { return ESP_ERR_NOT_SUPPORTED; }
bool audio_stream_manager_is_initialized(void) { return false; }
bool audio_stream_manager_sd_available(void) { return false; }

#endif /* CONFIG_HAS_AUDIO_PLAYER */
