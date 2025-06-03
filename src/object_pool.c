#include "object_pool.h"
#include <stdlib.h>
#include <stdio.h>

object_pool_t* object_pool_create(int capacity, object_factory_t factory, object_dtor_t dtor) {
    object_pool_t* pool = malloc(sizeof(object_pool_t));
    if (!pool) {
        fprintf(stderr, "[Pool] Allocation failed\n");
        return NULL;
    }
    pool->items = malloc(sizeof(void*) * capacity);
    if (!pool->items) {
        free(pool);
        fprintf(stderr, "[Pool] Allocation for items failed\n");
        return NULL;
    }
    pool->capacity = capacity;
    pool->count = 0;
    pool->factory = factory;
    pool->dtor = dtor;
    return pool;
}

void object_pool_destroy(object_pool_t* pool) {
    if (!pool)
        return;
    for (int i = 0; i < pool->count; i++) {
        if (pool->dtor)
            pool->dtor(pool->items[i]);
    }
    free(pool->items);
    free(pool);
}

void* object_pool_get(object_pool_t* pool) {
    if (pool->count > 0) {
        return pool->items[--pool->count];
    }
    if (pool->factory)
        return pool->factory();
    return NULL;
}

void object_pool_release(object_pool_t* pool, void* item) {
    if (pool->count >= pool->capacity) {
        int new_capacity = pool->capacity * 2;
        void** new_items = realloc(pool->items, sizeof(void*) * new_capacity);
        if (!new_items) {
            if (pool->dtor)
                pool->dtor(item);
            else
                free(item);
            return;
        }
        pool->items = new_items;
        pool->capacity = new_capacity;
    }
    pool->items[pool->count++] = item;
}
