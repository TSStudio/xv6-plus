#include "types.h"
#include "param.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

#ifndef KHUGEPAGED_INTERVAL
#define KHUGEPAGED_INTERVAL 50
#endif

extern struct proc proc[NPROC];
extern uint ticks;
extern struct spinlock tickslock;

static void
khugepaged_sleep_ticks(int delta)
{
    if (delta <= 0)
        return;

    acquire(&tickslock);
    uint start = ticks;
    while ((uint)(ticks - start) < (uint)delta) {
        sleep(&ticks, &tickslock);
    }
    release(&tickslock);
}

static int
is_runnable_state(enum procstate state)
{
    return state == RUNNABLE || state == RUNNING || state == SLEEPING;
}

static void
scan_proc_for_collapse(struct proc *p)
{
    if (p->pagetable == 0)
        return;
    if (p->sz < HUGEPGSIZE)
        return;

    for (uint64 va = 0; va + HUGEPGSIZE <= p->sz; va += HUGEPGSIZE) {
        if (vm_try_collapse(p, va)) {
            printf("khugepaged: collapsed pid=%d va=0x%p\n", p->pid, (void *)va);
        }
    }
}

static void
scan_all_procs_for_collapse(void)
{
    for (struct proc *p = proc; p < &proc[NPROC]; p++) {
        acquire(&p->lock);
        if (!p->is_kthread && is_runnable_state(p->state)) {
            scan_proc_for_collapse(p);
        }
        release(&p->lock);
    }
}

void
khugepaged_main(void *arg)
{
    (void)arg;

    for (;;) {
        scan_all_procs_for_collapse();
        khugepaged_sleep_ticks(KHUGEPAGED_INTERVAL);
    }
}
