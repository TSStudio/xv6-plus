#include "types.h"
#include "param.h"
#include "riscv.h"
#include "defs.h"
#include "spinlock.h"
#include "fcntl.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"
#include "stat.h"
#include "proc.h"
#include "memlayout.h"

uint64 max(uint64 a, uint64 b) {
    return a > b ? a : b;
}
uint64 min(uint64 a, uint64 b) {
    return a < b ? a : b;
}

struct {
    struct vma _vma[NVMA];
    struct vma *free_list;
    struct spinlock lock;
} vma_pool;

void vmainit(void) {
    int i;
    initlock(&vma_pool.lock, "vma_pool");
    vma_pool.free_list = 0;
    for (i = 0; i < NVMA; i++) {
        vma_pool._vma[i].next = vma_pool.free_list;
        vma_pool.free_list = &vma_pool._vma[i];
    }
}

struct vma *vmaalloc(void) {
    struct vma *v;
    acquire(&vma_pool.lock);
    v = vma_pool.free_list;
    if (v)
        vma_pool.free_list = v->next;
    else
        panic("vmaalloc: out of vmas");
    release(&vma_pool.lock);
    return v;
}

void vmafree(struct vma *v) {
    acquire(&vma_pool.lock);
    v->next = vma_pool.free_list;
    vma_pool.free_list = v;
    release(&vma_pool.lock);
}

uint64
mmap(struct proc *p, uint64 addr, uint64 len, int prot, int flags,
     struct file *f, uint64 offset) {
    struct vma *vma;

    vma = vmaalloc();
    if (vma == 0) {
        return -1;
    }

    // len must be > 0 and multiple of PGSIZE
    len = PGROUNDUP(len);

    if (addr == 0) {
        // find space for the mapping
        if (p->vma == 0) {
            vma->end = MAXVMEMMAP;
        } else {
            vma->end = p->vma->start;
        }
        vma->start = vma->end - len;
    } else {
        vma->start = addr;
        vma->end = addr + len;
    }
    vma->flags = flags;
    vma->prot = prot;
    vma->f = f;
    vma->offset = offset;
    // Record original file size at mmap time to prevent unintended file
    // extension when writing back a partial last page (mmaptest depends
    // on not extending the file beyond its original length).
    if (f->type == FD_INODE && f->ip) {
        ilock(f->ip);
        vma->fsize = f->ip->size;
        iunlock(f->ip);
    } else {
        vma->fsize = 0;
    }

    for (addr = vma->start; addr < vma->end; addr += PGSIZE) {
        if (mappages(p->pagetable, addr, PGSIZE, 0, PTE_U) != 0) {
            vmafree(vma);
            return -1;
        }
    }

    vma->next = p->vma;
    p->vma = vma;

    // increase file ref count
    filedup(f);

    return vma->start;
}

int writeback(struct file *f, off_t off, uint64 addr, int len) {
    int r, ret = 0;
    int n;

    if (f->writable == 0 || f->type != FD_INODE)
        return -1;

    int max_per_iter = ((MAXOPBLOCKS - 4) / 2) * BSIZE;
    int i = 0;

    n = len;
    while (i < n) {
        // write at most "max" bytes at a time.
        // number of bytes to write in this iteration
        int n1 = min(n - i, max_per_iter);

        begin_op();
        ilock(f->ip);
        if ((r = writei(f->ip, 1, addr + i, off, n1)) > 0)
            off += r;
        iunlock(f->ip);
        end_op();

        if (r != n1) {
            break;
        }
        i += r;
    }
    ret = (i == n ? n : -1);  // return -1 on error, else #bytes written

    return ret;
}

uint64
munmap(struct proc *p, uint64 start, uint64 end) {
    uint64 addr, l, r, flags;
    struct vma *iter, *curr, *prev = 0;
    pte_t *pte;

    for (iter = p->vma; iter;) {
        l = max(start, iter->start);
        r = min(end, iter->end);
        if (l >= r) {
            prev = iter;
            iter = iter->next;
            continue;
        }
        if (iter->flags & MAP_SHARED) {
            // need to write back dirty pages
            for (addr = l; addr < r; addr += PGSIZE) {
                pte = walk(p->pagetable, addr, 0);

                // page not present
                if (pte == 0)
                    continue;
                flags = PTE_FLAGS(*pte);
                // page not dirty
                if (!(flags & PTE_D))
                    continue;

                off_t file_off = iter->offset + addr - iter->start;
                if (file_off >= iter->fsize)
                    continue;  // entire page beyond original EOF

                if (file_off + PGSIZE <= iter->fsize) {
                    if (writeback(iter->f, file_off, addr, PGSIZE) < 0)
                        return -1;
                } else {
                    int nbytes = iter->fsize - file_off;
                    if (nbytes > 0) {
                        if (writeback(iter->f, file_off, addr, nbytes) < 0)
                            return -1;
                    }
                }
            }
        }
        uvmunmap(p->pagetable, l, (r - l) / PGSIZE, 1);
        if (l == iter->start && r == iter->end) {
            fileclose(iter->f);
            if (prev) {
                prev->next = iter->next;
            } else {
                p->vma = iter->next;
            }
            curr = iter;
            iter = iter->next;
            vmafree(curr);
        } else {
            if (l == iter->start) {
                iter->offset += r - l;
                iter->start = r;
            } else if (r == iter->end) {
                iter->end = l;
            } else {
                panic("munmap: assertion failed");
            }
            prev = iter;
            iter = iter->next;
        }
    }

    return 0;
}

uint64 sys_mmap(void) {
    uint64 len;
    int prot;
    int flags;
    struct file *f;
    struct proc *p = myproc();

    argaddr(1, &len);
    argint(2, &prot);
    argint(3, &flags);

    if (argfd(4, 0, &f) < 0) {
        return -1;
    }

    // flags must have either MAP_PRIVATE or MAP_SHARED
    if (flags == 0) {
        return -1;
    }

    // flags must have either MAP_PRIVATE or MAP_SHARED
    if ((flags & MAP_PRIVATE) && (flags & MAP_SHARED)) {
        return -1;
    }

    // check file type and length
    if (f->type != FD_INODE || f->ip->type != T_FILE || len == 0) {
        return -1;
    }

    // check prot against file mode
    if (((prot & PROT_READ) && !f->readable) ||
        ((prot & PROT_WRITE) && !f->writable && !(flags & MAP_PRIVATE))) {
        return -1;
    }

    return mmap(p, 0, len, prot, flags, f, 0);
}

uint64 sys_munmap(void) {
    uint64 addr;
    uint64 len;
    struct proc *p = myproc();
    uint64 start, end;

    argaddr(0, &addr);
    argaddr(1, &len);

    if (addr % PGSIZE != 0) {
        return -1;
    }
    start = addr;
    end = PGROUNDUP(addr + len);

    return munmap(p, start, end);
}

int mmap_page(struct vma *vma, uint64 addr, pte_t *pte) {
    char *mem;
    struct file *f;
    off_t offset;
    uint64 pte_flags;
    int rc;

    mem = kalloc();
    if (mem == 0) {
        return -1;
    }

    // read page content from file
    f = vma->f;
    offset = vma->offset + (addr - vma->start);
    ilock(f->ip);
    if ((rc = readi(f->ip, 0, (uint64)mem, offset, PGSIZE)) < 0) {
        iunlock(f->ip);
        kfree(mem);
        return -2;
    }
    iunlock(f->ip);

    if (rc < PGSIZE) {
        memset(mem + rc, 0, PGSIZE - rc);
    }

    pte_flags = PTE_FLAGS(*pte);
    pte_flags |= (vma->prot & (PROT_READ | PROT_WRITE | PROT_EXEC)) << 1;
    *pte = PA2PTE((uint64)mem) | pte_flags;

    return 0;
}