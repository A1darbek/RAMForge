#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../src/crc32c.h"


static void assert_crc(const char *msg, uint32_t expect)
{
    uint32_t c = crc32c(0, msg, strlen(msg));
    if (c != expect) {
        printf("FAIL %s → %#x (expected %#x)\n", msg, c, expect);
        exit(1);
    }
}

int main() {
    /* known vector from RFC 3720 */
    assert_crc("123456789", 0xe3069283);
    assert_crc("hello world", 0xc99465aa);
    printf("✓ crc32c vectors OK\n");
    return 0;
}

