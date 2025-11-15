#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"

#define MAP_HUGE 0x1

static inline uint64
level_page_size(int level)
{
    return (uint64)PGSIZE << (level * 9);
}

static pte_t *walk_to_level(pagetable_t pagetable, uint64 va, int alloc, int stop_level, int *level_reached);
static pte_t *walk_with_level(pagetable_t pagetable, uint64 va, int alloc, int *level_out);
static int split_huge_page(pagetable_t pagetable, uint64 va);
static int mappages_flags(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm, int flags);

/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[];  // kernel.ld sets this to end of kernel code.

extern char trampoline[];  // trampoline.S

// Make a direct-map page table for the kernel.
pagetable_t
kvmmake(void) {
    pagetable_t kpgtbl;

    kpgtbl = (pagetable_t)kalloc();
    memset(kpgtbl, 0, PGSIZE);

    // uart registers
    kvmmap(kpgtbl, UART0, UART0, PGSIZE, PTE_R | PTE_W);

    // virtio mmio disk interface
    kvmmap(kpgtbl, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

    // PLIC
    kvmmap(kpgtbl, PLIC, PLIC, 0x4000000, PTE_R | PTE_W);

    // map kernel text executable and read-only.
    kvmmap(kpgtbl, KERNBASE, KERNBASE, (uint64)etext - KERNBASE, PTE_R | PTE_X);

    // map kernel data and the physical RAM we'll make use of.
    kvmmap(kpgtbl, (uint64)etext, (uint64)etext, PHYSTOP - (uint64)etext, PTE_R | PTE_W);

    // map the trampoline for trap entry/exit to
    // the highest virtual address in the kernel.
    kvmmap(kpgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

    // allocate and map a kernel stack for each process.
    proc_mapstacks(kpgtbl);

    return kpgtbl;
}

// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
void kvmmap(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz, int perm) {
    if (mappages(kpgtbl, va, sz, pa, perm) != 0)
        panic("kvmmap");
}

// Initialize the kernel_pagetable, shared by all CPUs.
void kvminit(void) {
    kernel_pagetable = kvmmake();
}

// Switch the current CPU's h/w page table register to
// the kernel's page table, and enable paging.
void kvminithart() {
    // wait for any previous writes to the page table memory to finish.
    sfence_vma();

    w_satp(MAKE_SATP(kernel_pagetable));

    // flush stale entries from the TLB.
    sfence_vma();
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..29 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..11 -- 12 bits of byte offset within the page.
static pte_t *
walk_to_level(pagetable_t pagetable, uint64 va, int alloc, int stop_level, int *level_reached) {
    if (va >= MAXVA)
        panic("walk");
    if (stop_level < 0 || stop_level > 2)
        panic("walk level");

    for (int level = 2;; level--) {
        pte_t *pte = &pagetable[PX(level, va)];
        if (level == stop_level) {
            if (level_reached)
                *level_reached = level;
            return pte;
        }

        if (*pte & PTE_V) {
            if (*pte & (PTE_R | PTE_W | PTE_X)) {
                if (level_reached)
                    *level_reached = level;
                return pte;
            }
            pagetable = (pagetable_t)PTE2PA(*pte);
        } else {
            if (!alloc)
                return 0;
            pagetable_t next = (pagetable_t)kalloc();
            if (next == 0)
                return 0;
            memset(next, 0, PGSIZE);
            *pte = PA2PTE(next) | PTE_V;
            pagetable = next;
        }
    }
}

static pte_t *
walk_with_level(pagetable_t pagetable, uint64 va, int alloc, int *level_out) {
    return walk_to_level(pagetable, va, alloc, 0, level_out);
}

pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc) {
    return walk_with_level(pagetable, va, alloc, 0);
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
uint64
walkaddr(pagetable_t pagetable, uint64 va) {
    pte_t *pte;
    uint64 pa;
    int level = -1;

    if (va >= MAXVA)
        return 0;

    pte = walk_with_level(pagetable, va, 0, &level);
    if (pte == 0)
        return 0;
    if ((*pte & PTE_V) == 0)
        return 0;
    if ((*pte & PTE_U) == 0)
        return 0;
    pa = PTE2PA(*pte);
    uint64 offset = va & (level_page_size(level) - 1);
    return pa + offset;
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa.
// va and size MUST be page-aligned.
// Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
static int
mappages_flags(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm, int flags) {
    if (size == 0)
        panic("mappages: size");

    if (flags & MAP_HUGE) {
        if ((va % HUGEPGSIZE) != 0 || (pa % HUGEPGSIZE) != 0 || (size % HUGEPGSIZE) != 0)
            panic("mappages: huge align");
        uint64 a = va;
        uint64 last = va + size - HUGEPGSIZE;
        for (;;) {
            pte_t *pte = walk_to_level(pagetable, a, 1, 1, 0);
            if (pte == 0)
                return -1;
            if (*pte & PTE_V)
                panic("mappages: remap");
            *pte = PA2PTE(pa) | perm | PTE_V;
            if (a == last)
                break;
            a += HUGEPGSIZE;
            pa += HUGEPGSIZE;
        }
        return 0;
    }

    if ((va % PGSIZE) != 0)
        panic("mappages: va not aligned");
    if ((size % PGSIZE) != 0)
        panic("mappages: size not aligned");

    uint64 a = va;
    uint64 last = va + size - PGSIZE;
    for (;;) {
        pte_t *pte = walk(pagetable, a, 1);
        if (pte == 0)
            return -1;
        if (*pte & PTE_V)
            panic("mappages: remap");
        *pte = PA2PTE(pa) | perm | PTE_V;
        if (a == last)
            break;
        a += PGSIZE;
        pa += PGSIZE;
    }
    return 0;
}

int mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm) {
    return mappages_flags(pagetable, va, size, pa, perm, 0);
}

// create an empty user page table.
// returns 0 if out of memory.
pagetable_t
uvmcreate() {
    pagetable_t pagetable;
    pagetable = (pagetable_t)kalloc();
    if (pagetable == 0)
        return 0;
    memset(pagetable, 0, PGSIZE);
    return pagetable;
}

// Remove npages of mappings starting from va. va must be
// page-aligned. It's OK if the mappings don't exist.
// Optionally free the physical memory.
void uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free) {
    if ((va % PGSIZE) != 0)
        panic("uvmunmap: not aligned");

    uint64 a = va;
    uint64 end = va + npages * PGSIZE;
    while (a < end) {
        int level = -1;
        pte_t *pte = walk_with_level(pagetable, a, 0, &level);
        if (pte == 0 || (*pte & PTE_V) == 0) {
            a += PGSIZE;
            continue;
        }

        uint64 leaf_size = level_page_size(level);
        uint64 block_start = a - (a % leaf_size);
        uint64 block_end = block_start + leaf_size;

        if (level > 0 && (block_start != a || block_end > end)) {
            if (split_huge_page(pagetable, block_start) != 0)
                panic("uvmunmap split");
            continue;
        }

        if (do_free) {
            uint64 pa = PTE2PA(*pte);
            if (level == 0)
                kfree((void *)pa);
            else if (level == 1)
                kfree_huge((void *)pa);
            else
                panic("uvmunmap lvl");
        }
        *pte = 0;
        a = (level > 0) ? block_end : (a + PGSIZE);
    }
}

// Allocate PTEs and physical memory to grow a process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz, int xperm) {
    char *mem;
    uint64 a;

    if (newsz < oldsz)
        return oldsz;

    oldsz = PGROUNDUP(oldsz);
    for (a = oldsz; a < newsz; a += PGSIZE) {
        mem = kalloc();
        if (mem == 0) {
            uvmdealloc(pagetable, a, oldsz);
            return 0;
        }
        memset(mem, 0, PGSIZE);
        if (mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_R | PTE_U | xperm) != 0) {
            kfree(mem);
            uvmdealloc(pagetable, a, oldsz);
            return 0;
        }
    }
    return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz) {
    if (newsz >= oldsz)
        return oldsz;

    if (PGROUNDUP(newsz) < PGROUNDUP(oldsz)) {
        int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
        uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
    }

    return newsz;
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
void freewalk(pagetable_t pagetable) {
    // there are 2^9 = 512 PTEs in a page table.
    for (int i = 0; i < 512; i++) {
        pte_t pte = pagetable[i];
        if ((pte & PTE_V) && (pte & (PTE_R | PTE_W | PTE_X)) == 0) {
            // this PTE points to a lower-level page table.
            uint64 child = PTE2PA(pte);
            freewalk((pagetable_t)child);
            pagetable[i] = 0;
        } else if (pte & PTE_V) {
            panic("freewalk: leaf");
        }
    }
    kfree((void *)pagetable);
}

// Free user memory pages,
// then free page-table pages.
void uvmfree(pagetable_t pagetable, uint64 sz) {
    if (sz > 0)
        uvmunmap(pagetable, 0, PGROUNDUP(sz) / PGSIZE, 1);
    freewalk(pagetable);
}

static int
split_huge_page(pagetable_t pagetable, uint64 va)
{
    va -= (va % HUGEPGSIZE);
    int level = -1;
    pte_t *pte = walk_with_level(pagetable, va, 0, &level);
    if (pte == 0 || level != 1 || (*pte & PTE_V) == 0)
        return -1;

    uint64 pa = PTE2PA(*pte);
    uint64 flags = PTE_FLAGS(*pte);

    pagetable_t l0 = (pagetable_t)kalloc();
    if (l0 == 0)
        return -1;
    memset(l0, 0, PGSIZE);

    void **scratch = (void **)kalloc();
    if (scratch == 0) {
        kfree(l0);
        return -1;
    }

    int allocated = 0;
    for (; allocated < 512; allocated++) {
        void *page = kalloc();
        if (page == 0)
            goto fail;
        memmove(page, (void *)(pa + allocated * PGSIZE), PGSIZE);
        scratch[allocated] = page;
        l0[allocated] = PA2PTE((uint64)page) | flags;
    }

    *pte = PA2PTE(l0) | PTE_V;
    kfree_huge((void *)pa);
    kfree(scratch);
    sfence_vma();
    return 0;

fail:
    for (int i = 0; i < allocated; i++)
        kfree(scratch[i]);
    kfree(l0);
    kfree(scratch);
    return -1;
}

// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
int uvmcopy(pagetable_t old, pagetable_t new, uint64 sz) {
    uint64 i = 0;

    while (i < sz) {
        int level = -1;
        pte_t *pte = walk_with_level(old, i, 0, &level);
        if (pte == 0 || (*pte & PTE_V) == 0) {
            i += PGSIZE;
            continue;
        }

        uint64 pa = PTE2PA(*pte);
        uint flags = PTE_FLAGS(*pte);

        if (level == 1) {
            void *mem = kalloc_huge();
            if (mem == 0)
                goto err;
            memmove(mem, (void *)pa, HUGEPGSIZE);
            if (mappages_flags(new, i, HUGEPGSIZE, (uint64)mem, flags, MAP_HUGE) != 0) {
                kfree_huge(mem);
                goto err;
            }
            i += HUGEPGSIZE;
            continue;
        }

        void *mem = kalloc();
        if (mem == 0)
            goto err;
        memmove(mem, (char *)pa, PGSIZE);
        if (mappages(new, i, PGSIZE, (uint64)mem, flags) != 0) {
            kfree(mem);
            goto err;
        }
        i += PGSIZE;
    }
    return 0;

err:
    uvmunmap(new, 0, PGROUNDUP(i) / PGSIZE, 1);
    return -1;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
void uvmclear(pagetable_t pagetable, uint64 va) {
    pte_t *pte;

    pte = walk(pagetable, va, 0);
    if (pte == 0)
        panic("uvmclear");
    *pte &= ~PTE_U;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len) {
    uint64 n, va0, pa0;
    pte_t *pte;

    while (len > 0) {
        va0 = PGROUNDDOWN(dstva);
        if (va0 >= MAXVA)
            return -1;

        pa0 = walkaddr(pagetable, va0);
        if (pa0 == 0) {
            if ((pa0 = vmfault(pagetable, va0, 0)) == 0) {
                return -1;
            }
        }

        pte = walk(pagetable, va0, 0);
        // forbid copyout over read-only user text pages.
        if ((*pte & PTE_W) == 0)
            return -1;

        n = PGSIZE - (dstva - va0);
        if (n > len)
            n = len;
        memmove((void *)(pa0 + (dstva - va0)), src, n);

        len -= n;
        src += n;
        dstva = va0 + PGSIZE;
    }
    return 0;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
int copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len) {
    uint64 n, va0, pa0;

    while (len > 0) {
        va0 = PGROUNDDOWN(srcva);
        pa0 = walkaddr(pagetable, va0);
        if (pa0 == 0) {
            if ((pa0 = vmfault(pagetable, va0, 0)) == 0) {
                return -1;
            }
        }
        n = PGSIZE - (srcva - va0);
        if (n > len)
            n = len;
        memmove(dst, (void *)(pa0 + (srcva - va0)), n);

        len -= n;
        dst += n;
        srcva = va0 + PGSIZE;
    }
    return 0;
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max) {
    uint64 n, va0, pa0;
    int got_null = 0;

    while (got_null == 0 && max > 0) {
        va0 = PGROUNDDOWN(srcva);
        pa0 = walkaddr(pagetable, va0);
        if (pa0 == 0)
            return -1;
        n = PGSIZE - (srcva - va0);
        if (n > max)
            n = max;

        char *p = (char *)(pa0 + (srcva - va0));
        while (n > 0) {
            if (*p == '\0') {
                *dst = '\0';
                got_null = 1;
                break;
            } else {
                *dst = *p;
            }
            --n;
            --max;
            p++;
            dst++;
        }

        srcva = va0 + PGSIZE;
    }
    if (got_null) {
        return 0;
    } else {
        return -1;
    }
}

// allocate and map user memory if process is referencing a page
// that was lazily allocated in sys_sbrk().
// returns 0 if va is invalid or already mapped, or if
// out of physical memory, and physical address if successful.
uint64
vmfault(pagetable_t pagetable, uint64 va, int read) {
    uint64 mem;
    struct proc *p = myproc();

    if (va >= p->sz)
        return 0;
    va = PGROUNDDOWN(va);
    if (ismapped(pagetable, va)) {
        return 0;
    }
    mem = (uint64)kalloc();
    if (mem == 0)
        return 0;
    memset((void *)mem, 0, PGSIZE);
    if (mappages(p->pagetable, va, PGSIZE, mem, PTE_W | PTE_U | PTE_R) != 0) {
        kfree((void *)mem);
        return 0;
    }
    return mem;
}

int ismapped(pagetable_t pagetable, uint64 va) {
    pte_t *pte = walk(pagetable, va, 0);
    if (pte == 0) {
        return 0;
    }
    if (*pte & PTE_V) {
        return 1;
    }
    return 0;
}

int
vm_try_collapse(struct proc *p, uint64 va)
{
    if (p == 0 || p->pagetable == 0)
        return 0;
    if ((va % HUGEPGSIZE) != 0)
        return 0;
    if (va + HUGEPGSIZE > p->sz)
        return 0;
    if (!holding(&p->lock))
        panic("vm_try_collapse lock");

    pte_t *l1 = walk_to_level(p->pagetable, va, 0, 1, 0);
    if (l1 == 0)
        return 0;
    if ((*l1 & PTE_V) == 0)
        return 0;
    if (*l1 & (PTE_R | PTE_W | PTE_X))
        return 0;

    pagetable_t l0 = (pagetable_t)PTE2PA(*l1);
    uint64 flags = 0;
    for (int i = 0; i < 512; i++) {
        pte_t entry = l0[i];
        if ((entry & PTE_V) == 0)
            return 0;
        if ((entry & PTE_U) == 0)
            return 0;
        uint64 entry_flags = PTE_FLAGS(entry);
        if (i == 0)
            flags = entry_flags;
        else if (entry_flags != flags)
            return 0;
    }

    void *new_page = kalloc_huge();
    if (new_page == 0)
        return 0;

    for (int i = 0; i < 512; i++) {
        void *old_page = (void *)PTE2PA(l0[i]);
        memmove((char *)new_page + i * PGSIZE, old_page, PGSIZE);
    }

    for (int i = 0; i < 512; i++) {
        void *old_page = (void *)PTE2PA(l0[i]);
        kfree(old_page);
    }
    kfree((void *)l0);

    *l1 = PA2PTE((uint64)new_page) | flags;
    sfence_vma();
    return 1;
}
