#pragma once

#include <stddef.h>
#include <stdint.h>

// Builds a human-readable details string from an NDEF message.
// Includes a header with card_label and UID and a summary of records.
// Returns malloc'd string which the caller must free.
char* ndef_build_details_from_message(const uint8_t* ndef_msg,
                                      size_t ndef_len,
                                      const uint8_t* uid,
                                      uint8_t uid_len,
                                      const char* card_label);

// Build human-readable details string from TLV area (entire tag memory region).
// Returns malloc'd string which the caller must free.
char* ndef_build_details_from_tlv(const uint8_t* tlv_area,
                                  size_t tlv_len,
                                  const uint8_t* uid,
                                  uint8_t uid_len,
                                  const char* card_label);
