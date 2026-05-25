#ifndef MEMORY_DEBUG_H
#define MEMORY_DEBUG_H

#include <stdbool.h>
#include "esp_err.h"

void memory_debug_print_heap_summary(void);
void memory_debug_print_heap_regions(void);
bool memory_debug_check_heap_integrity(void);
void memory_debug_start_boot_trace(void);
esp_err_t memory_debug_trace_start(bool leaks_only);
esp_err_t memory_debug_trace_stop(void);
void memory_debug_trace_dump(void);

#endif // MEMORY_DEBUG_H
