// response.c
#include "response.h"
#include <stdarg.h>
#include <stdio.h>

// No heap here—writes directly into the Response’s fixed buffer.
void response_json(Response* res, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(res->buffer, RESPONSE_BUFFER_SIZE, fmt, args);
    va_end(args);
}
