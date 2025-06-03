// request.c
#include "request.h"
#include "slab_alloc.h"
#include <string.h>

Request parse_request(const char* body) {
    Request req;
    req.param_count = 0;
    if (body) {
        size_t len = strlen(body) + 1;
        // allocate via slab (fast, no heap fragmentation)
        char *copy = slab_alloc(len);
        memcpy(copy, body, len);
        req.body = copy;
    } else {
        req.body = NULL;
    }
    return req;
}

void free_request(Request* req) {
    if (req->body) {
        slab_free(req->body);
        req->body = NULL;
    }
}
