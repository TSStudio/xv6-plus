#include "types.h"
#include "param.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

#define KTHREAD_PRINT_INTERVAL 100  // ticks between log messages

static void
kthread_sleep_ticks(int delta) {
    if (delta <= 0)
        return;

    acquire(&tickslock);
    uint start = ticks;
    while ((uint)(ticks - start) < (uint)delta) {
        sleep(&ticks, &tickslock);
    }
    release(&tickslock);
}

static void
kthread_test_worker(void* arg) {
    int iteration = 0;
    (void)arg;

    for (;;) {
        uint snapshot;
        acquire(&tickslock);
        snapshot = ticks;
        release(&tickslock);

        printf("[kthread-test] hart=%d iteration=%d ticks=%u\n", cpuid(), iteration++, snapshot);
        kthread_sleep_ticks(KTHREAD_PRINT_INTERVAL);
    }
}

void kthread_test_init(void) {
    static int initialized = 0;

    if (initialized)
        return;
    initialized = 1;

    if (kthread_create(kthread_test_worker, 0, "kthr-test") == 0) {
        printf("kthread_test_init: failed to create test thread\n");
        return;
    }

    printf("kthread_test_init: started periodic kernel thread\n");
}
