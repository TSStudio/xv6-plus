#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"

volatile int milestone1, milestone2, milestone3;
static int test_initialized = 0;
//ACHIEVED_MILESTONE(x) amoadd 1 to counters[x]
#define ACHIEVED_MILESTONE(x)                   \
    do {                                        \
        __sync_fetch_and_add(&milestone##x, 1); \
    } while (0)
#define WAIT_FOR_HARTS_TO_JOIN(x, num_harts) \
    while (milestone##x < num_harts);

void init_slab_test() {
    milestone1 = 0;
    milestone2 = 0;
    milestone3 = 0;
    __sync_synchronize();
    test_initialized = 1;
    printf("slab test counters initialized\n");
}

void fuzz_slab_test_main() {
    int core = cpuid();
    if (core == 0) {
        printf("slab test multicore start\n");
    }
    ACHIEVED_MILESTONE(1);         //milestone 1: all harts enter slab_test
    WAIT_FOR_HARTS_TO_JOIN(1, 3);  //wait for all
    void **p = (void **)kalloc();  //allocate 4096 or 512 pointers
    for (int i = 0; i < 512; i++) {
        p[i] = kmalloc(64);
        if (!p[i]) {
            printf("allocation failed at %d\n", i);
            return;
        }
        *(uint64 *)p[i] = i + core * 100000;  //store something
    }
    for (int i = 0; i < 512; i++) {
        if (*(uint64 *)p[i] != i + core * 100000) {
            printf("data corrupted at %d: expected %d, got %ld\n", i, i + core * 100000, *(uint64 *)p[i]);
            return;
        }
    }
    ACHIEVED_MILESTONE(2);         //milestone 2: all harts allocated and verified
    WAIT_FOR_HARTS_TO_JOIN(2, 3);  //wait for all
    for (int k = 0; k < 52; k++) {
        //free 0,10,20,...
        // then free 1,11,21,...
        for (int i = 0; i < 10; i++) {
            if (i + k * 10 >= 512)
                break;
            kmfree(p[i + k * 10], 64);
        }
    }
    kfree(p);
    ACHIEVED_MILESTONE(3);         //milestone 2: all harts freed
    WAIT_FOR_HARTS_TO_JOIN(3, 3);  //wait for all
    if (core == 0) {
        printf("slab test multicore passed\n");
        print_statistics();
    }
}