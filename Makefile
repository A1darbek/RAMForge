CC=gcc
CFLAGS=-O3 -g -I./include -luv -lz -lhttp_parser -pipe -flto -march=x86-64-v3 \
          -fno-plt -fdata-sections -ffunction-sections \
          -DNDEBUG -DHTTP_SERVER_FAST \
          -Wall -Wextra -Wshadow -Wconversion -Wdouble-promotion

SRC=$(wildcard src/*.c)
OBJ=$(SRC:.c=.o)
EXEC=ramforge

$(EXEC): $(OBJ)
	$(CC) $(OBJ) -o $(EXEC) $(CFLAGS)

debug: $(EXEC)
	gdb ./$(EXEC)

clean:
	rm -f $(OBJ) $(EXEC)
TESTS := tests/crc32c_test tests/aof_roundtrip tests/rdb_corrupt tests/aof_multi_fork

# Test: crc32c_test (needs only its .c and src/crc32c.c)
tests/crc32c_test: tests/crc32c_test.c src/crc32c.c
	$(CC) -Isrc -o $@ $^

# Test: aof_roundtrip (needs aof_batch.c, storage.c, and crc32c.c)
tests/aof_roundtrip: tests/aof_roundtrip.c src/crc32c.c src/aof_batch.c src/storage.c
	$(CC) -Isrc -o $@ $^

tests/rdb_corrupt: tests/rdb_corrupt.c src/crc32c.c
	$(CC) -Isrc -o $@ $^

tests/aof_multi_fork: tests/aof_multi_fork.c src/crc32c.c src/aof_batch.c src/storage.c
	$(CC) -pthread -Isrc -o $@ $^


.PHONY: test
test: $(TESTS)
	@for t in $(TESTS); do $$t || exit 1; done
	@echo "All CRC smoke-tests passed."

.PHONY: chaos
chaos: tests/chaos/hard_kill.sh tests/chaos/disk_full.sh tests/chaos/power_loss.sh
	@set -e; for t in $^ ; do \
	    echo "=== $$t ==="; \
	    bash $$t ; \
	done
