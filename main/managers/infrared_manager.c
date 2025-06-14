#include "managers/infrared_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "managers/sd_card_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "sdkconfig.h"
#include "managers/rgb_manager.h"
#include "esp_log.h"
#include <ctype.h>
#include <stdint.h>
#include <stdbool.h>
#include "driver/rmt_tx.h"
#include "driver/rmt_types.h"
#include "driver/rmt_common.h"
#include <inttypes.h>
#include "esp_heap_caps.h"
#include <strings.h>
#include "managers/infrared_timings.h"
#include "managers/infrared_protocols.h"

static const char *TAG_IR_MANAGER = "infrared_manager";

bool infrared_manager_init(void) {
    bool ok = sd_card_manager.is_initialized;
    if (ok && CONFIG_HAS_INFRARED) {
        gpio_reset_pin(CONFIG_INFRARED_LED_PIN);
        gpio_set_direction(CONFIG_INFRARED_LED_PIN, GPIO_MODE_OUTPUT);
        gpio_set_level(CONFIG_INFRARED_LED_PIN, 0);
        ESP_LOGI(TAG_IR_MANAGER, "IR LED pin initialized: %d", CONFIG_INFRARED_LED_PIN);
    }
    return ok;
}

static char *read_file_to_buffer(const char *path) {
    if (!sd_card_manager.is_initialized) {
        ESP_LOGE(TAG_IR_MANAGER, "sd card not initialized for file: %s", path);
        return NULL;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG_IR_MANAGER, "failed to open file: %s", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size <= 0) {
        ESP_LOGE(TAG_IR_MANAGER, "file is empty or could not get size: %s", path);
        fclose(f);
        return NULL;
    }
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(size + 1);
    if (!buf) {
        ESP_LOGE(TAG_IR_MANAGER, "failed to allocate buffer for file: %s", path);
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, size, f) != (size_t)size) {
        ESP_LOGE(TAG_IR_MANAGER, "failed to read file content: %s", path);
        free(buf);
        fclose(f);
        return NULL;
    }
    buf[size] = '\0';
    fclose(f);
    ESP_LOGI(TAG_IR_MANAGER, "successfully read file to buffer: %s (size: %ld)", path, size);
    return buf;
}

bool infrared_manager_read_file(const char *path, infrared_signal_t *signal) {
    ESP_LOGI(TAG_IR_MANAGER, "attempting to read IR signal from file: %s", path);
    if (!signal) {
        ESP_LOGE(TAG_IR_MANAGER, "invalid signal pointer provided");
        return false;
    }
    char *buf = read_file_to_buffer(path);
    if (!buf) {
        return false;
    }
    cJSON *json = cJSON_Parse(buf);
    free(buf);
    if (!json) {
        ESP_LOGE(TAG_IR_MANAGER, "failed to parse json from file: %s", path);
        return false;
    }
    cJSON *type = cJSON_GetObjectItemCaseSensitive(json, "type");
    if (!cJSON_IsString(type) || (type->valuestring == NULL)) {
        ESP_LOGE(TAG_IR_MANAGER, "json missing 'type' field or invalid type for file: %s", path);
        cJSON_Delete(json);
        return false;
    }
    if (strcmp(type->valuestring, "raw") == 0) {
        cJSON *freq = cJSON_GetObjectItemCaseSensitive(json, "frequency");
        cJSON *duty = cJSON_GetObjectItemCaseSensitive(json, "duty_cycle");
        cJSON *data = cJSON_GetObjectItemCaseSensitive(json, "data");
        if (!cJSON_IsNumber(freq) || !cJSON_IsNumber(duty) || !cJSON_IsArray(data)) {
            ESP_LOGE(TAG_IR_MANAGER, "raw signal json missing 'frequency', 'duty_cycle', or 'data' for file: %s", path);
            cJSON_Delete(json);
            return false;
        }
        size_t count = cJSON_GetArraySize(data);
        uint32_t *timings = malloc(sizeof(uint32_t) * count);
        if (!timings) {
            ESP_LOGE(TAG_IR_MANAGER, "failed to allocate timings for raw signal from file: %s", path);
            cJSON_Delete(json);
            return false;
        }
        for (size_t i = 0; i < count; i++) {
            cJSON *item = cJSON_GetArrayItem(data, i);
            if (!cJSON_IsNumber(item)) {
                ESP_LOGE(TAG_IR_MANAGER, "invalid timing data in raw signal from file: %s at index %zu", path, i);
                free(timings);
                cJSON_Delete(json);
                return false;
            }
            timings[i] = (uint32_t)item->valuedouble;
        }
        signal->is_raw = true;
        signal->payload.raw.timings = timings;
        signal->payload.raw.timings_size = count;
        signal->payload.raw.frequency = (uint32_t)freq->valuedouble;
        signal->payload.raw.duty_cycle = (float)duty->valuedouble;
        ESP_LOGI(TAG_IR_MANAGER, "successfully parsed raw IR signal from file: %s", path);
    } else if (strcmp(type->valuestring, "parsed") == 0) {
        cJSON *protocol = cJSON_GetObjectItemCaseSensitive(json, "protocol");
        cJSON *address = cJSON_GetObjectItemCaseSensitive(json, "address");
        cJSON *command = cJSON_GetObjectItemCaseSensitive(json, "command");
        if (!cJSON_IsString(protocol) || !cJSON_IsNumber(address) || !cJSON_IsNumber(command)) {
            ESP_LOGE(TAG_IR_MANAGER, "parsed signal json missing 'protocol', 'address', or 'command' for file: %s", path);
            cJSON_Delete(json);
            return false;
        }
        signal->is_raw = false;
        strncpy(signal->payload.message.protocol, protocol->valuestring, sizeof(signal->payload.message.protocol) - 1);
        signal->payload.message.protocol[sizeof(signal->payload.message.protocol) - 1] = '\0';
        signal->payload.message.address = (uint32_t)address->valuedouble;
        signal->payload.message.command = (uint32_t)command->valuedouble;
        ESP_LOGI(TAG_IR_MANAGER, "successfully parsed standard IR signal from file: %s", path);
    } else {
        ESP_LOGE(TAG_IR_MANAGER, "unsupported IR signal type: %s for file: %s", type->valuestring, path);
        cJSON_Delete(json);
        return false;
    }
    cJSON_Delete(json);
    return true;
}

void infrared_manager_free_signal(infrared_signal_t *signal) {
    if (!signal) return;
    if (signal->is_raw && signal->payload.raw.timings) {
        free(signal->payload.raw.timings);
        signal->payload.raw.timings = NULL;
        signal->payload.raw.timings_size = 0;
    }
}

// parse a single IR signal JSON object into infrared_signal_t
static bool parse_signal_json(cJSON *json_obj, infrared_signal_t *signal) {
    // parse name field
    cJSON *name_item = cJSON_GetObjectItemCaseSensitive(json_obj, "name");
    if (cJSON_IsString(name_item) && name_item->valuestring) {
        strncpy(signal->name, name_item->valuestring, sizeof(signal->name) - 1);
        signal->name[sizeof(signal->name) - 1] = '\0';
    } else {
        signal->name[0] = '\0';
        ESP_LOGW(TAG_IR_MANAGER, "signal json missing 'name' field, using empty string");
    }
    cJSON *type = cJSON_GetObjectItemCaseSensitive(json_obj, "type");
    if (!cJSON_IsString(type) || (type->valuestring == NULL)) {
        ESP_LOGE(TAG_IR_MANAGER, "signal json missing 'type' field or invalid type");
        return false;
    }
    if (strcmp(type->valuestring, "raw") == 0) {
        cJSON *freq = cJSON_GetObjectItemCaseSensitive(json_obj, "frequency");
        cJSON *duty = cJSON_GetObjectItemCaseSensitive(json_obj, "duty_cycle");
        cJSON *data = cJSON_GetObjectItemCaseSensitive(json_obj, "data");
        if (!cJSON_IsNumber(freq) || !cJSON_IsNumber(duty) || !cJSON_IsArray(data)) {
            ESP_LOGE(TAG_IR_MANAGER, "raw signal json missing 'frequency', 'duty_cycle', or 'data'");
            return false;
        }
        size_t count = cJSON_GetArraySize(data);
        uint32_t *timings = malloc(sizeof(uint32_t) * count);
        if (!timings) {
            ESP_LOGE(TAG_IR_MANAGER, "failed to allocate timings for raw signal");
            return false;
        }
        for (size_t i = 0; i < count; i++) {
            cJSON *item = cJSON_GetArrayItem(data, i);
            if (!cJSON_IsNumber(item)) {
                ESP_LOGE(TAG_IR_MANAGER, "invalid timing data in raw signal at index %zu", i);
                free(timings);
                return false;
            }
            timings[i] = (uint32_t)item->valuedouble;
        }
        signal->is_raw = true;
        signal->payload.raw.timings = timings;
        signal->payload.raw.timings_size = count;
        signal->payload.raw.frequency = (uint32_t)freq->valuedouble;
        signal->payload.raw.duty_cycle = (float)duty->valuedouble;
    } else if (strcmp(type->valuestring, "parsed") == 0) {
        cJSON *protocol = cJSON_GetObjectItemCaseSensitive(json_obj, "protocol");
        cJSON *address = cJSON_GetObjectItemCaseSensitive(json_obj, "address");
        cJSON *command = cJSON_GetObjectItemCaseSensitive(json_obj, "command");
        if (!cJSON_IsString(protocol) || !cJSON_IsNumber(address) || !cJSON_IsNumber(command)) {
            ESP_LOGE(TAG_IR_MANAGER, "parsed signal json missing 'protocol', 'address', or 'command'");
            return false;
        }
        signal->is_raw = false;
        strncpy(signal->payload.message.protocol, protocol->valuestring, sizeof(signal->payload.message.protocol) - 1);
        signal->payload.message.protocol[sizeof(signal->payload.message.protocol) - 1] = '\0';
        signal->payload.message.address = (uint32_t)address->valuedouble;
        signal->payload.message.command = (uint32_t)command->valuedouble;
    } else {
        ESP_LOGE(TAG_IR_MANAGER, "unsupported IR signal type: %s", type->valuestring);
        return false;
    }
    return true;
}

static bool read_file_binary(const char *path, uint8_t **buf, size_t *buf_len) { 
    if (!sd_card_manager.is_initialized) return false;
    FILE *f = fopen(path,"rb");
    if(!f) return false;
    fseek(f,0,SEEK_END);
    long size = ftell(f);
    if(size<=0){fclose(f);return false;}
    fseek(f,0,SEEK_SET);
    uint8_t *b=malloc(size);
    if(!b){fclose(f);return false;}
    if(fread(b,1,size,f)!=(size_t)size){free(b);fclose(f);return false;}
    fclose(f);
    *buf=b;*buf_len=size;
    return true;
}

static bool parse_tlv_list(const uint8_t *buf, size_t buf_len, infrared_signal_t **signals, size_t *count){
    size_t idx = 0;
    if (buf_len >= 4 && buf[0] == 'R' && buf[1] == 'F' && buf[2] == 'I' && buf[3] == 'L') {
        ESP_LOGI(TAG_IR_MANAGER, "TLV magic header detected, skipping 4 bytes");
        idx = 4;
    }
    ESP_LOGI(TAG_IR_MANAGER, "starting tlv parse at offset %zu, buffer length: %zu", idx, buf_len);
    while(idx<buf_len){
        uint8_t tag=buf[idx++];
        uint16_t klen=buf[idx]|(buf[idx+1]<<8);idx+=2;
        const char *key=(const char*)&buf[idx];idx+=klen;
        ESP_LOGI(TAG_IR_MANAGER, "read top-level tag: %u, key: %s, klen: %u", tag, key, klen);
        uint32_t cnt=buf[idx]|(buf[idx+1]<<8)|(buf[idx+2]<<16)|(buf[idx+3]<<24);idx+=4;
        if(strcmp(key,"filetype")==0||strcmp(key,"version")==0){
            for(uint32_t i=0;i<cnt;i++){
                if(tag==6){uint16_t sl=buf[idx]|(buf[idx+1]<<8);idx+=2+sl;}
                else if(tag==0||tag==2) idx+=cnt;
                else if(tag==1||tag==7) idx+=cnt*4;
                else return false;
            }
            continue;
        }
        infrared_signal_t *list=NULL;size_t lc=0,lp=0;
        infrared_signal_t cur;bool in=false;
        while(idx<buf_len){
            uint8_t t=buf[idx++];
            uint16_t kl=buf[idx]|(buf[idx+1]<<8);idx+=2;
            char k[kl+1];memcpy(k,&buf[idx],kl);k[kl]='\0';idx+=kl;
            ESP_LOGI(TAG_IR_MANAGER, "read inner tag: %u, key: %s, klen: %u", t, k, kl);
            uint32_t ct=buf[idx]|(buf[idx+1]<<8)|(buf[idx+2]<<16)|(buf[idx+3]<<24);idx+=4;
            if(t==6){uint16_t sl=buf[idx]|(buf[idx+1]<<8);idx+=2;char v[sl+1];memcpy(v,&buf[idx],sl);v[sl]='\0';idx+=sl;
                ESP_LOGI(TAG_IR_MANAGER, "read string value: %s", v);
                if(strcmp(k,"name")==0){
                    if(in){if(lc==lp){size_t nc=lp?lp*2:4;infrared_signal_t*tmp=realloc(list,nc*sizeof(infrared_signal_t));if(!tmp) return false;list=tmp;lp=nc;}list[lc++]=cur;}
                    memset(&cur,0,sizeof(cur));in=true;strncpy(cur.name,v,sizeof(cur.name)-1);
                } else if(strcmp(k,"type")==0){cur.is_raw=(strcmp(v,"raw")==0);} else if(strcmp(k,"protocol")==0){strncpy(cur.payload.message.protocol,v,sizeof(cur.payload.message.protocol)-1);} }
            else if(t==1&&strcmp(k,"frequency")==0){uint32_t w=buf[idx]|(buf[idx+1]<<8)|(buf[idx+2]<<16)|(buf[idx+3]<<24);float f;memcpy(&f,&w,4);cur.payload.raw.frequency=(uint32_t)f;idx+=4;
                ESP_LOGI(TAG_IR_MANAGER, "read frequency: %lu", cur.payload.raw.frequency);
            }
            else if(t==1&&strcmp(k,"duty_cycle")==0){uint32_t w=buf[idx]|(buf[idx+1]<<8)|(buf[idx+2]<<16)|(buf[idx+3]<<24);float f;memcpy(&f,&w,4);cur.payload.raw.duty_cycle=f;idx+=4;
                ESP_LOGI(TAG_IR_MANAGER, "read duty cycle: %f", cur.payload.raw.duty_cycle);
            }
            else if(t==7&&strcmp(k,"data")==0){uint32_t dct=ct;uint32_t*arr=malloc(dct*4);for(uint32_t i=0;i<dct;i++){arr[i]=buf[idx]|(buf[idx+1]<<8)|(buf[idx+2]<<16)|(buf[idx+3]<<24);idx+=4;}cur.payload.raw.timings=arr;cur.payload.raw.timings_size=dct;
                ESP_LOGI(TAG_IR_MANAGER, "read raw data with %lu timings", dct);
            }
            else if(t==2&&(strcmp(k,"address")==0||strcmp(k,"command")==0)){uint32_t v=0;for(uint32_t i=0;i<ct;i++){v=(v<<8)|buf[idx++];}if(strcmp(k,"address")==0)cur.payload.message.address=v;else cur.payload.message.command=v;
                ESP_LOGI(TAG_IR_MANAGER, "read address/command: %lu", v);
            }
            else{if(t==6){for(uint32_t i=0;i<ct;i++){uint16_t sl=buf[idx]|(buf[idx+1]<<8);idx+=2+sl;}}else if(t==1||t==7){idx+=ct*4;}else if(t==2){idx+=ct;}else return false;}
        }
        if(in){if(lc==lp){size_t nc=lp?lp*2:4;infrared_signal_t*tmp=realloc(list,nc*sizeof(infrared_signal_t));if(!tmp) return false;list=tmp;lp=nc;}list[lc++]=cur;}
        if (lc == 0) {
            return false;
        }
        *signals = list;
        *count = lc;
        return true;
    }
    ESP_LOGI(TAG_IR_MANAGER, "tlv parsing finished");
    return false;
}

static bool parse_ir_file(const char *buf, const char *path, infrared_signal_t **signals, size_t *count) {
    char *dup = strdup(buf);
    if (!dup) return false;
    infrared_signal_t *list = NULL;
    size_t list_count = 0, list_capacity = 0;
    infrared_signal_t current;
    bool in_block = false;
    char *saveptr;
    char *line = strtok_r(dup, "\r\n", &saveptr);
    while (line) {
        char *s = line;
        while (*s && isspace((unsigned char)*s)) s++;
        if (*s == '\0' || *s == '#') { line = strtok_r(NULL, "\r\n", &saveptr); continue; }
        char *colon = strchr(s, ':');
        if (!colon) { line = strtok_r(NULL, "\r\n", &saveptr); continue; }
        *colon = '\0'; char *key = s; char *value = colon + 1;
        char *end = key + strlen(key) - 1;
        while (end > key && isspace((unsigned char)*end)) *end-- = '\0';
        while (*value && isspace((unsigned char)*value)) value++;
        char *v_end = value + strlen(value) - 1;
        while (v_end > value && isspace((unsigned char)*v_end)) *v_end-- = '\0';
        if (strcmp(key, "name") == 0) {
            if (in_block) {
                if (list_count == list_capacity) {
                    size_t new_cap = list_capacity ? list_capacity * 2 : 4;
                    infrared_signal_t *tmp = realloc(list, new_cap * sizeof(infrared_signal_t));
                    if (!tmp) { free(list); free(dup); return false; }
                    list = tmp; list_capacity = new_cap;
                }
                list[list_count++] = current;
            }
            memset(&current, 0, sizeof(current)); in_block = true;
            strncpy(current.name, value, sizeof(current.name) - 1);
        } else if (in_block && strcmp(key, "type") == 0) {
            current.is_raw = (strcmp(value, "raw") == 0);
        } else if (in_block && current.is_raw) {
            if (strcmp(key, "frequency") == 0) {
                current.payload.raw.frequency = (uint32_t)strtoul(value, NULL, 10);
            } else if (strcmp(key, "duty_cycle") == 0) {
                current.payload.raw.duty_cycle = strtof(value, NULL);
            } else if (strcmp(key, "data") == 0) {
                size_t data_count = 0; const char *p2 = value;
                while (*p2) { while (*p2 && isspace((unsigned char)*p2)) p2++; if (!*p2) break; data_count++; while (*p2 && !isspace((unsigned char)*p2)) p2++; }
                uint32_t *timings = malloc(sizeof(uint32_t) * data_count);
                if (!timings) { free(list); free(dup); return false; }
                size_t idx2 = 0; p2 = value; char *endptr;
                while (*p2) { while (*p2 && isspace((unsigned char)*p2)) p2++; if (!*p2) break; unsigned long v = strtoul(p2, &endptr, 10); timings[idx2++] = (uint32_t)v; p2 = endptr; }
                current.payload.raw.timings = timings; current.payload.raw.timings_size = data_count;
            }
        } else if (in_block && !current.is_raw) {
            if (strcmp(key, "protocol") == 0) {
                strncpy(current.payload.message.protocol, value, sizeof(current.payload.message.protocol) - 1);
            } else if (strcmp(key, "address") == 0) {
                uint32_t addr = 0; const char *p2 = value; char *endptr; uint8_t shift = 0;
                while (*p2) { while (*p2 && isspace((unsigned char)*p2)) p2++; if (!*p2) break; unsigned long b = strtoul(p2, &endptr, 16); addr |= (uint32_t)(b & 0xFF) << shift; shift += 8; p2 = endptr; }
                current.payload.message.address = addr;
            } else if (strcmp(key, "command") == 0) {
                uint32_t cmd = 0; const char *p2 = value; char *endptr; uint8_t shift = 0;
                while (*p2) { while (*p2 && isspace((unsigned char)*p2)) p2++; if (!*p2) break; unsigned long b = strtoul(p2, &endptr, 16); cmd |= (uint32_t)(b & 0xFF) << shift; shift += 8; p2 = endptr; }
                current.payload.message.command = cmd;
            }
        }
        line = strtok_r(NULL, "\r\n", &saveptr);
    }
    if (in_block) {
        if (list_count == list_capacity) {
            size_t new_cap = list_capacity ? list_capacity * 2 : 4;
            infrared_signal_t *tmp = realloc(list, new_cap * sizeof(infrared_signal_t));
            if (!tmp) { free(list); free(dup); return false; }
            list = tmp; list_capacity = new_cap;
        }
        list[list_count++] = current;
    }
    free(dup);
    if (list_count == 0) { free(list); return false; }
    *signals = list; *count = list_count;
    return true;
}

// read a JSON file containing an array of IR signal objects
bool infrared_manager_read_list(const char *path, infrared_signal_t **signals, size_t *count) {
    char *buf = read_file_to_buffer(path);
    if (buf) {
        bool ok = parse_ir_file(buf, path, signals, count);
        free(buf);
        if (ok) return true;
    }
    uint8_t *binbuf = NULL; size_t binlen = 0;
    if (read_file_binary(path, &binbuf, &binlen)) {
        bool ok = parse_tlv_list(binbuf, binlen, signals, count);
        free(binbuf);
        if (ok) return true;
    }
    char *json_buf = read_file_to_buffer(path);
    if (!json_buf) return false;
    cJSON *json = cJSON_Parse(json_buf);
    free(json_buf);
    if (!json) return false;
    cJSON*array=cJSON_IsArray(json)?json:cJSON_GetObjectItemCaseSensitive(json,"signals");
    if(!cJSON_IsArray(array)){cJSON_Delete(json);return false;}
    *count=cJSON_GetArraySize(array);
    *signals=malloc((*count)*sizeof(infrared_signal_t));
    if(!*signals){cJSON_Delete(json);return false;}
    for(size_t i=0;i<*count;i++){infrared_signal_t*s=&(*signals)[i];memset(s,0,sizeof(*s));cJSON*item=cJSON_GetArrayItem(array,i);if(!parse_signal_json(item,s)){for(size_t j=0;j<i;j++)infrared_manager_free_signal(&(*signals)[j]);free(*signals);cJSON_Delete(json);return false;}}
    cJSON_Delete(json);
    return true;
}

// free a list of IR signals
void infrared_manager_free_list(infrared_signal_t *signals, size_t count) {
    if (!signals) return;
    for (size_t i = 0; i < count; i++) {
        infrared_manager_free_signal(&signals[i]);
    }
    free(signals);
    ESP_LOGI(TAG_IR_MANAGER, "freed %zu IR signals", count);
}

static const InfraredCommonProtocolSpec* infrared_manager_get_protocol_spec(const char* name) {
    if (strcasecmp(name, "nec") == 0) return &infrared_protocol_nec;
    if (strcasecmp(name, "necext") == 0) return &infrared_protocol_necext;
    if (strcasecmp(name, "kaseikyo") == 0) return &infrared_protocol_kaseikyo;
    if (strcasecmp(name, "pioneer") == 0) return &infrared_protocol_pioneer;
    if (strcasecmp(name, "rca") == 0) return &infrared_protocol_rca;
    if (strcasecmp(name, "samsung32") == 0) return &infrared_protocol_samsung;
    if (strcasecmp(name, "samsung") == 0) return &infrared_protocol_samsung;
    if (strcasecmp(name, "sirc") == 0) return &infrared_protocol_sirc;
    if (strcasecmp(name, "rc5") == 0) return &infrared_protocol_rc5;
    if (strcasecmp(name, "rc6") == 0) return &infrared_protocol_rc6;
    return NULL;
}

static bool send_rmt(const uint32_t *timings, size_t count, uint32_t freq, float duty) {
    size_t item_count = (count + 1) / 2;
    size_t block_symbols = item_count;
    if (block_symbols < 48) block_symbols = 48;
    if (block_symbols % 2 != 0) block_symbols++;
    ESP_LOGI(TAG_IR_MANAGER, "send_rmt: count=%zu, item_count=%zu, block_symbols=%zu, freq=%" PRIu32 ", duty=%.2f", count, item_count, block_symbols, freq, duty);
    rmt_symbol_word_t *symbols = heap_caps_malloc(block_symbols * sizeof(rmt_symbol_word_t), MALLOC_CAP_DMA);
    if (!symbols) {
        ESP_LOGE(TAG_IR_MANAGER, "send_rmt: failed to allocate symbols");
        return false;
    }
    for (size_t i = 0; i < item_count; i++) {
        symbols[i].level0 = 1;
        symbols[i].duration0 = timings[2 * i];
        symbols[i].level1 = 0;
        symbols[i].duration1 = (2 * i + 1 < count) ? timings[2 * i + 1] : 0;
    }
    rmt_tx_channel_config_t tx_chan_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = CONFIG_INFRARED_LED_PIN,
        .mem_block_symbols = block_symbols,
        .resolution_hz = 1000000,
        .trans_queue_depth = 1,
        .flags = {
            .with_dma = true,
            .invert_out = false
        }
    };
    rmt_channel_handle_t tx_chan = NULL;
    esp_err_t err = rmt_new_tx_channel(&tx_chan_config, &tx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_IR_MANAGER, "send_rmt: rmt_new_tx_channel failed: %s", esp_err_to_name(err));
        heap_caps_free(symbols);
        return false;
    }
    ESP_LOGI(TAG_IR_MANAGER, "send_rmt: channel created");
    err = rmt_enable(tx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_IR_MANAGER, "send_rmt: rmt_enable failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG_IR_MANAGER, "send_rmt: channel enabled");
        rmt_carrier_config_t carrier_cfg = {
            .frequency_hz = freq,
            .duty_cycle = duty,
            .flags.polarity_active_low = false
        };
        err = rmt_apply_carrier(tx_chan, &carrier_cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG_IR_MANAGER, "send_rmt: rmt_apply_carrier failed: %s", esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG_IR_MANAGER, "send_rmt: carrier applied");
        }
        rmt_copy_encoder_config_t copy_config = {};
        rmt_encoder_handle_t copy_encoder = NULL;
        err = rmt_new_copy_encoder(&copy_config, &copy_encoder);
        if (err != ESP_OK) {
            ESP_LOGE(TAG_IR_MANAGER, "send_rmt: rmt_new_copy_encoder failed: %s", esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG_IR_MANAGER, "send_rmt: copy encoder created");
            err = rmt_transmit(tx_chan, copy_encoder, symbols, item_count * sizeof(rmt_symbol_word_t), &(rmt_transmit_config_t){.loop_count = 0});
            rmt_del_encoder(copy_encoder);
            if (err != ESP_OK) {
                ESP_LOGE(TAG_IR_MANAGER, "send_rmt: rmt_transmit failed: %s", esp_err_to_name(err));
            } else {
                ESP_LOGI(TAG_IR_MANAGER, "send_rmt: transmit OK");
                err = rmt_tx_wait_all_done(tx_chan, pdMS_TO_TICKS(1000));
                if (err != ESP_OK) {
                    ESP_LOGE(TAG_IR_MANAGER, "send_rmt: wait_all_done failed: %s", esp_err_to_name(err));
                } else {
                    ESP_LOGI(TAG_IR_MANAGER, "send_rmt: wait_all_done OK");
                    err = rmt_disable(tx_chan);
                    if (err != ESP_OK) {
                        ESP_LOGE(TAG_IR_MANAGER, "send_rmt: rmt_disable failed: %s", esp_err_to_name(err));
                    }
                }
            }
        }
    }
    rmt_del_channel(tx_chan);
    heap_caps_free(symbols);
    return (err == ESP_OK);
}

bool infrared_manager_transmit(const infrared_signal_t *signal) {
    if (!signal) return false;
    printf("transmitting IR signal (name: %s)\n", signal->name);
    gpio_set_level(CONFIG_INFRARED_LED_PIN, 1);
    rgb_manager_set_color(&rgb_manager, -1, 255, 0, 255, false);
    bool ok = false;
    if (signal->is_raw) {
        ok = send_rmt(signal->payload.raw.timings,
                      signal->payload.raw.timings_size,
                      signal->payload.raw.frequency,
                      signal->payload.raw.duty_cycle);
    } else {
        const InfraredCommonProtocolSpec* protocol_spec = infrared_manager_get_protocol_spec(signal->payload.message.protocol);
        if (protocol_spec) {
            InfraredCommonEncoder* enc = infrared_common_encoder_alloc(protocol_spec);
            protocol_spec->reset(enc, (const InfraredMessage*)&signal->payload.message);
            
            size_t max_bits = 0;
            for(int i=0; i<4; ++i) if(protocol_spec->databit_len[i] > max_bits) max_bits = protocol_spec->databit_len[i];
            
            size_t max_timings = 2 + max_bits * 2 + 10;
            uint32_t* timings = malloc(max_timings * sizeof(uint32_t));
            
            if (timings) {
                size_t timing_count = 0;
                InfraredStatus st;
                uint32_t dur;
                bool level;
                // skip initial silence
                st = infrared_common_encode(enc, &dur, &level);
                while (st == InfraredStatusOk && level == false) {
                    st = infrared_common_encode(enc, &dur, &level);
                }
                if (st == InfraredStatusOk) {
                    // collect primary frame durations
                    do {
                        timings[timing_count++] = dur;
                        st = infrared_common_encode(enc, &dur, &level);
                    } while (st == InfraredStatusOk && timing_count < max_timings);
                    // include final duration if Done
                    if (st == InfraredStatusDone && timing_count < max_timings) {
                        timings[timing_count++] = dur;
                    }
                }
                if (timing_count > 0) {
                    ok = send_rmt(timings, timing_count,
                                  protocol_spec->carrier_frequency,
                                  protocol_spec->duty_cycle);
                }
                free(timings);
            }
            infrared_common_encoder_free(enc);
        } else {
            ESP_LOGE(TAG_IR_MANAGER, "unsupported IR protocol: %s", signal->payload.message.protocol);
            ok = false;
        }
    }
    gpio_set_level(CONFIG_INFRARED_LED_PIN, 0);
    rgb_manager_set_color(&rgb_manager, -1, 0, 0, 0, false);
    printf("ir signal transmission complete (name: %s, status: %s)\n", signal->name, ok ? "OK" : "FAIL");
    return ok;
}

// brute force transmit all signals in a list with delay
bool infrared_manager_bruteforce(const char *path, uint32_t delay_ms) {
    ESP_LOGI(TAG_IR_MANAGER, "starting IR brute force for file: %s with delay: %lu ms", path, delay_ms);
    infrared_signal_t *signals = NULL;
    size_t count = 0;
    if (!infrared_manager_read_list(path, &signals, &count)) {
        ESP_LOGE(TAG_IR_MANAGER, "failed to read IR list for brute force from file: %s", path);
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        infrared_manager_transmit(&signals[i]);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
    infrared_manager_free_list(signals, count);
    ESP_LOGI(TAG_IR_MANAGER, "IR brute force complete for file: %s", path);
    return true;
} 