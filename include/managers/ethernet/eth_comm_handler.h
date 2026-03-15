#pragma once

#include "sdkconfig.h"

#ifdef CONFIG_WITH_ETHERNET

#ifdef __cplusplus
extern "C" {
#endif

// Register the ethernet command handler with the comms manager.
// Call once at startup (or when GhostLink connects).
// Handles incoming "ethernet" commands from a peer display device and
// streams results back via COMM_STREAM_CHANNEL_ETHERNET.
void eth_comm_handler_init(void);

// Unregister and stop any running remote scan task.
void eth_comm_handler_deinit(void);

#ifdef __cplusplus
}
#endif

#endif // CONFIG_WITH_ETHERNET
