// storage.c
#include "storage.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/// Simple 32-bit integer mix for hashing
static inline uint32_t mix32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}



/// Initialize with a small power-of-two capacity.
void storage_init(Storage *st) {
    st->capacity = 16;
    st->size     = 0;
    st->flags    = calloc(st->capacity, sizeof(uint8_t));
    st->keys     = malloc(st->capacity * sizeof(int));
    st->values   = malloc(st->capacity * sizeof(void*));
    st->val_sizes= malloc(st->capacity * sizeof(size_t));
}

/// Free all data blocks and arrays.
void storage_destroy(Storage *st) {
    for (size_t i = 0; i < st->capacity; i++) {
        if (st->flags[i] == BUCKET_OCCUPIED) {
            free(st->values[i]);
        }
    }
    free(st->flags);
    free(st->keys);
    free(st->values);
    free(st->val_sizes);
}

/// Rehash into a new table twice as large
static void storage_rehash(Storage *st) {
    size_t old_cap = st->capacity;
    uint8_t *old_flags = st->flags;
    int     *old_keys  = st->keys;
    void    **old_vals = st->values;
    size_t  *old_sz    = st->val_sizes;

    st->capacity *= 2;
    st->size = 0;
    st->flags    = calloc(st->capacity, sizeof(uint8_t));
    st->keys     = malloc(st->capacity * sizeof(int));
    st->values   = malloc(st->capacity * sizeof(void*));
    st->val_sizes= malloc(st->capacity * sizeof(size_t));

    for (size_t i = 0; i < old_cap; i++) {
        if (old_flags[i] == BUCKET_OCCUPIED) {
            storage_save(st, old_keys[i], old_vals[i], old_sz[i]);
            // storage_save makes its own copy, so free old buffer
            free(old_vals[i]);
        }
    }
    free(old_flags);
    free(old_keys);
    free(old_vals);
    free(old_sz);
}

/// Insert or update via Robin-Hood hashing
void storage_save(Storage *st, int id, const void *data, size_t size) {
    // Grow if load factor > 0.7
    if ((double)(st->size + 1) / st->capacity > 0.7) {
        storage_rehash(st);
    }

    uint32_t hash = mix32((uint32_t)id);
    size_t  mask = st->capacity - 1;
    size_t  idx  = hash & mask;
    size_t  dist = 0;

    int    cur_key;
    void  *cur_val;
    size_t cur_sz;
    uint8_t cur_flag;

    // Prepare new entry
    int    new_key = id;
    void  *new_val = malloc(size);
    memcpy(new_val, data, size);
    size_t new_sz  = size;
    uint8_t new_flag = BUCKET_OCCUPIED;

    for (;;) {
        cur_flag = st->flags[idx];
        if (cur_flag != BUCKET_OCCUPIED) {
            // Empty or deleted: place here
            st->flags[idx]     = BUCKET_OCCUPIED;
            st->keys[idx]      = new_key;
            st->values[idx]    = new_val;
            st->val_sizes[idx] = new_sz;
            st->size++;
            return;
        }

        // Compute existing entry's probe distance
        uint32_t cur_hash = mix32((uint32_t)st->keys[idx]);
        size_t  cur_dist = (idx + st->capacity - (cur_hash & mask)) & mask;

        if (cur_dist < dist) {
            // Robin-Hood swap
            cur_key   = st->keys[idx];
            cur_val   = st->values[idx];
            cur_sz    = st->val_sizes[idx];

            st->keys[idx]      = new_key;
            st->values[idx]    = new_val;
            st->val_sizes[idx] = new_sz;

            new_key   = cur_key;
            new_val   = cur_val;
            new_sz    = cur_sz;
            dist      = cur_dist;
        } else if (st->keys[idx] == new_key) {
            // Overwrite existing key
            free(st->values[idx]);
            st->values[idx]    = new_val;
            st->val_sizes[idx] = new_sz;
            return;
        }

        // Next slot
        idx = (idx + 1) & mask;
        dist++;
    }
}

/// Retrieve the data for `id` if present.
int storage_get(Storage *st, int id, void *out, size_t out_sz) {
    uint32_t hash = mix32((uint32_t)id);
    size_t  mask = st->capacity - 1;
    size_t  idx  = hash & mask;

    for (size_t dist = 0; dist < st->capacity; dist++) {
        if (st->flags[idx] == BUCKET_EMPTY) {
            return 0;  // not found
        }
        if (st->flags[idx] == BUCKET_OCCUPIED && st->keys[idx] == id) {
            if (out_sz < st->val_sizes[idx]) return 0;
            memcpy(out, st->values[idx], st->val_sizes[idx]);
            return 1;
        }
        idx = (idx + 1) & mask;
    }
    return 0;
}

/// Remove entry and mark deleted.
void storage_remove(Storage *st, int id) {
    uint32_t hash = mix32((uint32_t)id);
    size_t  mask = st->capacity - 1;
    size_t  idx  = hash & mask;

    for (size_t dist = 0; dist < st->capacity; dist++) {
        if (st->flags[idx] == BUCKET_EMPTY) {
            return;  // not found
        }
        if (st->flags[idx] == BUCKET_OCCUPIED && st->keys[idx] == id) {
            free(st->values[idx]);
            st->flags[idx] = BUCKET_DELETED;
            st->size--;
            return;
        }
        idx = (idx + 1) & mask;
    }
}

void storage_iterate(Storage *st, void (*fn)(int, const void *, size_t, void *), void *udata) {
    // Access the flags/keys/values arrays directly (they're already in storage.c)
    for (size_t i = 0; i < st->capacity; i++) {
        if (st->flags[i] == BUCKET_OCCUPIED) {
            fn(st->keys[i],
               st->values[i],
               st->val_sizes[i],
               udata);
        }
    }
}
