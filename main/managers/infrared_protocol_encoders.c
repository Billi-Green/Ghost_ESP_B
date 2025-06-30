#include "managers/infrared_common.h"
#include "managers/infrared_timings.h"
#include <string.h>

// NEC
void infrared_encoder_nec_reset(InfraredCommonEncoder* encoder, const InfraredMessage* message) {
    infrared_common_encoder_reset(encoder);
    uint8_t address = message->address & 0xFF;
    uint8_t command = message->command & 0xFF;
    encoder->data[0] = address;
    encoder->data[1] = ~address;
    encoder->data[2] = command;
    encoder->data[3] = ~command;
    encoder->bits_to_encode = encoder->protocol->databit_len[0];
}

InfraredStatus infrared_encoder_nec_encode_repeat(InfraredCommonEncoder* encoder, uint32_t* duration, bool* level) {
    if(encoder->timings_encoded == 0) {
        *duration = INFRARED_NEC_REPEAT_MARK;
        *level = true;
        encoder->timings_encoded = 1;
        return InfraredStatusOk;
    } else if(encoder->timings_encoded == 1) {
        *duration = INFRARED_NEC_REPEAT_SPACE;
        *level = false;
        encoder->timings_encoded = 2;
        return InfraredStatusOk;
    } else {
        *duration = INFRARED_NEC_BIT1_MARK;
        *level = true;
        return InfraredStatusDone;
    }
}

// NEC Extended
void infrared_encoder_necext_reset(InfraredCommonEncoder* encoder, const InfraredMessage* message) {
    infrared_common_encoder_reset(encoder);
    uint16_t address = message->address & 0xFFFF;
    uint8_t command = message->command & 0xFF;
    encoder->data[0] = address & 0xFF;
    encoder->data[1] = (address >> 8) & 0xFF;
    encoder->data[2] = command;
    encoder->data[3] = ~command;
    encoder->bits_to_encode = encoder->protocol->databit_len[0];
}

// Kaseikyo
void infrared_encoder_kaseikyo_reset(InfraredCommonEncoder* encoder, const InfraredMessage* message) {
    infrared_common_encoder_reset(encoder);
    uint32_t addr_le = message->address;
    uint32_t cmd_le = message->command;
    uint8_t id = (addr_le >> 24) & 0x03;
    uint16_t vendor_id = (addr_le >> 8) & 0xFFFF;
    uint8_t genre1 = (addr_le >> 4) & 0x0F;
    uint8_t genre2 = addr_le & 0x0F;
    uint16_t data = cmd_le & 0x3FF;
    encoder->data[0] = vendor_id & 0xFF;
    encoder->data[1] = vendor_id >> 8;
    uint8_t vp = encoder->data[0] ^ encoder->data[1];
    vp = (vp & 0x0F) ^ (vp >> 4);
    encoder->data[2] = (vp & 0x0F) | (genre1 << 4);
    encoder->data[3] = ((data & 0x0F) << 4) | genre2;
    encoder->data[4] = (id << 6) | ((data >> 4) & 0x3F);
    encoder->data[5] = encoder->data[2] ^ encoder->data[3] ^ encoder->data[4];
    encoder->bits_to_encode = encoder->protocol->databit_len[0];
}

// Pioneer
void infrared_encoder_pioneer_reset(InfraredCommonEncoder* encoder, const InfraredMessage* message) {
    infrared_common_encoder_reset(encoder);
    uint8_t address = message->address & 0xFF;
    uint8_t command = message->command & 0xFF;
    uint32_t data = (address) | ((~address & 0xFF) << 8) | ((command) << 16) | ((~command & 0xFF) << 24);
    memcpy(encoder->data, &data, 4);
    encoder->bits_to_encode = encoder->protocol->databit_len[0];
}

InfraredStatus infrared_encoder_pioneer_encode_repeat(InfraredCommonEncoder* encoder, uint32_t* duration, bool* level) {
    return InfraredStatusDone; // Pioneer sends the whole frame again
}

// RCA
void infrared_encoder_rca_reset(InfraredCommonEncoder* encoder, const InfraredMessage* message) {
    infrared_common_encoder_reset(encoder);
    uint32_t data = (message->address & 0x0F) | ((message->command & 0xFF) << 4) | ((~message->address & 0x0F) << 12) | ((~message->command & 0xFF) << 16);
    memcpy(encoder->data, &data, 4);
    encoder->bits_to_encode = encoder->protocol->databit_len[0];
}

InfraredStatus infrared_encoder_rca_encode_repeat(InfraredCommonEncoder* encoder, uint32_t* duration, bool* level) {
    if(encoder->timings_encoded == 0) {
        *duration = INFRARED_RCA_REPEAT_MARK;
        *level = true;
        encoder->timings_encoded = 1;
        return InfraredStatusOk;
    } else {
        *duration = INFRARED_RCA_REPEAT_SPACE;
        *level = false;
        return InfraredStatusDone;
    }
}

// Samsung
void infrared_encoder_samsung_reset(InfraredCommonEncoder* encoder, const InfraredMessage* message) {
    infrared_common_encoder_reset(encoder);
    uint32_t data = (message->address & 0xFF) | ((message->address & 0xFF) << 8) | ((message->command & 0xFF) << 16) | ((~message->command & 0xFF) << 24);
    memcpy(encoder->data, &data, 4);
    encoder->bits_to_encode = encoder->protocol->databit_len[0];
}

InfraredStatus infrared_encoder_samsung_encode_repeat(InfraredCommonEncoder* encoder, uint32_t* duration, bool* level) {
    if(encoder->timings_encoded == 0) {
        *duration = INFRARED_SAMSUNG_REPEAT_MARK;
        *level = true;
        encoder->timings_encoded = 1;
        return InfraredStatusOk;
    } else {
        *duration = INFRARED_SAMSUNG_REPEAT_SPACE;
        *level = false;
        return InfraredStatusDone;
    }
}

// SIRC
void infrared_encoder_sirc_reset(InfraredCommonEncoder* encoder, const InfraredMessage* message) {
    infrared_common_encoder_reset(encoder);
    uint8_t cmd = message->command & 0x7F;
    uint32_t data = 0;
    uint8_t addr_bits = encoder->protocol->databit_len[0] - 7;
    uint32_t addr_mask = (1 << addr_bits) - 1;
    uint32_t addr = message->address & addr_mask;
    data = cmd | (addr << 7);
    size_t byte_count = (encoder->protocol->databit_len[0] + 7) / 8;
    memcpy(encoder->data, &data, byte_count);
    encoder->bits_to_encode = encoder->protocol->databit_len[0];
}

InfraredStatus infrared_encoder_sirc_encode_repeat(InfraredCommonEncoder* encoder, uint32_t* duration, bool* level) {
    // SIRC repeats by re-sending the whole frame
    return InfraredStatusDone;
}

// RC5 protocol encoder
void infrared_encoder_rc5_reset(InfraredCommonEncoder* encoder, const InfraredMessage* message) {
    infrared_common_encoder_reset(encoder);
    uint16_t word = 0;
    word |= (1 << 13); // start bit 1
    word |= (1 << 12); // start bit 2
    uint8_t addr = message->address & 0x1F;
    word |= (uint16_t)addr << 6; // address 5 bits
    uint8_t cmd = message->command & 0x3F;
    word |= cmd; // command 6 bits
    encoder->data[0] = ~(word & 0xFF);
    encoder->data[1] = ~((word >> 8) & 0xFF);
    encoder->bits_to_encode = encoder->protocol->databit_len[0];
    encoder->bits_encoded = 0;
}
InfraredStatus infrared_encoder_rc5_encode(InfraredCommonEncoder* encoder, uint32_t* duration, bool* level) {
    return infrared_common_encode_manchester(encoder, duration, level);
}

// RC6 protocol encoder
void infrared_encoder_rc6_reset(InfraredCommonEncoder* encoder, const InfraredMessage* message) {
    infrared_common_encoder_reset(encoder);
    // build RC6 mode 0 frame: start bit + mode bits + address + command
    uint32_t bits = 0;
    // start bit at bit position databit_len-1
    bits |= 1U << (encoder->protocol->databit_len[0] - 1);
    // mode bits (0) follow automatically
    uint8_t address = message->address & 0x1F;
    bits |= (uint32_t)address << (encoder->protocol->databit_len[0] - 1 - 5);
    uint8_t command = message->command & 0x3F;
    bits |= (uint32_t)command;
    // write bits into encoder data LSB first
    size_t byte_count = (encoder->protocol->databit_len[0] + 7) / 8;
    for (size_t i = 0; i < byte_count; i++) {
        encoder->data[i] = (bits >> (8 * i)) & 0xFF;
    }
    encoder->bits_to_encode = encoder->protocol->databit_len[0];
    encoder->bits_encoded = 0;
}
InfraredStatus infrared_encoder_rc6_encode_manchester(InfraredCommonEncoder* encoder, uint32_t* duration, bool* level) {
    return infrared_common_encode_manchester(encoder, duration, level);
} 