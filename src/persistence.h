#ifndef PERSISTENCE_H
#define PERSISTENCE_H

#include "storage.h"

/// Load RDB, replay AOF, start snapshot timer & AOF batch thread
void Persistence_init(const char *rdb_path,
                      const char *aof_path,
                      Storage    *storage,
                      unsigned    snapshot_interval_sec,
                      unsigned    aof_flush_ms);

void Persistence_compact(void);
/// Flush and stop the batch AOF thread (call on shutdown)
void Persistence_shutdown(void);

#endif // PERSISTENCE_H
