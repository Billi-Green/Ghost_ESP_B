/* haha glog */
#include "core/glog.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define GLOG_BUF_SIZE 512

void glog(const char *fmt, ...) {
    if (!fmt) return;

    char buf[GLOG_BUF_SIZE];

    va_list ap;
    va_start(ap, fmt);
    int written = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (written < 0) {
        return;
    }

    if (written >= (int)sizeof(buf)) {
        written = (int)sizeof(buf) - 1;
    }

    if (written == 0 || buf[written - 1] != '\n') {
        if (written < (int)sizeof(buf) - 1) {
            buf[written++] = '\n';
            buf[written] = '\0';
        } else {
            buf[sizeof(buf) - 2] = '\n';
            buf[sizeof(buf) - 1] = '\0';
            written = (int)sizeof(buf) - 1;
        }
    }

    printf("%s", buf);

    if (esp_comm_manager_is_remote_command()) {
        esp_comm_manager_send_response((const uint8_t *)buf, strlen(buf));
    }
    terminal_view_add_text(buf);
    ap_manager_add_log(buf);
}


