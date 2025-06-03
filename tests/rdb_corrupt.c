#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include "../src/crc32c.h"

/* minimal fake storage record */
struct pair { int id; char msg[4]; };

int main() {
    const char *file = "test.rdb";
    FILE *f = fopen(file,"wb");
    struct pair p = { 42, "hey" };

    /* write one record */
    fwrite(&p.id, 4,1,f);
    uint32_t sz = sizeof p.msg;
    fwrite(&sz,4,1,f);
    fwrite(p.msg, sz,1,f);

    /* crc footer */
    uint32_t crc = 0;
    crc = crc32c(crc,&p.id,4);
    crc = crc32c(crc,&sz,4);
    crc = crc32c(crc,p.msg,sz);
    fwrite(&crc,4,1,f);
    fclose(f);

    /* flip one byte before footer */
    int fd = open(file,O_RDWR);
    lseek(fd, 2, SEEK_SET);
    uint8_t b; read(fd,&b,1); b ^= 0xFF; lseek(fd,-1,SEEK_CUR); write(fd,&b,1);
    close(fd);

    /* invoke loader the same way persistence.c would */
    FILE *r = fopen(file,"rb");
    fseek(r,-4,SEEK_END);
    fread(&crc,4,1,r);      /* footer */
    rewind(r);

    uint32_t crc2=0; long end = ftell(r)+ftell(r); /* dummy */
    int id; uint32_t s; char buf[4];
    fread(&id,4,1,r); fread(&s,4,1,r); fread(buf,s,1,r);
    crc2 = crc32c(crc2,&id,4); crc2=crc32c(crc2,&s,4); crc2=crc32c(crc2,buf,s);

    if (crc2==crc){ puts("FAIL corruption undetected"); return 1;}
    puts("✓ corruption detected");
    return 0;
}

