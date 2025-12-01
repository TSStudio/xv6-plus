// Physical memory allocator built on a buddy system so that
// both 4KB pages and 2MB huge pages can be serviced.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

extern char end[];  // first address after kernel, defined by kernel.ld.

#define MIN_ORDER 0
#define MAX_ORDER 15
#define HUGE_ORDER HUGEPGORDER
#define NPAGES ((PHYSTOP - KERNBASE) / PGSIZE)
#define PAGE_STATE_ALLOC -1

struct run {
    struct run* next;
    struct run* prev;
    int order;
};

static struct {
    struct spinlock lock;
    struct run* freelist[MAX_ORDER + 1];
    uint64 base;
    uint64 limit;
} buddy;

static signed char page_state[NPAGES];

static inline uint64
order_size(int order) {
    return (uint64)PGSIZE << order;
}

static inline int
pa_to_index(uint64 pa) {
    if (pa < buddy.base || pa >= buddy.limit)
        panic("pa_to_index");
    return (pa - buddy.base) / PGSIZE;
}

static void list_insert(int order, struct run* r);
static void list_remove(int order, struct run* r);
static void buddy_free_locked(uint64 pa, int order);
static void* buddy_alloc_locked(int order);
static void buddy_add_range(uint64 start, uint64 end);

void kinit() {
    initlock(&buddy.lock, "kmem");
    buddy.base = KERNBASE;
    buddy.limit = PHYSTOP;
    for (int i = 0; i <= MAX_ORDER; i++)
        buddy.freelist[i] = 0;
    for (int i = 0; i < NPAGES; i++)
        page_state[i] = PAGE_STATE_ALLOC;

    uint64 managed_start = PGROUNDUP((uint64)end);
    buddy_add_range(managed_start, buddy.limit);
}

static void
buddy_add_range(uint64 start, uint64 end) {
    if (start >= end)
        return;

    acquire(&buddy.lock);
    start = PGROUNDUP(start);
    end = PGROUNDDOWN(end);
    while (start + PGSIZE <= end) {
        int order = MAX_ORDER;
        while (order > MIN_ORDER) {
            uint64 sz = order_size(order);
            if ((start % sz) == 0 && start + sz <= end)
                break;
            order--;
        }
        uint64 sz = order_size(order);
        buddy_free_locked(start, order);
        start += sz;
    }
    release(&buddy.lock);
}

static void
list_insert(int order, struct run* r) {
    r->order = order;
    r->prev = 0;
    r->next = buddy.freelist[order];
    if (r->next)
        r->next->prev = r;
    buddy.freelist[order] = r;
}

static void
list_remove(int order, struct run* r) {
    if (r->prev)
        r->prev->next = r->next;
    else if (buddy.freelist[order] == r)
        buddy.freelist[order] = r->next;
    if (r->next)
        r->next->prev = r->prev;
    r->next = r->prev = 0;
}

static void
buddy_free_locked(uint64 pa, int order) {
    if (order < MIN_ORDER || order > MAX_ORDER)
        panic("buddy_free_locked order");
    if (pa % order_size(order))
        panic("buddy_free_locked align");

    int idx = pa_to_index(pa);
    if (page_state[idx] != PAGE_STATE_ALLOC)
        panic("buddy double free");
    while (order < MAX_ORDER) {
        uint64 offset = pa - buddy.base;
        uint64 buddy_off = offset ^ order_size(order);
        uint64 buddy_pa = buddy.base + buddy_off;
        if (buddy_pa < buddy.base || buddy_pa + order_size(order) > buddy.limit)
            break;
        int buddy_idx = pa_to_index(buddy_pa);
        if (page_state[buddy_idx] != order)
            break;
        struct run* br = (struct run*)buddy_pa;
        list_remove(order, br);
        page_state[buddy_idx] = PAGE_STATE_ALLOC;
        if (buddy_pa < pa)
            pa = buddy_pa;
        idx = pa_to_index(pa);
        order++;
    }

    struct run* r = (struct run*)pa;
    list_insert(order, r);
    page_state[idx] = order;
}

static void*
buddy_alloc_locked(int order) {
    if (order < MIN_ORDER || order > MAX_ORDER)
        panic("buddy_alloc_locked order");

    for (int current = order; current <= MAX_ORDER; current++) {
        struct run* r = buddy.freelist[current];
        if (r == 0)
            continue;
        list_remove(current, r);
        page_state[pa_to_index((uint64)r)] = PAGE_STATE_ALLOC;
        while (current > order) {
            current--;
            uint64 pa = (uint64)r + order_size(current);
            struct run* split = (struct run*)pa;
            list_insert(current, split);
            page_state[pa_to_index(pa)] = current;
        }
        memset((void*)r, 5, order_size(order));
        return (void*)r;
    }
    return 0;
}

void kfree(void* pa) {
    uint64 addr = (uint64)pa;
    if (addr % PGSIZE || addr < (uint64)end || addr >= buddy.limit)
        panic("kfree");
    memset(pa, 1, PGSIZE);
    acquire(&buddy.lock);
    buddy_free_locked(addr, MIN_ORDER);
    release(&buddy.lock);
}

void* kalloc(void) {
    acquire(&buddy.lock);
    void* result = buddy_alloc_locked(MIN_ORDER);
    release(&buddy.lock);
    return result;
}

void* kalloc_huge(void) {
    acquire(&buddy.lock);
    void* result = buddy_alloc_locked(HUGE_ORDER);
    release(&buddy.lock);
    return result;
}

void kfree_huge(void* pa) {
    uint64 addr = (uint64)pa;
    if (addr % HUGEPGSIZE || addr < buddy.base || addr + HUGEPGSIZE > buddy.limit)
        panic("kfree_huge");
    memset(pa, 1, HUGEPGSIZE);
    acquire(&buddy.lock);
    buddy_free_locked(addr, HUGE_ORDER);
    release(&buddy.lock);
}
