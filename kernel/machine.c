#include "types.h"
#include "riscv.h"

uint64
sys_halt(void) {
    w_satp(0);
    (*(volatile uint32 *)0x100000) = 0x5555;  //for qemu
    return 0;
}