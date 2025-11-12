# 实验文档：在 xv6 中实现 kthread 和 khugepaged

## Linux kthread

在 Linux 中，`kthread` 是一种特殊的 task，它完全运行在内核空间，没有用户空间地址（即没有 `mm_struct`）。它们由内核创建（通过 `kthread_create()` / `kthread_run()`），并用于执行后台管理任务。

**主要特点：**

- **内核态运行：** 它们只在 CPU 的内核态执行，不会切换到用户态。
- **独立调度：** 它们像普通 task 一样参与内核的调度器调度。
- **用途：** 用于执行异步或定期的内核任务，例如：
  - `ksoftirqd`：处理软中断。
  - `kswapd`：管理内存页面换出。
  - `flush-X:X`：将脏页回写到磁盘。
  - `khugepaged`：管理透明巨页。

## Linux khugepaged

`khugepaged` 是一个特定的 `kthread`。它的核心任务是实现**透明巨页（Transparent Huge Pages, THP）**。

THP 旨在通过使用更大的内存页面（例如 2MB 或 1GB，而不是标准的 4KB）来减少 TLB (Translation Lookaside Buffer) Miss，从而提高具有大内存占用应用的性能。

**`khugepaged` 的工作流程：**

1. **后台扫描：** `khugepaged` 定期在后台运行。
2. **寻找候选：** 它会扫描进程的虚拟内存区域（VMA），寻找适合合并的内存。
3. **合并（Collapse）：** 当它发现一个 2MB 对齐的虚拟地址范围内，存在 512 个连续的、可合并的 4KB 物理页面时，它会尝试将它们 "合并" 成一个 2MB 的巨页。
4. **页面迁移：** 这个过程通常涉及分配一个新的 2MB 连续物理页面，将 512 个 4KB 页面的数据拷贝过去，然后更新进程的页表，最后释放掉旧的 4KB 页面。

## 实验方案

在 xv6 中，`struct proc` 结构体同时管理着内核栈、用户页表、trapframe 等。要实现 `kthread`，我们的目标是创建一个只运行内核代码、没有用户页表的 `proc`。

修改 `proc.h` 中的 `struct proc`，增加一个标志位来区分内核线程和普通进程：

```c
// proc.h
struct proc {
  // ... existing fields ...
  int is_kthread;           // 标志位：1 表示为内核线程，0 表示为普通进程
  void (*kthread_func)(void *); // 内核线程的入口函数
  void *kthread_arg;        // 内核线程的参数
  // ...
};
```

我们需要一个新的函数 `kthread_create`，用于创建内核线程。

```c
// proc.c

// (声明在 defs.h)
struct proc* kthread_create(void (*func)(void *), void *arg, char *name);

struct proc* kthread_create(void (*func)(void *), void *arg, char *name)
{
  struct proc *p;

  // 1. 分配一个 proc 结构体
  p = allocproc();
  if(p == 0)
    return 0;

  // 2. 设置为内核线程
  p->is_kthread = 1;
  p->kthread_func = func;
  p->kthread_arg = arg;
  safestrcpy(p->name, name, sizeof(p->name));

  // 3. 内核线程没有用户页表
  // allocproc() 默认创建了内核页表 (p->pagetable = proc_pagetable(p))
  // 我们不需要用户映射，所以 p->sz 保持为 0
  p->sz = 0; 
  // 也不需要 p->pagetable = proc_pagetable(p) 之后的用户空间设置

  // 4. 设置内核栈和上下文
  // kstack 已经在 allocproc() 中分配
  // 我们需要伪造一个 trapframe，以便 "返回" 到内核函数
  
  // 清空 trapframe
  memset(p->trapframe, 0, sizeof(struct trapframe));

  // 5. 设置执行入口 (epc)
  // 当 kthread 第一次被调度时，swtch() 会返回到 kthread_entry
  // 我们设置一个 "引导" 函数 kthread_entry，由它来调用真正的线程函数
  
  // swtch() 返回时会执行 ra (return address)
  // 我们将 ra 设置为 kthread_entry
  p->context.ra = (uint64)kthread_entry;
  
  // swtch() 会加载 kstack 作为栈顶
  p->context.sp = p->kstack + PGSIZE;

  // 6. 设置为 RUNNABLE 状态，等待调度器调度
  acquire(&p->lock);
  p->state = RUNNABLE;
  release(&p->lock);

  return p;
}

// 内核线程的引导入口
void
kthread_entry()
{
  struct proc *p = myproc();

  // 释放 proc.c:allocproc() 中持有的 p->lock
  // 否则 kthread_func 尝试 sleep 或 exit 时会死锁
  release(&p->lock);

  // 执行真正的内核线程函数
  if(p->kthread_func) {
    p->kthread_func(p->kthread_arg);
  }

  // 线程函数返回后，自动退出
  // 注意：kthread 的退出需要修改 exit()
  exit(0);
}
```

**修改 `fork()` 和 `exec()`**

- `fork()`：内核线程不应该被 `fork`。在 `fork()` 开头添加检查：`if(myproc()->is_kthread) panic("kthread fork");`
- `exec()`：内核线程不能执行用户程序。在 `exec()` 开头添加检查：`if(myproc()->is_kthread) panic("kthread exec");`

**修改 `exit()`**

`exit()` 需要跳过用户空间资源的回收。

```c
// proc.c
void
exit(int status)
{
  struct proc *p = myproc();
  // ...
  
  if(p->is_kthread == 0) {
    // 只有普通进程才需要回收用户页表和内存
    reparent(p);
    // ...
    uvmunmap(p->pagetable, 0, p->sz / PGSIZE, 1);
    // ...
    freevm(p->pagetable);
  }
  
  // ... (kthread 和普通进程都需要的部分)
  acquire(&p->lock);
  p->state = ZOMBIE;
  // ...
}
```

**修改 `usertrap()`**

内核线程永远不应该进入 `usertrap()`（因为它永远不在用户态）。如果进入了，说明发生了严重错误。

```c
// trap.c
void
usertrap(void)
{
  struct proc *p = myproc();
  if(p->is_kthread)
    panic("kthread in usertrap");
  // ...
}
```

在实现 `khugepaged` 守护进程之前，xv6 必须首先支持巨页。

**1. 修改物理内存分配器 (Buddy Allocator)**

- xv6 默认的 `kalloc.c` 是一个简单的、基于链表的 4KB 页面分配器。它不管理物理地址的连续性。
- **方案：** 必须将其替换为**伙伴分配器 (Buddy Allocator)**。
- 此分配器需要能够管理不同 order 的块（例如 $2^0 \times 4\text{KB}, 2^1 \times 4\text{KB}, \dots$）。
- 为了支持 2MB 巨页（$512 \times 4\text{KB}$），伙伴分配器需要能分配 $2^9 \times 4\text{KB}$ 的连续物理内存块。
- 需要新的 `kalloc_huge()`（分配 2MB）和 `kfree_huge()`（释放 2MB）。

**2. 修改 VM 系统 (支持 2MB PTE)**

- RISC-V Sv39 页表中，L2 (Level 2) PTE 如果设置了 R/W/X 位，它就不是一个指向 L1 页表的指针，而是一个 2MB 巨页的叶子 PTE。
- **修改 `walk()` (vm.c)：**
  - `walk()` 函数在查找 PTE 时，必须检查 L2 PTE。
  - 如果 L2 PTE 的 `V` 位有效且 R/W/X 位 *不* 为 0，则它是一个 2MB 巨页映射，应直接返回这个 L2 PTE。
- **修改 `mappages()` (vm.c)：**
  - 需要增加一个标志，允许 `mappages()` 尝试创建 2MB 映射，而不是默认的 4KB 映射。
  - 当映射一个 2MB 对齐且大小至少为 2MB 的区域时，它应该尝试直接在 L2 页表中设置一个PTE。

我们现在可以实现 `khugepaged` 线程本身。

**1. 创建线程**

在 `main.c` 的 `main()` 函数中（或 `userinit` 中），创建 `khugepaged` 线程：

```c
// main.c
int main() {
  // ...
  userinit();
  kthread_create(khugepaged_main, NULL, "khugepaged"); // 启动守护进程
  scheduler();
  // ...
}
```

**2. `khugepaged_main` 循环**

这是 `khugepaged` 的核心逻辑。

```c
// (在新文件 kpage.c 中)
void
khugepaged_main(void *arg)
{
  while(1) {
    // 1. 扫描所有进程
    scan_all_procs_for_collapse();
    
    // 2. 休眠一段时间
    // (需要实现一个 ksleep，或者使用现有的 sleep/wakeup 机制)
    // 简单起见，可以 sleep 在一个固定的 channel 上
    acquire(&tickslock);
    sleep(&ticks, &tickslock); // 简单地每 tick 唤醒一次，或者更长时间
    release(&tickslock);
  }
}
```

**3. 扫描与合并逻辑 ( `scan_all_procs_for_collapse` )**

这是最复杂的部分。

```c
void
scan_all_procs_for_collapse()
{
  struct proc *p;

  // 遍历进程表 (在 proc.c 中)
  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state != RUNNABLE && p->state != RUNNING && p->state != SLEEPING) {
      release(&p->lock);
      continue;
    }
    
    if(p->is_kthread) { // 不扫描内核线程
      release(&p->lock);
      continue;
    }

    // 扫描此进程的用户页表
    khugepaged_scan_pagetable(p);
    
    release(&p->lock);
  }
}

void
khugepaged_scan_pagetable(struct proc *p)
{
  pagetable_t pagetable = p->pagetable;
  uint64 va;

  // 遍历 L2 页表
  // (L2 索引范围 0-511)
  for (int l2_idx = 0; l2_idx < 512; l2_idx++) {
    pte_t *l2_pte = &pagetable[l2_idx];
    va = (uint64)l2_idx << 21; // 2MB 对齐的虚拟地址

    if(va >= p->sz) // 超出用户内存范围
      break;

    // 检查 L2 PTE 是否指向 L1 页表 (即 4KB 页面)
    if((*l2_pte & PTE_V) && !(*l2_pte & (PTE_R | PTE_W | PTE_X))) 
    {
      // 这是一个指向 L1 表的指针，是 4KB 页面的候选区
      pagetable_t l1_table = (pagetable_t)PTE2PA(*l2_pte);
      
      // 尝试合并
      try_collapse_pages(p, l2_pte, l1_table, va);
    }
    // else: 已经是巨页，或者未映射，跳过
  }
}

void
try_collapse_pages(struct proc *p, pte_t *l2_pte, pagetable_t l1_table, uint64 va)
{
  // 1. 检查 L1 表中的 512 个 4KB 页面是否都已映射
  for(int l1_idx = 0; l1_idx < 512; l1_idx++) {
    pte_t *l1_pte = &l1_table[l1_idx];
    if(!(*l1_pte & PTE_V)) {
      // 存在未映射的 4KB 页面，无法合并
      return; 
    }
  }

  // 2. (Linux 在此会检查物理页面是否连续，但 xv6 很难做到)
  // (Linux 还会检查 VMA 属性，xv6 没有 VMA，我们简化)
  
  // 3. 分配一个新的 2MB 巨页
  char *new_page = kalloc_huge(); // 假设返回 2MB 连续物理内存
  if(new_page == 0) {
    // 物理内存不足
    return;
  }

  // 4. 拷贝数据 (关键步骤)
  // 必须保证在拷贝时，用户进程不能修改这些页面
  // (简化的 xv6 缺少锁，这里存在竞态条件)
  // (一个粗暴的方案是暂停该进程，但这在 xv6 中不易实现)
  // (我们假设 p->lock 足以阻止页表修改，但不能阻止用户写入)
  
  for(int i = 0; i < 512; i++) {
    char *old_page = (char*)PTE2PA(l1_table[i]);
    memmove(new_page + (i * PGSIZE), old_page, PGSIZE);
  }
  
  // 5. 更新 L2 PTE
  // (获取 pagetable_lock)
  uint64 pa = (uint64)new_page;
  uint flags = (PTE_FLAGS(*l2_pte) | PTE_R | PTE_W | PTE_X | PTE_U | PTE_V) & ~PTE_V; 
  // (继承 L2 原有的 V 之外的标志，并设为 R/W/X/U/V 叶子)
  
  *l2_pte = PA2PTE(pa) | flags;

  // 6. 刷新 TLB
  // 在 RISC-V 上，修改页表后需要 sfence.vma
  // (需要一个函数来执行 sfence.vma)
  sfence_vma_addr(va); // 假设我们有这个函数
  
  // 7. 释放旧的 512 个 4KB 页面和 L1 页表
  for(int i = 0; i < 512; i++) {
    kfree((void*)PTE2PA(l1_table[i]));
  }
  kfree((void*)l1_table);
  
  // (释放 pagetable_lock)
  printf("khugepaged: collapsed 2M page at va 0x%x\n", va);
}
```