/* haha glog */
#include "core/glog.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define GLOG_BUF_SIZE 1024

void glog(const char *fmt, ...) {
    if (!fmt) return;

    char buf[GLOG_BUF_SIZE];
    va_list ap;
    va_start(ap, fmt);
    int written = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (written < 0) return;

    /* ensure NUL termination */
    buf[sizeof(buf)-1] = '\0';

    /* print to stdout without adding extra characters */
    printf("%s", buf);

#ifdef TERMINAL_VIEW_ADD_TEXT
    /* forward to terminal view using a simple "%s" format to avoid
       repeated formatting allocations */
    TERMINAL_VIEW_ADD_TEXT("%s", buf);
#endif
}


