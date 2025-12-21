#ifndef OUIS_H
#define OUIS_H

#include <esp_http_server.h>
#include <stdbool.h>
#include <stddef.h>

// lookup vendor by mac address (any format with hex digits)
// returns true if found and fills out_vendor, false otherwise
bool ouis_lookup_vendor(const char *mac, char *out_vendor, size_t out_sz);

// register http endpoints for oui lookup (/oui GET/POST)
void ouis_register_handlers(httpd_handle_t handle);

#endif // OUIS_H
