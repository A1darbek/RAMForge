// slab_alloc.c
#define _POSIX_C_SOURCE 200112L
#include "slab_alloc.h"
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>

/// --- Configuration ---
/// List of size-classes (bytes); must be ascending.
static const size_t size_classes[] = {
        64, 128, 256, 512, 1024, 2048, 4096
};
#define NUM_CLASSES \
    (sizeof(size_classes)/sizeof(size_classes[0]))

/// We'll carve each slab page from a 64 KiB chunk
static size_t PAGE_SIZE;
static size_t PAGE_MASK;

/// Header prepended to each block in a slab
typedef struct slab_header {
    struct slab_header *next_free;
} slab_header;

/// Metadata at start of each slab page
typedef struct slab_page {
    struct slab_page     *next;
    int                   class_idx;
} slab_page;

/// One slab class descriptor
typedef struct {
    size_t         block_size;    // size_classes[i]
    slab_header   *free_list;     // singly-linked free blocks
    slab_page     *pages;         // list of allocated pages
} slab_class;

static slab_class classes[NUM_CLASSES];

void slab_init(void) {
    // Determine page size (multiple of OS page size).
    long ps = sysconf(_SC_PAGESIZE);
    if (ps <= 0) ps = 4096;
    PAGE_SIZE = (size_t)ps * 16;        // 64 KiB on 4 KiB pages
    PAGE_MASK = PAGE_SIZE - 1;

    // Initialize each size class
    for (int i = 0; i < NUM_CLASSES; i++) {
        classes[i].block_size = size_classes[i];
        classes[i].free_list  = NULL;
        classes[i].pages      = NULL;
    }
}

static int find_class(size_t size) {
    for (int i = 0; i < NUM_CLASSES; i++) {
        if (size <= classes[i].block_size)
            return i;
    }
    return -1;  // larger than max class
}

void *slab_alloc(size_t size) {
    int ci = find_class(size);
    if (ci < 0) {
        // fallback for large allocations
        void *p = malloc(size);
        return p;
    }
    slab_class *cl = &classes[ci];

    // Refill from a new page if empty
    if (!cl->free_list) {
        // allocate one big slab page, page-aligned
        void *mem;
        if (posix_memalign(&mem, PAGE_SIZE, PAGE_SIZE) != 0)
            return malloc(size);  // fallback on failure

        slab_page *pg = (slab_page*)mem;
        pg->next      = cl->pages;
        pg->class_idx = ci;
        cl->pages     = pg;

        // carve it into blocks
        size_t header_sz = sizeof(slab_header);
        size_t bs        = cl->block_size + header_sz;
        size_t max_blocks = (PAGE_SIZE - sizeof(slab_page)) / bs;

        char *blk = (char*)mem + sizeof(slab_page);
        for (size_t j = 0; j < max_blocks; j++) {
            slab_header *hdr = (slab_header*)blk;
            hdr->next_free   = cl->free_list;
            cl->free_list    = hdr;
            blk += bs;
        }
    }

    // Pop one block
    slab_header *h = cl->free_list;
    cl->free_list = h->next_free;
    return (void*)( (char*)h + sizeof(slab_header) );
}

void slab_free(void *ptr) {
    if (!ptr) return;
    // Check if pointer lies within a slab page
    uintptr_t up = (uintptr_t)ptr;
    slab_header *h = (slab_header*)(up - sizeof(slab_header));
    // Compute base of page via alignment
    uintptr_t page_base = up & ~PAGE_MASK;
    slab_page *pg = (slab_page*)page_base;
    // Validate that this page is one we allocated
    int ci = pg->class_idx;
    if (ci >= 0 && ci < NUM_CLASSES) {
        slab_class *cl = &classes[ci];
        h->next_free   = cl->free_list;
        cl->free_list  = h;
    } else {
        // fallback (large alloc or invalid)
        free(ptr);
    }
}

void slab_destroy(void) {
    // Free all pages
    for (int i = 0; i < NUM_CLASSES; i++) {
        slab_page *pg = classes[i].pages;
        while (pg) {
            slab_page *next = pg->next;
            free(pg);
            pg = next;
        }
        classes[i].pages     = NULL;
        classes[i].free_list = NULL;
    }
}
