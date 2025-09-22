/* haha glog */
#include "core/glog.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define GLOG_BUF_SIZE 512

void glog(const char *fmt, ...) {
    if (!fmt) return;

    va_list ap;
    va_start(ap, fmt);
    va_list ap_copy;
    va_copy(ap_copy, ap);

    vprintf(fmt, ap);
    va_end(ap);

    char buf[GLOG_BUF_SIZE];
    int written = vsnprintf(buf, sizeof(buf), fmt, ap_copy);
    if (written < 0) {
        va_end(ap_copy);
        return;
    }
    buf[sizeof(buf)-1] = '\0';
    if (esp_comm_manager_is_remote_command()) {
        esp_comm_manager_send_response((const uint8_t*)buf, strlen(buf));
    }
    terminal_view_add_text(buf);
    ap_manager_add_log(buf);

    va_end(ap_copy);

}


