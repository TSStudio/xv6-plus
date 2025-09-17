//for size:
// 64
// 128
// 256
// 512
// 1024
// 2048
#include "types.h"
#include "spinlock.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"

#define min(a, b) ((a) < (b) ? (a) : (b))

typedef struct slab {
    struct slab *next;
    void *mem;  // slab 对象区首地址（位于若干页中）
    uint capacity;
    uint nr_free;  // 空闲对象数
    uint64 freemap;
} slab;

void slab_init(slab *s, char *mem, uint capacity) {  //max capacity: 64
    s->next = 0;
    s->mem = mem;
    s->capacity = capacity;
    s->nr_free = capacity;
    s->freemap = ~0ULL >> (64 - capacity);  // 低 capacity 位为 1
}

void *slab_alloc(slab *s, uint obj_size) {
    if (s->nr_free == 0) {
        return 0;
    }
    uint i;
    for (i = 0; i < s->capacity; i++) {
        if (s->freemap & (1ULL << i)) {
            s->freemap &= ~(1ULL << i);
            s->nr_free--;
            return (void *)s->mem + i * obj_size;
        }
    }
    return 0;  // 不应该到这里
}

void slab_free(slab *s, void *obj, uint obj_size) {
    uint i = ((char *)obj - (char *)s->mem) / obj_size;
    if (i >= s->capacity)
        return;  // 错误
    if (s->freemap & (1ULL << i))
        return;  // 重复释放，错误
    s->freemap |= (1ULL << i);
    s->nr_free++;
}

typedef struct kmem_cache {
    uint obj_size;  // 对象大小
    // void (*ctor)(void *);
    // void (*dtor)(void *);
    struct slab *partial;
    struct slab *full;
    struct slab *empty;
    struct spinlock lock;
} kmem_cache;

void kmem_cache_init(struct kmem_cache *c, uint obj_size) {
    c->obj_size = obj_size;
    // c->ctor = ctor;
    // c->dtor = dtor;
    c->partial = 0;
    c->full = 0;
    c->empty = 0;
    initlock(&c->lock, "kmem_cache");
}

void *kmem_cache_alloc(struct kmem_cache *c) {
    acquire(&c->lock);
    void *mem;
    if (c->partial) {
        mem = slab_alloc(c->partial, c->obj_size);
        if (c->partial->nr_free == 0) {
            // 移动到 full 链表
            slab *s = c->partial;
            c->partial = c->partial->next;
            s->next = c->full;
            c->full = s;
        }
    } else if (c->empty) {
        mem = slab_alloc(c->empty, c->obj_size);
        if (c->empty->nr_free == 0) {
            // 移动到 full 链表
            slab *s = c->empty;
            c->empty = c->empty->next;
            s->next = c->full;
            c->full = s;
        } else {
            // 移动到 partial 链表
            slab *s = c->empty;
            c->empty = c->empty->next;
            s->next = c->partial;
            c->partial = s;
        }
    } else {
        // 分配新 slab
        char *mem_page_for_slab_itself = kalloc();  // 一页内存
        if (!mem_page_for_slab_itself) {
            release(&c->lock);
            return 0;  // 分配失败
        }
        slab *s = (slab *)mem_page_for_slab_itself;
        char *mem_page_for_objects = kalloc();  // 另一页内存
        if (!mem_page_for_objects) {
            kfree(mem_page_for_slab_itself);
            release(&c->lock);
            return 0;  // 分配失败
        }
        slab_init(s, mem_page_for_objects, min(64, 4096 / (c->obj_size)));  // 每页64个对象
        mem = slab_alloc(s, c->obj_size);

        if (s->nr_free == 0) {
            // 移动到 full 链表
            s->next = c->full;
            c->full = s;
        } else {
            // 移动到 partial 链表
            s->next = c->partial;
            c->partial = s;
        }
    }
    release(&c->lock);
    return mem;
}

void kmem_cache_free(struct kmem_cache *c, void *obj) {
    acquire(&c->lock);
    // 找到 obj 属于哪个 slab
    slab *s;
    for (s = c->partial; s; s = s->next) {
        if (obj >= s->mem && obj < (void *)s->mem + s->capacity * c->obj_size) {
            slab_free(s, obj, c->obj_size);
            if (s->nr_free == s->capacity) {
                // 移动到 empty 链表
                // 从 partial 链表移除 s
                slab **pp;
                for (pp = &c->partial; *pp && *pp != s; pp = &(*pp)->next);
                if (*pp == s) {
                    *pp = s->next;
                    s->next = c->empty;
                    c->empty = s;
                }
            }
            break;
        }
    }
    if (!s) {
        for (s = c->full; s; s = s->next) {
            if (obj >= s->mem && obj < (void *)s->mem + s->capacity * c->obj_size) {
                slab_free(s, obj, c->obj_size);
                if (s->nr_free == 1) {
                    // 移动到 partial 链表
                    // 从 full 链表移除 s
                    slab **pp;
                    for (pp = &c->full; *pp && *pp != s; pp = &(*pp)->next);
                    if (*pp == s) {
                        *pp = s->next;
                        s->next = c->partial;
                        c->partial = s;
                    }
                } else if (s->nr_free == s->capacity) {
                    // 移动到 empty 链表
                    // 从 full 链表移除 s
                    slab **pp;
                    for (pp = &c->full; *pp && *pp != s; pp = &(*pp)->next);
                    if (*pp == s) {
                        *pp = s->next;
                        s->next = c->empty;
                        c->empty = s;
                    }
                }
                break;
            }
        }
    }
    if (!s) {
        // 错误，找不到所属 slab
        release(&c->lock);
        return;
    }
    // todo: 如果 empty 链表过长，可以考虑释放一些 slab 回收内存
    int counter = 0;  //删除第 2 个 及以后的 empty slab
    slab *pp = c->empty;
    while (pp && pp->next) {
        if (counter >= 1) {
            //printf("kmem_cache_free: freeing a slab for obj_size %d\n", c->obj_size);
            slab *tofree = pp->next;
            pp->next = tofree->next;
            kfree(tofree->mem);     // 释放对象内存页
            kfree((char *)tofree);  // 释放 slab 结构体内存页
        } else {
            counter++;
            pp = pp->next;
        }
    }
    if (pp) {
        pp->next = 0;
    }

    release(&c->lock);
}

inline uint assist_size(uint size) {
    if (size <= 64)
        return 64;
    else if (size <= 128)
        return 128;
    else if (size <= 256)
        return 256;
    else if (size <= 512)
        return 512;
    else if (size <= 1024)
        return 1024;
    else if (size <= 2048)
        return 2048;
    else
        return 0;  // too large
}

kmem_cache kmem_caches[6];  // 64,128,256,512,1024,2048

void *kmalloc(uint size) {
    uint asize = assist_size(size);
    if (asize == 0)
        return kalloc();  // too large, use page allocator
    kmem_cache *c = 0;
    for (int i = 0; i < 6; i++) {
        if (kmem_caches[i].obj_size == asize) {
            c = &kmem_caches[i];
            break;
        }
    }
    return kmem_cache_alloc(c);
}

void kmfree(void *obj, uint size) {
    uint asize = assist_size(size);
    if (asize == 0) {
        kfree(obj);  // too large, use page allocator
        return;
    }
    kmem_cache *c = 0;
    for (int i = 0; i < 6; i++) {
        if (kmem_caches[i].obj_size == asize) {
            c = &kmem_caches[i];
            break;
        }
    }
    kmem_cache_free(c, obj);
}

void init_all_slabs() {
    // 初始化各种 size 的 kmem_cache
    kmem_cache_init(&kmem_caches[0], 64);
    kmem_cache_init(&kmem_caches[1], 128);
    kmem_cache_init(&kmem_caches[2], 256);
    kmem_cache_init(&kmem_caches[3], 512);
    kmem_cache_init(&kmem_caches[4], 1024);
    kmem_cache_init(&kmem_caches[5], 2048);
    printf("slab allocator initialized\n");
}

void print_statistics() {
    for (int i = 0; i < 6; ++i) {
        kmem_cache *c = &kmem_caches[i];
        acquire(&c->lock);
        int partial_count = 0, full_count = 0, empty_count = 0;
        slab *s;
        uint total_free = 0;
        uint total_capacity = 0;
        for (s = c->partial; s; s = s->next) {
            partial_count++;
            total_free += s->nr_free;
            total_capacity += s->capacity;
        }
        for (s = c->full; s; s = s->next) {
            full_count++;
            total_capacity += s->capacity;
            total_free += s->nr_free;
        }
        for (s = c->empty; s; s = s->next) {
            empty_count++;
            total_capacity += s->capacity;
            total_free += s->nr_free;
        }
        printf("kmem_cache obj_size=%d: partial=%d, full=%d, empty=%d\n", c->obj_size, partial_count, full_count, empty_count);
        if (total_capacity > 0) {
            printf("[%d%%]total objects: %d, total free: %d\n", 100 - (total_free * 100 / total_capacity), total_capacity, total_free);
        }
        release(&c->lock);
    }
}

void slab_single_thread_test() {
    printf("single thread test start\n");
    void **p = (void **)kalloc();  //allocate 4096 or 512 pointers
    for (int i = 0; i < 512; i++) {
        p[i] = kmalloc(64);
        if (!p[i]) {
            printf("allocation failed at %d\n", i);
            return;
        }
    }
    print_statistics();
    for (int i = 0; i < 512; i++) {
        kmfree(p[i], 64);
    }
    kfree(p);
    printf("single thread test passed\n");
}

void slab_benchmark_test() {
    printf("benchmark test start\n");
    uint64 start = r_time();
    void **p = (void **)kalloc();  //allocate 4096 or 512 pointers
    for (int i = 0; i < 512; i++) {
        p[i] = kmalloc(64);
        if (!p[i]) {
            printf("allocation failed at %d\n", i);
            return;
        }
    }
    for (int i = 0; i < 512; i++) {
        kmfree(p[i], 64);
    }
    kfree(p);
    uint64 end = r_time();
    //use kalloc and kfree to do the same thing
    void **q = (void **)kalloc();  //allocate 4096 or 512 pointers
    for (int i = 0; i < 512; i++) {
        q[i] = kalloc();
        if (!q[i]) {
            printf("allocation failed at %d\n", i);
            return;
        }
    }
    for (int i = 0; i < 512; i++) {
        kfree(q[i]);
    }
    kfree(q);
    uint64 end2 = r_time();
    printf("benchmark test passed, time: %ld\n", end - start);
    printf("comparison time: %ld\n", end2 - end);
}