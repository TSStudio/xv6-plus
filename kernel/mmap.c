#include "types.h"
#include "riscv.h"
#include "defs.h"

uint64 sys_mmap(void) {
    uint64 addr;
    uint64 len;
    int prot;
    int flags;
    int fd;
    uint64 offset;

    argaddr(0, &addr);
    argaddr(1, &len);
    argint(2, &prot);
    argint(3, &flags);
    argint(4, &fd);
    argaddr(5, &offset);

    (void)addr;
    (void)len;
    (void)prot;
    (void)flags;
    (void)fd;
    (void)offset;

    return (uint64)-1;
}

uint64 sys_munmap(void) {
    uint64 addr;
    uint64 len;

    argaddr(0, &addr);
    argaddr(1, &len);

    (void)addr;
    (void)len;

    return 0;
}