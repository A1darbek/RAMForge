#ifndef AOF_BATCH_H
#define AOF_BATCH_H

#include <stddef.h>
#include "storage.h"

/// Initialize the AOF batcher:
///   path               – file to append to
///   ring_capacity      – size of the ring buffer (power of two)
///   flush_interval_ms  – group-commit interval
void AOF_init(const char *path,
              size_t ring_capacity,
              unsigned flush_interval_ms);

/// Synchronously replay the existing AOF file into `storage`.
void AOF_load(struct Storage *storage);

/// Enqueue one command (id + data blob) for batched fsync.
int AOF_append(int id, const void *data, size_t size);

/// Flush any pending entries, stop the writer thread, close the file.
void AOF_shutdown(void);

void AOF_rewrite(Storage *storage);

#endif // AOF_BATCH_H
