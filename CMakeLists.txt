cmake_minimum_required(VERSION 3.30)
project(RAMForge C)

set(CMAKE_C_STANDARD 11)

add_executable(RAMForge src/main.c src/http_server.c src/http_server.h src/router.c src/router.h src/storage.c src/storage.h src/ramforge.c src/ramforge.h src/request.h src/response.h src/user.h src/request.c src/response.c src/cluster.c src/cluster.h src/app_routes.c src/app_routes.h src/object_pool.c src/object_pool.h src/persistence.c src/persistence.h src/app.h src/slab_alloc.c src/slab_alloc.h src/aof_batch.c src/aof_batch.h src/globals.c src/fast_json.h src/app.c src/crc32c.c src/crc32c.h tests/crc32c_test.c tests/aof_roundtrip.c tests/rdb_corrupt.c tests/aof_multi_fork.c)
