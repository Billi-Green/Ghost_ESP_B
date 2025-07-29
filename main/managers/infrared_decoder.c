#include "managers/infrared_decoder.h"
#include "managers/infrared_timings.h"
#include <stdlib.h>
#include <string.h>
#include <esp_log.h>


static const char* TAG = "IR_DECODER";

// Protocol specifications
static const InfraredDecoderProtocolSpec infrared_protocol_nec_decoder = {
    .timings = {
        .preamble_mark = INFRARED_NEC_PREAMBLE_MARK,
        .preamble_space = INFRARED_NEC_PREAMBLE_SPACE,
        .bit1_mark = INFRARED_NEC_BIT1_MARK,
        .bit1_space = INFRARED_NEC_BIT1_SPACE,
        .bit0_mark = INFRARED_NEC_BIT0_MARK,
        .bit0_space = INFRARED_NEC_BIT0_SPACE,
        .preamble_tolerance = INFRARED_NEC_PREAMBLE_TOLERANCE,
        .bit_tolerance = INFRARED_NEC_BIT_TOLERANCE,
        .silence_time = INFRARED_NEC_SILENCE,
        .min_split_time = INFRARED_NEC_MIN_SPLIT_TIME,
    },
    .databit_len = {32, 42, 0, 0},
    .manchester_start_from_space = false,
    .decode = infrared_common_decode_pdwm,
    .decode_repeat = infrared_decoder_nec_decode_repeat,
    .interpret = infrared_decoder_nec_interpret,
    .reset = NULL,
};

static const InfraredDecoderProtocolSpec infrared_protocol_samsung32_decoder = {
    .timings = {
        .preamble_mark = 4500,
        .preamble_space = 4500,
        .bit1_mark = 550,
        .bit1_space = 1650,
        .bit0_mark = 550,
        .bit0_space = 550,
        .preamble_tolerance = 200,
        .bit_tolerance = 120,
        .silence_time = 145000,
        .min_split_time = 5000,
    },
    .databit_len = {32, 0, 0, 0},
    .manchester_start_from_space = false,
    .decode = infrared_common_decode_pdwm,
    .decode_repeat = infrared_decoder_samsung32_decode_repeat,
    .interpret = infrared_decoder_samsung32_interpret,
    .reset = NULL,
};

static const InfraredDecoderProtocolSpec infrared_protocol_sirc_decoder = {
    .timings = {
        .preamble_mark = INFRARED_SIRC_PREAMBLE_MARK,
        .preamble_space = INFRARED_SIRC_PREAMBLE_SPACE,
        .bit1_mark = INFRARED_SIRC_BIT1_MARK,
        .bit1_space = INFRARED_SIRC_BIT1_SPACE,
        .bit0_mark = INFRARED_SIRC_BIT0_MARK,
        .bit0_space = INFRARED_SIRC_BIT0_SPACE,
        .preamble_tolerance = INFRARED_SIRC_PREAMBLE_TOLERANCE,
        .bit_tolerance = INFRARED_SIRC_BIT_TOLERANCE,
        .silence_time = INFRARED_SIRC_SILENCE,
        .min_split_time = INFRARED_SIRC_MIN_SPLIT_TIME,
    },
    .databit_len = {20, 15, 12, 0},
    .manchester_start_from_space = false,
    .decode = infrared_common_decode_sirc,
    .decode_repeat = NULL,
    .interpret = infrared_decoder_sirc_interpret,
    .reset = NULL,
};

static const InfraredDecoderProtocolSpec infrared_protocol_rc5_decoder = {
    .timings = {
        .preamble_mark = INFRARED_RC5_PREAMBLE_MARK,
        .preamble_space = INFRARED_RC5_PREAMBLE_SPACE,
        .bit1_mark = INFRARED_RC5_BIT,
        .bit1_space = 0,
        .bit0_mark = 0,
        .bit0_space = 0,
        .preamble_tolerance = INFRARED_RC5_PREAMBLE_TOLERANCE,
        .bit_tolerance = INFRARED_RC5_BIT_TOLERANCE,
        .silence_time = INFRARED_RC5_SILENCE,
        .min_split_time = INFRARED_RC5_MIN_SPLIT_TIME,
    },
    .databit_len = {14, 0, 0, 0}, // 1 + 1 + 1 + 5 + 6
    .manchester_start_from_space = true,
    .decode = infrared_common_decode_manchester,
    .decode_repeat = NULL,
    .interpret = infrared_decoder_rc5_interpret,
    .reset = NULL,
};

static const InfraredDecoderProtocolSpec infrared_protocol_rc6_decoder = {
    .timings = {
        .preamble_mark = INFRARED_RC6_PREAMBLE_MARK,
        .preamble_space = INFRARED_RC6_PREAMBLE_SPACE,
        .bit1_mark = INFRARED_RC6_BIT,
        .bit1_space = 0,
        .bit0_mark = 0,
        .bit0_space = 0,
        .preamble_tolerance = INFRARED_RC6_PREAMBLE_TOLERANCE,
        .bit_tolerance = INFRARED_RC6_BIT_TOLERANCE,
        .silence_time = INFRARED_RC6_SILENCE,
        .min_split_time = INFRARED_RC6_MIN_SPLIT_TIME,
    },
    .databit_len = {21, 0, 0, 0}, // 1 + 3 + 1 + 8 + 8
    .manchester_start_from_space = false,
    .decode = infrared_decoder_rc6_decode_manchester,
    .decode_repeat = NULL,
    .interpret = infrared_decoder_rc6_interpret,
    .reset = NULL,
};

static const InfraredDecoderProtocolSpec infrared_protocol_rca_decoder = {
    .timings = {
        .preamble_mark = 4000,
        .preamble_space = 4000,
        .bit1_mark = 500,
        .bit1_space = 2000,
        .bit0_mark = 500,
        .bit0_space = 1000,
        .preamble_tolerance = 200,
        .bit_tolerance = 120,
        .silence_time = 8000,
        .min_split_time = 4000,
    },
    .databit_len = {24, 0, 0, 0},
    .manchester_start_from_space = false,
    .decode = infrared_common_decode_pdwm,
    .decode_repeat = NULL,
    .interpret = infrared_decoder_rca_interpret,
    .reset = NULL,
};

static const InfraredDecoderProtocolSpec infrared_protocol_pioneer_decoder = {
    .timings = {
        .preamble_mark = 8500,
        .preamble_space = 4225,
        .bit1_mark = 500,
        .bit1_space = 1500,
        .bit0_mark = 500,
        .bit0_space = 500,
        .preamble_tolerance = 200,
        .bit_tolerance = 120,
        .silence_time = 26000,
        .min_split_time = 26000,
    },
    .databit_len = {33, 32, 0, 0},
    .manchester_start_from_space = false,
    .decode = infrared_common_decode_pdwm,
    .decode_repeat = NULL,
    .interpret = infrared_decoder_pioneer_interpret,
    .reset = NULL,
};

static const InfraredDecoderProtocolSpec infrared_protocol_kaseikyo_decoder = {
    .timings = {
        .preamble_mark = 3360,
        .preamble_space = 1665,
        .bit1_mark = 420,
        .bit1_space = 1274,
        .bit0_mark = 420,
        .bit0_space = 420,
        .preamble_tolerance = 200,
        .bit_tolerance = 120,
        .silence_time = 74000,
        .min_split_time = 74000,
    },
    .databit_len = {48, 0, 0, 0},
    .manchester_start_from_space = false,
    .decode = infrared_common_decode_pdwm,
    .decode_repeat = NULL,
    .interpret = infrared_decoder_kaseikyo_interpret,
    .reset = NULL,
};

// Utility function to reverse bits in a byte
uint8_t reverse(uint8_t value) {
    uint8_t result = 0;
    for (int i = 0; i < 8; i++) {
        result = (result << 1) | (value & 1);
        value >>= 1;
    }
    return result;
}

// Convert protocol enum to string
const char* infrared_protocol_to_string(InfraredProtocol protocol) {
    switch (protocol) {
        case InfraredProtocolNEC: return "NEC";
        case InfraredProtocolNECext: return "NECext";
        case InfraredProtocolNEC42: return "NEC42";
        case InfraredProtocolNEC42ext: return "NEC42ext";
        case InfraredProtocolSamsung32: return "Samsung32";
        case InfraredProtocolSIRC: return "SIRC";
        case InfraredProtocolSIRC15: return "SIRC15";
        case InfraredProtocolSIRC20: return "SIRC20";
        case InfraredProtocolRC5: return "RC5";
        case InfraredProtocolRC5X: return "RC5X";
        case InfraredProtocolRC6: return "RC6";
        case InfraredProtocolRCA: return "RCA";
        case InfraredProtocolPioneer: return "Pioneer";
        case InfraredProtocolKaseikyo: return "Kaseikyo";
        default: return "Unknown";
    }
}

// Allocate main decoder context
InfraredDecoderContext* infrared_decoder_alloc(void) {
    InfraredDecoderContext* decoder = malloc(sizeof(InfraredDecoderContext));
    if (!decoder) {
        ESP_LOGE(TAG, "Failed to allocate decoder context");
        return NULL;
    }
    
    memset(decoder, 0, sizeof(InfraredDecoderContext));
    
    // Initialize protocol decoders
    decoder->decoders[0] = infrared_common_decoder_alloc(&infrared_protocol_nec_decoder);
    decoder->decoders[1] = infrared_common_decoder_alloc(&infrared_protocol_samsung32_decoder);
    decoder->decoders[2] = infrared_common_decoder_alloc(&infrared_protocol_sirc_decoder);
    decoder->decoders[3] = infrared_common_decoder_alloc(&infrared_protocol_rc5_decoder);
    decoder->decoders[4] = infrared_common_decoder_alloc(&infrared_protocol_rc6_decoder);
    decoder->decoders[5] = infrared_common_decoder_alloc(&infrared_protocol_rca_decoder);
    decoder->decoders[6] = infrared_common_decoder_alloc(&infrared_protocol_pioneer_decoder);
    decoder->decoders[7] = infrared_common_decoder_alloc(&infrared_protocol_kaseikyo_decoder);
    
    // Set up RC5 decoder context
    if (decoder->decoders[3]) {
        InfraredRc5Decoder* rc5_context = malloc(sizeof(InfraredRc5Decoder));
        if (rc5_context) {
            rc5_context->toggle = false;
            decoder->decoders[3]->context = rc5_context;
        }
    }
    
    // Set up RC6 decoder context
    if (decoder->decoders[4]) {
        InfraredRc6Decoder* rc6_context = malloc(sizeof(InfraredRc6Decoder));
        if (rc6_context) {
            rc6_context->toggle = false;
            decoder->decoders[4]->context = rc6_context;
        }
    }
    
    decoder->decoder_count = 8;
    
    ESP_LOGI(TAG, "Decoder context allocated with %d protocols", decoder->decoder_count);
    return decoder;
}

// Free decoder context
void infrared_decoder_free(InfraredDecoderContext* decoder) {
    if (!decoder) return;
    
    for (int i = 0; i < decoder->decoder_count; i++) {
        if (decoder->decoders[i]) {
            if (decoder->decoders[i]->context) {
                free(decoder->decoders[i]->context);
            }
            infrared_common_decoder_free(decoder->decoders[i]);
        }
    }
    
    free(decoder);
    ESP_LOGI(TAG, "Decoder context freed");
}

// Reset all decoders
void infrared_decoder_reset(InfraredDecoderContext* decoder) {
    if (!decoder) return;
    
    for (int i = 0; i < decoder->decoder_count; i++) {
        if (decoder->decoders[i]) {
            infrared_common_decoder_reset(decoder->decoders[i]);
        }
    }
}

// Main decode function - tries all protocols
InfraredDecodedMessage* infrared_decoder_decode(InfraredDecoderContext* decoder, bool level, uint32_t timing) {
    if (!decoder) return NULL;
    
    for (int i = 0; i < decoder->decoder_count; i++) {
        InfraredCommonDecoder* common_decoder = decoder->decoders[i];
        if (!common_decoder || !common_decoder->protocol) continue;
        
        InfraredDecoderStatus status = common_decoder->protocol->decode(common_decoder, level, timing);
        
        if (status == InfraredDecoderStatusError) {
            ESP_LOGD(TAG, "Protocol %d failed decode - level=%d, timing=%luµs", i, level, timing);
            // Reset decoder on error to prevent state corruption
            infrared_common_decoder_reset(common_decoder);
        } else if (status == InfraredDecoderStatusOk) {
            ESP_LOGD(TAG, "Protocol %d accepted timing - level=%d, timing=%luµs, databit_cnt=%d", i, level, timing, common_decoder->databit_cnt);
        }
        
        if (status == InfraredDecoderStatusReady) {
            ESP_LOGD(TAG, "Protocol %d ready for interpretation, databit_cnt=%d", i, common_decoder->databit_cnt);
            if (common_decoder->protocol->interpret && common_decoder->protocol->interpret(common_decoder)) {
                decoder->last_message = common_decoder->message;
                ESP_LOGI(TAG, "Decoded %s: addr=0x%08lX cmd=0x%08lX repeat=%d (databit_cnt=%d)", 
                        infrared_protocol_to_string(common_decoder->message.protocol),
                        common_decoder->message.address,
                        common_decoder->message.command,
                        common_decoder->message.repeat,
                        common_decoder->databit_cnt);
                return &decoder->last_message;
            } else {
                ESP_LOGD(TAG, "Protocol %d interpretation failed, databit_cnt=%d", i, common_decoder->databit_cnt);
            }
        }
    }
    
    return NULL;
}

// Allocate common decoder
InfraredCommonDecoder* infrared_common_decoder_alloc(const InfraredDecoderProtocolSpec* protocol) {
    if (!protocol) return NULL;
    
    InfraredCommonDecoder* decoder = malloc(sizeof(InfraredCommonDecoder));
    if (!decoder) {
        ESP_LOGE(TAG, "Failed to allocate common decoder");
        return NULL;
    }
    
    memset(decoder, 0, sizeof(InfraredCommonDecoder));
    decoder->protocol = protocol;
    
    return decoder;
}

// Free common decoder
void infrared_common_decoder_free(InfraredCommonDecoder* decoder) {
    if (decoder) {
        free(decoder);
    }
}

// Reset common decoder
void infrared_common_decoder_reset(InfraredCommonDecoder* decoder) {
    if (!decoder) return;
    
    decoder->timings_cnt = 0;
    decoder->databit_cnt = 0;
    decoder->switch_detect = false;
    decoder->state = InfraredDecoderStateIdle;
    memset(decoder->data, 0, sizeof(decoder->data));
    memset(decoder->timings, 0, sizeof(decoder->timings));
}

// Check if decoder is ready
InfraredDecodedMessage* infrared_common_decoder_check_ready(InfraredCommonDecoder* decoder) {
    return (decoder && decoder->message.protocol != InfraredProtocolUnknown) ? &decoder->message : NULL;
}

// Common PDWM (Pulse Distance Width Modulation) decoder
InfraredDecoderStatus infrared_common_decode_pdwm(InfraredCommonDecoder* decoder, bool level, uint32_t timing) {
    if (!decoder || !decoder->protocol) return InfraredDecoderStatusError;
    
    const InfraredTimings* timings = &decoder->protocol->timings;
    
    // Store timing
    if (decoder->timings_cnt < sizeof(decoder->timings) / sizeof(decoder->timings[0])) {
        decoder->timings[decoder->timings_cnt++] = timing;
    }
    
    // State machine for proper preamble detection
    switch (decoder->state) {
        case InfraredDecoderStateIdle:
            // Waiting for preamble mark
            if (level && MATCH_TIMING(timing, timings->preamble_mark, timings->preamble_tolerance)) {
                decoder->state = InfraredDecoderStatePreambleMark;
                ESP_LOGD(TAG, "PDWM: Preamble mark detected: %luµs (expected %luµs ±%luµs)", 
                         timing, timings->preamble_mark, timings->preamble_tolerance);
                return InfraredDecoderStatusOk;
            }
            ESP_LOGD(TAG, "PDWM: Waiting for preamble mark - got level=%d, timing=%luµs", level, timing);
            return InfraredDecoderStatusError;
            
        case InfraredDecoderStatePreambleMark:
            // Waiting for preamble space
            if (!level && MATCH_TIMING(timing, timings->preamble_space, timings->preamble_tolerance)) {
                decoder->state = InfraredDecoderStateData;
                ESP_LOGD(TAG, "PDWM: Preamble space detected: %luµs (expected %luµs ±%luµs) - starting data collection", 
                         timing, timings->preamble_space, timings->preamble_tolerance);
                return InfraredDecoderStatusOk;
            }
            ESP_LOGD(TAG, "PDWM: Expected preamble space but got level=%d, timing=%luµs", level, timing);
            return InfraredDecoderStatusError;
            
        case InfraredDecoderStateData:
            // Decode data bits
            if (level) {
                // Mark phase - validate mark timing but don't decode yet
                if (MATCH_TIMING(timing, timings->bit1_mark, timings->bit_tolerance) ||
                    MATCH_TIMING(timing, timings->bit0_mark, timings->bit_tolerance)) {
                    return InfraredDecoderStatusOk;
                } else {
                    ESP_LOGD(TAG, "PDWM: Invalid mark timing: %luµs", timing);
                    return InfraredDecoderStatusError;
                }
            } else {
                // Space phase - determine bit value
                bool bit_value;
                if (MATCH_TIMING(timing, timings->bit1_space, timings->bit_tolerance)) {
                    bit_value = true;
                } else if (MATCH_TIMING(timing, timings->bit0_space, timings->bit_tolerance)) {
                    bit_value = false;
                } else {
                    ESP_LOGD(TAG, "PDWM: Invalid space timing: %luµs (bit1=%luµs±%lu, bit0=%luµs±%lu)", 
                             timing, timings->bit1_space, timings->bit_tolerance, 
                             timings->bit0_space, timings->bit_tolerance);
                    return InfraredDecoderStatusError;
                }
                
                // Store bit
                uint8_t byte_index = decoder->databit_cnt / 8;
                uint8_t bit_index = decoder->databit_cnt % 8;
                
                if (byte_index < sizeof(decoder->data)) {
                    if (bit_value) {
                        decoder->data[byte_index] |= (1 << bit_index);
                    }
                    decoder->databit_cnt++;
                    
                    ESP_LOGD(TAG, "PDWM: Bit %d = %d (total bits: %d)", decoder->databit_cnt - 1, bit_value, decoder->databit_cnt);
                    
                    // Check if we have reached the maximum possible bits
                    uint32_t max_bits = 0;
                    for (int i = 0; i < 4; i++) {
                        if (decoder->protocol->databit_len[i] > max_bits) {
                            max_bits = decoder->protocol->databit_len[i];
                        }
                    }
                    
                    if (decoder->databit_cnt >= max_bits) {
                        ESP_LOGD(TAG, "PDWM: Collected maximum %d bits, ready for interpretation", decoder->databit_cnt);
                        return InfraredDecoderStatusReady;
                    }
                }
            }
            break;
            
        default:
            ESP_LOGE(TAG, "PDWM: Invalid decoder state: %d", decoder->state);
            return InfraredDecoderStatusError;
    }
    
    return InfraredDecoderStatusOk;
}

// SIRC-specific decoder (uses mark timing for bit values)
InfraredDecoderStatus infrared_common_decode_sirc(InfraredCommonDecoder* decoder, bool level, uint32_t timing) {
    if (!decoder || !decoder->protocol) return InfraredDecoderStatusError;
    
    const InfraredTimings* timings = &decoder->protocol->timings;
    
    // Store timing
    if (decoder->timings_cnt < sizeof(decoder->timings) / sizeof(decoder->timings[0])) {
        decoder->timings[decoder->timings_cnt++] = timing;
    }
    
    // State machine for proper preamble detection
    switch (decoder->state) {
        case InfraredDecoderStateIdle:
            // Waiting for preamble mark
            if (level && MATCH_TIMING(timing, timings->preamble_mark, timings->preamble_tolerance)) {
                decoder->state = InfraredDecoderStatePreambleMark;
                ESP_LOGD(TAG, "SIRC: Preamble mark detected: %luµs (expected %luµs ±%luµs)", 
                         timing, timings->preamble_mark, timings->preamble_tolerance);
                return InfraredDecoderStatusOk;
            }
            ESP_LOGD(TAG, "SIRC: Waiting for preamble mark - got level=%d, timing=%luµs", level, timing);
            return InfraredDecoderStatusError;
            
        case InfraredDecoderStatePreambleMark:
            // Waiting for preamble space
            if (!level && MATCH_TIMING(timing, timings->preamble_space, timings->preamble_tolerance)) {
                decoder->state = InfraredDecoderStateData;
                ESP_LOGD(TAG, "SIRC: Preamble space detected: %luµs (expected %luµs ±%luµs) - starting data collection", 
                         timing, timings->preamble_space, timings->preamble_tolerance);
                return InfraredDecoderStatusOk;
            }
            ESP_LOGD(TAG, "SIRC: Expected preamble space but got level=%d, timing=%luµs", level, timing);
            return InfraredDecoderStatusError;
            
        case InfraredDecoderStateData:
            // Decode data bits - SIRC uses MARK timing for bit values
            if (level) {
                // Mark phase - determine bit value based on mark duration
                bool bit_value;
                if (MATCH_TIMING(timing, timings->bit1_mark, timings->bit_tolerance)) {
                    bit_value = true;
                } else if (MATCH_TIMING(timing, timings->bit0_mark, timings->bit_tolerance)) {
                    bit_value = false;
                } else {
                    ESP_LOGD(TAG, "SIRC: Invalid mark timing: %luµs (bit1=%luµs±%lu, bit0=%luµs±%lu)", 
                             timing, timings->bit1_mark, timings->bit_tolerance, 
                             timings->bit0_mark, timings->bit_tolerance);
                    return InfraredDecoderStatusError;
                }
                
                // Store bit
                uint8_t byte_index = decoder->databit_cnt / 8;
                uint8_t bit_index = decoder->databit_cnt % 8;
                
                if (byte_index < sizeof(decoder->data)) {
                    if (bit_value) {
                        decoder->data[byte_index] |= (1 << bit_index);
                    }
                    decoder->databit_cnt++;
                    
                    ESP_LOGD(TAG, "SIRC: Bit %d = %d (total bits: %d)", decoder->databit_cnt - 1, bit_value, decoder->databit_cnt);
                    
                    // Check if we have reached the maximum possible bits
                    uint32_t max_bits = 0;
                    for (int i = 0; i < 4; i++) {
                        if (decoder->protocol->databit_len[i] > max_bits) {
                            max_bits = decoder->protocol->databit_len[i];
                        }
                    }
                    
                    if (decoder->databit_cnt >= max_bits) {
                        ESP_LOGD(TAG, "SIRC: Collected maximum %d bits, ready for interpretation", decoder->databit_cnt);
                        return InfraredDecoderStatusReady;
                    }
                }
                return InfraredDecoderStatusOk;
            } else {
                // Space phase - just validate space timing but don't decode
                if (MATCH_TIMING(timing, timings->bit1_space, timings->bit_tolerance) ||
                    MATCH_TIMING(timing, timings->bit0_space, timings->bit_tolerance)) {
                    return InfraredDecoderStatusOk;
                } else {
                    ESP_LOGD(TAG, "SIRC: Invalid space timing: %luµs", timing);
                    return InfraredDecoderStatusError;
                }
            }
            break;
            
        default:
            ESP_LOGE(TAG, "SIRC: Invalid decoder state: %d", decoder->state);
            return InfraredDecoderStatusError;
    }
    
    return InfraredDecoderStatusOk;
}

// Common Manchester decoder
InfraredDecoderStatus infrared_common_decode_manchester(InfraredCommonDecoder* decoder, bool level, uint32_t timing) {
    if (!decoder || !decoder->protocol) return InfraredDecoderStatusError;
    
    const InfraredTimings* timings = &decoder->protocol->timings;
    uint32_t bit_time = timings->bit1_mark;
    uint32_t tolerance = timings->bit_tolerance;
    
    bool single_timing = MATCH_TIMING(timing, bit_time, tolerance);
    bool double_timing = MATCH_TIMING(timing, 2 * bit_time, tolerance);
    
    if (!single_timing && !double_timing) {
        return InfraredDecoderStatusError;
    }
    
    // Manchester decoding logic
    if (single_timing) {
        if (decoder->switch_detect) {
            // Complete bit
            uint8_t byte_index = decoder->databit_cnt / 8;
            uint8_t bit_index = decoder->databit_cnt % 8;
            
            if (byte_index < sizeof(decoder->data)) {
                bool bit_value = decoder->protocol->manchester_start_from_space ? !level : level;
                if (bit_value) {
                    decoder->data[byte_index] |= (1 << bit_index);
                }
                decoder->databit_cnt++;
                decoder->switch_detect = false;
                
                // Check completion
                if (decoder->databit_cnt >= decoder->protocol->databit_len[0]) {
                    return InfraredDecoderStatusReady;
                }
            }
        } else {
            decoder->switch_detect = true;
        }
    } else if (double_timing) {
        // Double timing - complete bit immediately
        uint8_t byte_index = decoder->databit_cnt / 8;
        uint8_t bit_index = decoder->databit_cnt % 8;
        
        if (byte_index < sizeof(decoder->data)) {
            bool bit_value = decoder->protocol->manchester_start_from_space ? level : !level;
            if (bit_value) {
                decoder->data[byte_index] |= (1 << bit_index);
            }
            decoder->databit_cnt++;
            
            // Check completion
            if (decoder->databit_cnt >= decoder->protocol->databit_len[0]) {
                return InfraredDecoderStatusReady;
            }
        }
    }
    
    return InfraredDecoderStatusOk;
}

// NEC protocol interpreter
bool infrared_decoder_nec_interpret(InfraredCommonDecoder* decoder) {
    if (!decoder) return false;
    
    bool result = false;
    
    if (decoder->databit_cnt == 32) {
        uint8_t address = decoder->data[0];
        uint8_t address_inverse = decoder->data[1];
        uint8_t command = decoder->data[2];
        uint8_t command_inverse = decoder->data[3];
        uint8_t inverse_command_inverse = (uint8_t)~command_inverse;
        uint8_t inverse_address_inverse = (uint8_t)~address_inverse;
        
        if ((command == inverse_command_inverse) && (address == inverse_address_inverse)) {
            decoder->message.protocol = InfraredProtocolNEC;
            decoder->message.address = address;
            decoder->message.command = command;
            decoder->message.repeat = false;
            result = true;
        } else {
            decoder->message.protocol = InfraredProtocolNECext;
            decoder->message.address = decoder->data[0] | (decoder->data[1] << 8);
            decoder->message.command = decoder->data[2] | (decoder->data[3] << 8);
            decoder->message.repeat = false;
            result = true;
        }
    } else if (decoder->databit_cnt == 42) {
        uint32_t* data1 = (void*)decoder->data;
        uint16_t* data2 = (void*)(data1 + 1);
        uint16_t address = *data1 & 0x1FFF;
        uint16_t address_inverse = (*data1 >> 13) & 0x1FFF;
        uint16_t command = ((*data1 >> 26) & 0x3F) | ((*data2 & 0x3) << 6);
        uint16_t command_inverse = (*data2 >> 2) & 0xFF;
        
        if ((address == (~address_inverse & 0x1FFF)) && (command == (~command_inverse & 0xFF))) {
            decoder->message.protocol = InfraredProtocolNEC42;
            decoder->message.address = address;
            decoder->message.command = command;
            decoder->message.repeat = false;
            result = true;
        } else {
            decoder->message.protocol = InfraredProtocolNEC42ext;
            decoder->message.address = address | (address_inverse << 13);
            decoder->message.command = command | (command_inverse << 8);
            decoder->message.repeat = false;
            result = true;
        }
    }
    
    return result;
}

// Samsung32 protocol interpreter
bool infrared_decoder_samsung32_interpret(InfraredCommonDecoder* decoder) {
    if (!decoder) return false;
    
    bool result = false;
    uint8_t address1 = decoder->data[0];
    uint8_t address2 = decoder->data[1];
    uint8_t command = decoder->data[2];
    uint8_t command_inverse = decoder->data[3];
    uint8_t inverse_command_inverse = (uint8_t)~command_inverse;
    
    if ((address1 == address2) && (command == inverse_command_inverse)) {
        decoder->message.command = command;
        decoder->message.address = address1;
        decoder->message.protocol = InfraredProtocolSamsung32;
        decoder->message.repeat = false;
        result = true;
    }
    
    return result;
}

// SIRC protocol interpreter
bool infrared_decoder_sirc_interpret(InfraredCommonDecoder* decoder) {
    if (!decoder) return false;
    
    uint32_t* data = (void*)&decoder->data[0];
    uint16_t address = 0;
    uint8_t command = 0;
    InfraredProtocol protocol = InfraredProtocolUnknown;
    
    if (decoder->databit_cnt == 12) {
        address = (*data >> 7) & 0x1F;
        command = *data & 0x7F;
        protocol = InfraredProtocolSIRC;
    } else if (decoder->databit_cnt == 15) {
        address = (*data >> 7) & 0xFF;
        command = *data & 0x7F;
        protocol = InfraredProtocolSIRC15;
    } else if (decoder->databit_cnt == 20) {
        address = (*data >> 7) & 0x1FFF;
        command = *data & 0x7F;
        protocol = InfraredProtocolSIRC20;
    } else {
        return false;
    }
    
    decoder->message.protocol = protocol;
    decoder->message.address = address;
    decoder->message.command = command;
    decoder->message.repeat = false;
    
    return true;
}

// RC5 protocol interpreter
bool infrared_decoder_rc5_interpret(InfraredCommonDecoder* decoder) {
    if (!decoder) return false;
    
    // RC5 must be exactly 14 bits - reject anything else
    if (decoder->databit_cnt != 14) {
        ESP_LOGD(TAG, "RC5: Invalid bit count %d (expected 14)", decoder->databit_cnt);
        return false;
    }
    
    bool result = false;
    uint32_t* data = (void*)&decoder->data[0];
    
    ESP_LOGD(TAG, "RC5: Raw data before inversion: 0x%08lX 0x%08lX (bits=%d)", 
             decoder->data[0], decoder->data[1], decoder->databit_cnt);
    
    /* Manchester (inverse):
     *      0->1 : 1
     *      1->0 : 0
     */
    decoder->data[0] = ~decoder->data[0];
    decoder->data[1] = ~decoder->data[1];
    
    ESP_LOGD(TAG, "RC5: Raw data after inversion: 0x%08lX 0x%08lX", 
             decoder->data[0], decoder->data[1]);
    
    // MSB first
    uint8_t address = reverse((uint8_t)decoder->data[0]) & 0x1F;
    uint8_t command = (reverse((uint8_t)decoder->data[1]) >> 2) & 0x3F;
    bool start_bit1 = *data & 0x01;
    bool start_bit2 = *data & 0x02;
    bool toggle = !!(*data & 0x04);
    
    if (start_bit1 == 1) {
        InfraredProtocol protocol = start_bit2 ? InfraredProtocolRC5 : InfraredProtocolRC5X;
        InfraredDecodedMessage* message = &decoder->message;
        InfraredRc5Decoder* rc5_decoder = decoder->context;
        
        if (rc5_decoder) {
            bool* prev_toggle = &rc5_decoder->toggle;
            if ((message->address == address) && (message->command == command) &&
               (message->protocol == protocol)) {
                message->repeat = (toggle == *prev_toggle);
            } else {
                message->repeat = false;
            }
            *prev_toggle = toggle;
        } else {
            message->repeat = false;
        }
        
        message->command = command;
        message->address = address;
        message->protocol = protocol;
        result = true;
    }
    
    return result;
}

// RC6 protocol interpreter
bool infrared_decoder_rc6_interpret(InfraredCommonDecoder* decoder) {
    if (!decoder) return false;
    
    bool result = false;
    uint32_t* data = (void*)&decoder->data[0];
    
    // MSB first
    uint8_t address = reverse((uint8_t)(*data >> 5));
    uint8_t command = reverse((uint8_t)(*data >> 13));
    bool start_bit = *data & 0x01;
    bool toggle = !!(*data & 0x10);
    uint8_t mode = (*data >> 1) & 0x7;
    
    if ((start_bit == 1) && (mode == 0)) {
        InfraredDecodedMessage* message = &decoder->message;
        InfraredRc6Decoder* rc6_decoder = decoder->context;
        
        if (rc6_decoder) {
            bool* prev_toggle = &rc6_decoder->toggle;
            if ((message->address == address) && (message->command == command) &&
               (message->protocol == InfraredProtocolRC6)) {
                message->repeat = (toggle == *prev_toggle);
            } else {
                message->repeat = false;
            }
            *prev_toggle = toggle;
        } else {
            message->repeat = false;
        }
        
        message->command = command;
        message->address = address;
        message->protocol = InfraredProtocolRC6;
        result = true;
    }
    
    return result;
}

// NEC repeat decoder
InfraredDecoderStatus infrared_decoder_nec_decode_repeat(InfraredCommonDecoder* decoder) {
    if (!decoder) return InfraredDecoderStatusError;
    
    float preamble_tolerance = decoder->protocol->timings.preamble_tolerance;
    uint32_t bit_tolerance = decoder->protocol->timings.bit_tolerance;
    InfraredDecoderStatus status = InfraredDecoderStatusError;
    
    if (decoder->timings_cnt < 4) return InfraredDecoderStatusOk;
    
    if ((decoder->timings[0] > INFRARED_NEC_REPEAT_PAUSE_MIN) &&
       (decoder->timings[0] < INFRARED_NEC_REPEAT_PAUSE_MAX) &&
       MATCH_TIMING(decoder->timings[1], INFRARED_NEC_REPEAT_MARK, preamble_tolerance) &&
       MATCH_TIMING(decoder->timings[2], INFRARED_NEC_REPEAT_SPACE, preamble_tolerance) &&
       MATCH_TIMING(decoder->timings[3], decoder->protocol->timings.bit1_mark, bit_tolerance)) {
        status = InfraredDecoderStatusReady;
        decoder->timings_cnt = 0;
    } else {
        status = InfraredDecoderStatusError;
    }
    
    return status;
}

// Samsung32 repeat decoder
InfraredDecoderStatus infrared_decoder_samsung32_decode_repeat(InfraredCommonDecoder* decoder) {
    if (!decoder) return InfraredDecoderStatusError;
    
    float preamble_tolerance = decoder->protocol->timings.preamble_tolerance;
    uint32_t bit_tolerance = decoder->protocol->timings.bit_tolerance;
    InfraredDecoderStatus status = InfraredDecoderStatusError;
    
    if (decoder->timings_cnt < 6) return InfraredDecoderStatusOk;
    
    if ((decoder->timings[0] > INFRARED_SAMSUNG_REPEAT_PAUSE_MIN) &&
       (decoder->timings[0] < INFRARED_SAMSUNG_REPEAT_PAUSE_MAX) &&
       MATCH_TIMING(decoder->timings[1], INFRARED_SAMSUNG_REPEAT_MARK, preamble_tolerance) &&
       MATCH_TIMING(decoder->timings[2], INFRARED_SAMSUNG_REPEAT_SPACE, preamble_tolerance) &&
       MATCH_TIMING(decoder->timings[3], decoder->protocol->timings.bit1_mark, bit_tolerance) &&
       MATCH_TIMING(decoder->timings[4], decoder->protocol->timings.bit1_space, bit_tolerance) &&
       MATCH_TIMING(decoder->timings[5], decoder->protocol->timings.bit1_mark, bit_tolerance)) {
        status = InfraredDecoderStatusReady;
        decoder->timings_cnt = 0;
    } else {
        status = InfraredDecoderStatusError;
    }
    
    return status;
}

// RC6 special Manchester decoder (handles 4th bit double timing)
InfraredDecoderStatus infrared_decoder_rc6_decode_manchester(InfraredCommonDecoder* decoder, bool level, uint32_t timing) {
    if (!decoder || !decoder->protocol) return InfraredDecoderStatusError;
    
    InfraredDecoderStatus status = InfraredDecoderStatusError;
    uint32_t bit = decoder->protocol->timings.bit1_mark;
    uint32_t tolerance = decoder->protocol->timings.bit_tolerance;
    
    bool single_timing = MATCH_TIMING(timing, bit, tolerance);
    bool double_timing = MATCH_TIMING(timing, 2 * bit, tolerance);
    bool triple_timing = MATCH_TIMING(timing, 3 * bit, tolerance);
    
    if (decoder->databit_cnt == 4) {
        // 4th bit (toggle) lasts 2x times more
        if (single_timing ^ triple_timing) {
            ++decoder->databit_cnt;
            decoder->data[0] |= (single_timing ? !level : level) << 4;
            status = InfraredDecoderStatusOk;
        }
    } else if (decoder->databit_cnt == 5) {
        if (single_timing || triple_timing) {
            if (triple_timing) timing = bit;
            decoder->switch_detect = false;
            status = infrared_common_decode_manchester(decoder, level, timing);
        } else if (double_timing) {
            status = InfraredDecoderStatusOk;
        }
    } else {
        status = infrared_common_decode_manchester(decoder, level, timing);
    }
    
    return status;
}

// RCA protocol interpreter
bool infrared_decoder_rca_interpret(InfraredCommonDecoder* decoder) {
    if (!decoder) return false;
    
    uint32_t* data = (void*)&decoder->data;
    
    uint8_t address = (*data & 0xF);
    uint8_t command = (*data >> 4) & 0xFF;
    uint8_t address_inverse = (*data >> 12) & 0xF;
    uint8_t command_inverse = (*data >> 16) & 0xFF;
    uint8_t inverse_address_inverse = (uint8_t)~address_inverse & 0xF;
    uint8_t inverse_command_inverse = (uint8_t)~command_inverse;
    
    if ((command == inverse_command_inverse) && (address == inverse_address_inverse)) {
        decoder->message.protocol = InfraredProtocolRCA;
        decoder->message.address = address;
        decoder->message.command = command;
        decoder->message.repeat = false;
        return true;
    }
    
    return false;
}

bool infrared_decoder_pioneer_interpret(InfraredCommonDecoder* decoder) {
    if (!decoder) return false;
    
    uint32_t* data = (void*)&decoder->data[0];
    uint8_t address = 0;
    uint8_t command = 0;
    InfraredProtocol protocol = InfraredProtocolUnknown;
    
    if (decoder->databit_cnt == decoder->protocol->databit_len[0] ||
       decoder->databit_cnt == decoder->protocol->databit_len[1]) {
        address = *data & 0xFF;
        uint8_t real_address_checksum = ~address;
        uint8_t address_checksum = (*data >> 8) & 0xFF;
        command = (*data >> 16) & 0xFF;
        uint8_t real_command_checksum = ~command;
        uint8_t command_checksum = (*data >> 24) & 0xFF;
        
        if (address_checksum != real_address_checksum) {
            return false;
        }
        if (command_checksum != real_command_checksum) {
            return false;
        }
        protocol = InfraredProtocolPioneer;
    } else {
        return false;
    }
    
    decoder->message.protocol = protocol;
    decoder->message.address = address;
    decoder->message.command = command;
    decoder->message.repeat = false;
    
    return true;
}

bool infrared_decoder_kaseikyo_interpret(InfraredCommonDecoder* decoder) {
    if (!decoder) return false;
    
    bool result = false;
    uint16_t vendor_id = ((uint16_t)(decoder->data[1]) << 8) | (uint16_t)decoder->data[0];
    uint8_t vendor_parity = decoder->data[2] & 0x0f;
    uint8_t genre1 = decoder->data[2] >> 4;
    uint8_t genre2 = decoder->data[3] & 0x0f;
    uint16_t data = (uint16_t)(decoder->data[3] >> 4) | ((uint16_t)(decoder->data[4] & 0x3f) << 4);
    uint8_t id = decoder->data[4] >> 6;
    uint8_t parity = decoder->data[5];
    
    uint8_t vendor_parity_check = decoder->data[0] ^ decoder->data[1];
    vendor_parity_check = (vendor_parity_check & 0xf) ^ (vendor_parity_check >> 4);
    uint8_t parity_check = decoder->data[2] ^ decoder->data[3] ^ decoder->data[4];
    
    if (vendor_parity == vendor_parity_check && parity == parity_check) {
        decoder->message.command = (uint32_t)data;
        decoder->message.address = ((uint32_t)id << 24) | ((uint32_t)vendor_id << 8) |
                                   ((uint32_t)genre1 << 4) | (uint32_t)genre2;
        decoder->message.protocol = InfraredProtocolKaseikyo;
        decoder->message.repeat = false;
        result = true;
    }
    
    return result;
}
