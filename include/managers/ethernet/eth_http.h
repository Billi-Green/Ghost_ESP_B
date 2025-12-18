#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int eth_http_get_simple(const char *url, char *out, size_t out_len, int timeout_ms);

#ifdef __cplusplus
}
#endif
