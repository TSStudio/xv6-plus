// Mutual exclusion lock.
#ifndef SPINLOCK_H
#define SPINLOCK_H
struct spinlock {
    uint locked;  // Is the lock held?

    // For debugging:
    char *name;       // Name of lock.
    struct cpu *cpu;  // The cpu holding the lock.
};

#endif