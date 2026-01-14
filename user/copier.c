#include "kernel/types.h"
#include "user/user.h"

#define BSIZE (4096 * 8)

static char src[BSIZE];
static char dst[BSIZE];

static void
cpu_work(void) {
    volatile int acc = 0;
    for (int i = 0; i < 2000000; i++)
        acc += i;
    if (acc == -1)
        printf("unreachable\n");
}

int main(void) {
    for (int i = 0; i < BSIZE; i++)
        src[i] = (char)(i & 0xff);

    printf("[copier] submitting async copy (%d bytes)\n", BSIZE);
    int handle = amemcpy(dst, src, BSIZE);
    if (handle < 0) {
        printf("[copier] amemcpy failed\n");
        exit(1);
    }

    printf("[copier] doing CPU work while copying\n");
    cpu_work();

    if (csync(handle) < 0) {
        printf("[copier] csync failed\n");
        exit(1);
    }

    for (int i = 0; i < BSIZE; i++) {
        if (dst[i] != src[i]) {
            printf("[copier] mismatch at %d\n", i);
            exit(1);
        }
    }

    printf("[copier] success\n");
    exit(0);
}
