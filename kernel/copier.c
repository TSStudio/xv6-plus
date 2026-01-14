#include "types.h"
#include "param.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "vm.h"

#define MAX_COPY_TASKS 64

enum copy_state { COPY_FREE = 0,
                  COPY_QUEUED,
                  COPY_RUNNING,
                  COPY_DONE };

struct copy_task {
    int id;
    struct proc* owner;
    uint64 src;
    uint64 dst;
    uint64 len;
    int result;
    enum copy_state state;
};

static struct {
    struct spinlock lock;
    struct copy_task tasks[MAX_COPY_TASKS];
    int q[MAX_COPY_TASKS];
    int qhead;
    int qtail;
    int qcount;
    int next_id;
} copier;

static int
copy_pages(struct copy_task* t) {
    struct proc* p = t->owner;
    if (p == 0 || p->pagetable == 0)
        return -1;

    acquire(&p->lock);
    if (p->state == ZOMBIE) {
        release(&p->lock);
        return -1;
    }

    uint64 src = t->src;
    uint64 dst = t->dst;
    uint64 remain = t->len;

    int res = 0;

    while (remain > 0) {
        uint64 src_pa = walkaddr(p->pagetable, src);
        uint64 dst_pa = walkaddr(p->pagetable, dst);
        if (src_pa == 0 || dst_pa == 0) {
            res = -1;
            break;
        }

        uint64 src_off = src & (PGSIZE - 1);
        uint64 dst_off = dst & (PGSIZE - 1);
        uint64 chunk = PGSIZE - src_off;
        uint64 dst_room = PGSIZE - dst_off;
        if (dst_room < chunk)
            chunk = dst_room;
        if (chunk > remain)
            chunk = remain;

        memmove((void*)(dst_pa + dst_off), (void*)(src_pa + src_off), chunk);

        src += chunk;
        dst += chunk;
        remain -= chunk;
    }

    release(&p->lock);
    return res;
}

static struct copy_task*
find_task_locked(int id) {
    for (int i = 0; i < MAX_COPY_TASKS; i++) {
        if (copier.tasks[i].state != COPY_FREE && copier.tasks[i].id == id)
            return &copier.tasks[i];
    }
    return 0;
}

static void
copier_worker(void* arg) {
    (void)arg;
    for (;;) {
        acquire(&copier.lock);
        while (copier.qcount == 0)
            sleep(&copier, &copier.lock);

        int idx = copier.q[copier.qhead];
        copier.qhead = (copier.qhead + 1) % MAX_COPY_TASKS;
        copier.qcount--;

        struct copy_task* t = &copier.tasks[idx];
        t->state = COPY_RUNNING;
        release(&copier.lock);

        int res = copy_pages(t);

        acquire(&copier.lock);
        t->result = res;
        t->state = COPY_DONE;
        wakeup(t);
        release(&copier.lock);
    }
}

void copier_init(void) {
    initlock(&copier.lock, "copier");
    for (int i = 0; i < MAX_COPY_TASKS; i++) {
        copier.tasks[i].state = COPY_FREE;
        copier.tasks[i].id = 0;
        copier.tasks[i].owner = 0;
    }
    copier.qhead = copier.qtail = copier.qcount = 0;
    copier.next_id = 1;

    if (kthread_create(copier_worker, 0, "copier") == 0)
        panic("copier kthread");
}

uint64
sys_amemcpy(void) {
    uint64 dst, src;
    int len;

    argaddr(0, &dst);
    argaddr(1, &src);
    argint(2, &len);
    if (len <= 0)
        return -1;

    struct proc* owner = myproc();

    acquire(&copier.lock);
    int slot = -1;
    while (slot == -1) {
        for (int i = 0; i < MAX_COPY_TASKS; i++) {
            if (copier.tasks[i].state == COPY_FREE) {
                slot = i;
                break;
            }
        }
        if (slot == -1)
            sleep(&copier, &copier.lock);
    }

    struct copy_task* t = &copier.tasks[slot];
    t->id = copier.next_id++;
    t->owner = owner;
    t->src = src;
    t->dst = dst;
    t->len = (uint64)len;
    t->result = 0;
    t->state = COPY_QUEUED;

    copier.q[copier.qtail] = slot;
    copier.qtail = (copier.qtail + 1) % MAX_COPY_TASKS;
    copier.qcount++;

    wakeup(&copier);
    int handle = t->id;
    release(&copier.lock);
    return handle;
}

uint64
sys_csync(void) {
    int handle;
    argint(0, &handle);

    acquire(&copier.lock);
    struct copy_task* t = find_task_locked(handle);
    if (t == 0) {
        release(&copier.lock);
        return -1;
    }

    while (t->state == COPY_QUEUED || t->state == COPY_RUNNING)
        sleep(t, &copier.lock);

    int res = t->result;
    t->state = COPY_FREE;
    t->id = 0;
    t->owner = 0;
    t->len = 0;
    wakeup(&copier);
    release(&copier.lock);
    return res;
}
