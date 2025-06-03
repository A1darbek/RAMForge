#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include "../src/crc32c.h"

extern int aof_write_record(int, int, const void*, uint32_t);

int main() {
    int fd = open("rt.aof", O_CREAT|O_TRUNC|O_RDWR, 0600);
    const char body[] = "{\"id\":7,\"name\":\"neo\"}";
    aof_write_record(fd, 7, body, sizeof body);
    lseek(fd, 0, SEEK_SET);

    /* re-read exactly as loader would */
    int id; uint32_t sz, crc_file;
    read(fd,&id,4); read(fd,&sz,4);
    char buf[64]; read(fd,buf,sz); read(fd,&crc_file,4);

    uint32_t crc = crc32c(0,&id,4);
    crc = crc32c(crc,&sz,4);
    crc = crc32c(crc,buf,sz);

    if (crc != crc_file) { puts("FAIL roundtrip"); return 1; }
    puts("✓ roundtrip CRC OK"); return 0;
}

