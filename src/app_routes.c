#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "persistence.h"
#include "user.h"
#include "aof_batch.h"
#include "fast_json.h"
#include "router.h"
#include "ramforge_rotation_metrics.h"

extern App *g_app;

// ═══════════════════════════════════════════════════════════════════════════════
// Optimized Request/Response Handling
// ═══════════════════════════════════════════════════════════════════════════════

typedef struct {
    char* buffer;
    size_t len;
    size_t capacity;
} fast_response_t;

static inline void response_ensure_capacity(fast_response_t* res, size_t needed) {
    if (res->len + needed > res->capacity) {
        while (res->capacity < res->len + needed) {
            res->capacity *= 2;
        }
        res->buffer = realloc(res->buffer, res->capacity);
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Lightning-Fast Route Handlers
// ═══════════════════════════════════════════════════════════════════════════════

// POST /users → create or update a user (sub-100μs target)
int create_user_fast(Request *req, Response *res) {
    // Parse JSON using zero-copy parser
    json_value_t* root = json_parse(req->body, strlen(req->body));
    if (!root || root->type != JSON_OBJECT) {
        const char* error = "{\"error\":\"Invalid JSON\"}";
        size_t error_len = strlen(error);
        memcpy(res->buffer, error, error_len);
        res->buffer[error_len] = '\0';
        if (root) json_free(root);
        return -1;
    }

    // Extract fields using fast lookup
    json_value_t* id_field = json_get_field(root, "id");
    json_value_t* name_field = json_get_field(root, "name");

    if (!id_field || !name_field ||
        id_field->type != JSON_INT ||
        name_field->type != JSON_STRING) {
        const char* error = "{\"error\":\"Missing or invalid fields\"}";
        size_t error_len = strlen(error);
        memcpy(res->buffer, error, error_len);
        res->buffer[error_len] = '\0';
        json_free(root);
        return -1;
    }

    // Create user struct
    User u = {0}; // or memset(&u, 0, sizeof(u));
    u.id = id_field->as.i;

    // Copy name (safe bounds checking)
    size_t name_len = name_field->as.s.len;
    if (name_len >= sizeof(u.name)) name_len = sizeof(u.name) - 1;
    memcpy(u.name, name_field->as.s.ptr, name_len);
    u.name[name_len] = '\0';

    // AOF-FIRST: Persist to AOF before memory (ensures durability)
    if (AOF_append(u.id, &u, sizeof(u)) < 0) {
        const char *error = "{\"error\":\"Disk full\"}";
        size_t error_len = strlen(error);
        memcpy(res->buffer, error, error_len);
        res->buffer[error_len] = '\0';
        json_free(root);
        return -3;  // disk full -> HTTP 503
    }

    // Only after AOF success, persist to in-memory storage
    storage_save(g_app->storage, u.id, &u, sizeof(u));

    // Generate response using template (ultra-fast)
    size_t response_len = serialize_user_fast(res->buffer, u.id, u.name);
    res->buffer[response_len] = '\0';

    json_free(root);
    return 0;
}



// GET /users/:id (sub-50μs target)
int get_user_fast(Request *req, Response *res) {
    // Fast integer parsing from URL parameter
    const char* id_str = req->params[0].value;
    int id = 0;

    // Inline fast atoi (avoid library call overhead)
    const char* p = id_str;
    while (*p >= '0' && *p <= '9') {
        id = id * 10 + (*p - '0');
        p++;
    }

    User u;
    if (storage_get(g_app->storage, id, &u, sizeof(u))) {
        // Template-based serialization
        size_t len = serialize_user_fast(res->buffer, u.id, u.name);
        res->buffer[len] = '\0';
    } else {
        const char* error = "{\"error\":\"User not found\"}";
        size_t error_len = strlen(error);
        memcpy(res->buffer, error, error_len);
        res->buffer[error_len] = '\0';
    }
}

// Context for user iteration
typedef struct {
    char* buffer;
    char* pos;
    int first;
} user_array_ctx_t;

static void user_iter_callback(int id, const void* data, size_t size, void* ud) {
    (void)size;  // We know it's sizeof(User)
    user_array_ctx_t* ctx = (user_array_ctx_t*)ud;
    const User* user = (const User*)data;

    if (!ctx->first) {
        *ctx->pos++ = ',';
    } else {
        ctx->first = 0;
    }

    // Fast serialization directly into buffer
    size_t len = serialize_user_fast(ctx->pos, user->id, user->name);
    ctx->pos += len;
}

// GET /users → list all users (sub-200μs target for 1000 users)
int list_users_fast(Request *req, Response *res) {
    (void)req;

    char* p = res->buffer;
    *p++ = '[';

    user_array_ctx_t ctx = {
            .buffer = res->buffer,
            .pos = p,
            .first = 1
    };

    // Single iteration with direct serialization
    storage_iterate(g_app->storage, user_iter_callback, &ctx);

    *ctx.pos++ = ']';
    *ctx.pos = '\0';
}

// Health check optimized for monitoring tools
int health_fast(Request *req, Response *res) {
    (void)req;

    // Pre-computed response (only 8 bytes!)
    static const char health_response[] = "{\"ok\":1}";
    static const size_t health_len = sizeof(health_response) - 1;

    memcpy(res->buffer, health_response, health_len);
    res->buffer[health_len] = '\0';
}

// Admin compaction with progress tracking
int compact_handler_fast(Request *req, Response *res) {
    (void)req;

    // Start compaction in background
    Persistence_compact();

    // Immediate response (don't wait for completion)
    static const char compact_response[] = "{\"result\":\"compaction_started\",\"async\":true}";
    static const size_t compact_len = sizeof(compact_response) - 1;

    memcpy(res->buffer, compact_response, compact_len);
    res->buffer[compact_len] = '\0';
}
//int prometheus_metrics_handler(Request* req, Response* res) {
//    (void)req;
//    RAMForge_export_prometheus_metrics_buffer(res->buffer, RESPONSE_BUFFER_SIZE);
//    return 0;
//}

int prometheus_metrics_handler(Request* req, Response* res) {
    (void)req;
    strcpy(res->buffer, "OK\n");
    return 0;
}





// ═══════════════════════════════════════════════════════════════════════════════
// Framework Integration & Route Registration
// ═══════════════════════════════════════════════════════════════════════════════

// Main registration function that cluster.c expects
void register_application_routes(App *app) {
    // Set global app reference for route handlers
    g_app = app;

    // Core CRUD operations using the App's route registration methods
    app->post(app, "/users", create_user_fast);
    app->get(app, "/users/:id", get_user_fast);
    app->get(app, "/users", list_users_fast);

//    // Batch operations for high throughput
//    app->post(app, "/users/batch", create_users_batch);

    // System routes
    app->get(app, "/health", health_fast);
    app->post(app, "/admin/compact", compact_handler_fast);
    app->get(app, "/metrics", prometheus_metrics_handler);

}

// Legacy alias for backward compatibility
void register_fast_routes(App *app) {
    register_application_routes(app);
}
