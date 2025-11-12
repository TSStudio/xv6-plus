#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "vm.h"
#include "signal.h"

uint64
sys_exit(void) {
    int n;
    argint(0, &n);
    kexit(n);
    return 0;  // not reached
}

uint64
sys_getpid(void) {
    return myproc()->pid;
}

uint64
sys_fork(void) {
    return kfork();
}

uint64
sys_wait(void) {
    uint64 p;
    argaddr(0, &p);
    return kwait(p);
}

uint64
sys_sbrk(void) {
    uint64 addr;
    int t;
    int n;

    argint(0, &n);
    argint(1, &t);
    addr = myproc()->sz;

    if (t == SBRK_EAGER || n < 0) {
        if (growproc(n) < 0) {
            return -1;
        }
    } else {
        // Lazily allocate memory for this process: increase its memory
        // size but don't allocate memory. If the processes uses the
        // memory, vmfault() will allocate it.
        if (addr + n < addr)
            return -1;
        myproc()->sz += n;
    }
    return addr;
}

uint64
sys_pause(void) {
    int n;
    uint ticks0;

    argint(0, &n);
    if (n < 0)
        n = 0;
    acquire(&tickslock);
    ticks0 = ticks;
    while (ticks - ticks0 < n) {
        if (killed(myproc())) {
            release(&tickslock);
            return -1;
        }
        sleep(&ticks, &tickslock);
    }
    release(&tickslock);
    return 0;
}

uint64
sys_kill(void) {
    int pid;
    int signum;

    argint(0, &pid);
    argint(1, &signum);
    return kkill(pid, signum);
}

uint64
sys_signal(void) {
    int signum;
    uint64 handler;
    struct proc* p = myproc();

    argint(0, &signum);
    argaddr(1, &handler);

    if (signum < 0 || signum >= NSIG || signum == SIGKILL)
        return (uint64)-1;

    acquire(&p->lock);
    uint64 previous = p->signal_handlers[signum];
    p->signal_handlers[signum] = handler;
    p->signal_handlers_valid |= (1ULL << signum);
    release(&p->lock);
    // printf("sys_signal: process %d set handler for signal %d to %p (previous %p)\n",
    //p->pid, signum, (void*)handler, (void*)previous);
    return previous;
}

uint64
sys_sigreturn(void) {
    struct proc* p = myproc();
    uint64 ret;

    acquire(&p->lock);
    if (!p->sig_trapframe_valid) {
        release(&p->lock);
        return (uint64)-1;
    }

    memmove(p->trapframe, &p->sig_trapframe, sizeof(struct trapframe));
    ret = p->trapframe->a0;
    p->sig_trapframe_valid = 0;
    release(&p->lock);

    return ret;
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void) {
    uint xticks;

    acquire(&tickslock);
    xticks = ticks;
    release(&tickslock);
    return xticks;
}
