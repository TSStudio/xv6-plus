# Lab 1: Slab 内存分配器

在本实验中，你将为 **xv6-riscv** 内核实现一个 **slab 内存分配器**，用于高效地在内核中按对象粒度进行动态内存管理。完成后，你应能：

- 解释内核为何需要对象级（object-level）的动态分配（如 `struct proc`/PCB、inode、buffer head、pipe、file 等）。
- 说清 **xv6** 默认内存分配器如何工作，以及它的性能与碎片化问题。
- 设计并实现 **slab** 的核心抽象（cache/slab/object）与算法流程（分配、释放、回收、并发）。
- 用基准测试与可视化统计，验证 slab 对 **小对象** 分配的吞吐提升与碎片下降。

## 内核的内存分配器是什么？为什么需要？

**内核内存分配器**负责在内核态运行时**动态**获得和释放内存对象。与用户态不同，内核中的许多数据结构的**数量与生命周期不可预知**，例如：

- **进程控制块（PCB, `struct proc`）**：进程创建/退出时动态分配/回收。
- **VFS/Inode、dentry、buffer、管道、文件表项**：受 I/O 负载与缓存策略影响。
- **网络缓冲（skb）**、定时器节点、内核工作队列元素等。

这些对象通常**小于一页（4 KB）且频繁分配/释放**。因此，内核需要一个**快速、可并发、低碎片**的对象级分配器。否则会出现：

- 使用整页分配造成**严重的内部碎片**（以 4 KB 供给 64B 对象）。
- 全局锁或慢路径导致**高并发下的争用**与延迟抖动。
- 频繁初始化/清零带来**额外开销**。

## xv6 默认的内存分配器

xv6 提供的默认分配器是 **按页（page-granularity）** 的物理页分配器（见 `kalloc.c`）：

- **核心思想**：在启动时把可用物理内存切成 4 KB 页，挂到一个（或按 CPU 分片的）**空闲链表**。
  - `kalloc()`：从空闲链表取一页返回。
  - `kfree(p)`：把页 `p` 放回空闲链表。
- **并发**：使用**per-CPU freelist + spinlock**降低全局锁争用。
- **优点**：实现简单、健壮；适合页级（如页表、用户内存）分配。
- **关键缺点**：
  1. **仅支持 4 KB 粒度**，缺少通用 `kmalloc`/`kfree`。小对象只能用静态数组或自建池。
  2. 高并发小对象分配场景下，**频繁走页级慢路径**（拿锁、清零整页），**吞吐差**。

## Slab 内存分配器

### 核心概念

- **对象（object）**：特定类型内核结构（如 `struct proc`、`struct inode`）。
- **缓存（cache / kmem_cache）**：**面向“对象类型”的池**。每种对象类型有一个 cache，定义对象大小、对齐、构造器（ctor）/析构器（dtor）等。
- **slab**：由**一页或多页**组成的**对象容器**。一个 slab 中装若干等大小的对象。slab 维护对象的**分配位图或空闲链表**。

**为什么有效：**

- **对象复用**：同一 cache 里的对象生命周期相近；释放后留在 slab，下次分配**无需找页/清整页**。
- **低碎片**：对象等大装箱，减少内部碎片；按 cache 聚合提升局部性。
- **快速路径**：常在 cache 的 **partial 列表**中 O(1) 取空闲对象；可做 **每 CPU** 结构降低锁争用。
- **可扩展**：通过 `kmalloc` 封装“通用大小类”（8、16、32、…、2048、4096），覆盖杂项分配。

### 数据结构（建议）

```c
// 每种对象类型一个 cache
struct kmem_cache {
  char            name[32];
  uint            objsize;      // 对象大小（含对齐/元数据开销后）
  uint            align;        // 对齐（通常按 cacheline 对齐）
  void          (*ctor)(void *);
  void          (*dtor)(void *);
  struct slab   *partial;       // 部分可用的 slab 链
  struct slab   *full;          // 已满
  struct slab   *empty;         // 全空
  struct spinlock lock;
  // 可选：每CPU小栈/杂志（magazine）、颜色（color）等
};

struct slab {
  struct slab   *next;
  struct kmem_cache *cache;
  char          *mem;           // slab 对象区首地址（位于若干页中）
  uint           nr_objs;       // 对象总数
  uint           nr_free;       // 空闲对象数
  // 空闲对象管理：位图或空闲链（存指针/索引）
  uint8          *freemap;      // 位图（可选）
  void          **freelist;     // 单链表或栈（推荐简洁）
  // 可选：本 slab 的颜色偏移 color_off 以减少冲突
};
```

**页来源**：slab 的底层页用 **xv6 的 `kalloc()`** 获取；当 slab 变空且达到回收条件时，用 `kfree()` 归还物理页。

### 分配算法（`kmem_cache_alloc`）

1. **Fast path**：在 `cache->partial` 的头部 slab 取一个空闲对象：
   - 从 `freelist` 弹出对象指针；
   - 若 `nr_free` 变为 0，slab 从 `partial` 移到 `full`；
   - 返回对象；如设置 ctor，在首次创建该 slab 时或“对象从 free→alloc”时调用（见实现策略）。
2. **若无 partial**：
   - 尝试 `cache->empty`（首次使用的空 slab），取对象并把 slab 移到 `partial/full`；
3. **若无 empty**：
   - **向下层 `kalloc()` 申请一页或多页**，新建 slab：切分为等大对象，初始化空闲结构，放入 `partial`，重复步骤 1。
4. 并发：对 `cache` 上锁（或每-CPU 小池减少锁频率）。

### 释放算法（`kmem_cache_free`）

1. 计算对象属于哪个 slab（两种常见法）：
   - **整页对齐法**：slab 元数据放在页头或单独结构体，记录 `mem..mem+npages*PGSIZE` 范围；通过指针算出 slab。
   - **对象头法**：对象前加一个**小头部**记录所属 slab 指针（开销略增，定位简单）。
2. 把对象压回 slab 的 `freelist`，`nr_free++`。
3. 若 slab 原为 `full`，移至 `partial`；若变为**全空**，移至 `empty`。
4. **回收策略**（可选）：当 `empty` 数量或空闲页数超阈值时，将 `empty` slab 的全部页 `kfree()` 归还系统。

### kmalloc/kfree（通用大小类，建议）

在 slab 之上提供用户友好的通用接口：

- 预建一组 **size-class cache**：`{8,16,32,64,128,256,512,1024,2048}`（4 KB 直接 fall back 到 `kalloc()`）。
- `kmalloc(n)`：向上取整到最小能容纳 `n` 的 size-class，并从该 cache 分配。
- `kfree(p)`：通过对象头记录的 cache 指针或大小类索引，归还至对应 cache。

### 并发与可扩展

- 基线：**每 cache 一把自旋锁**。

## 建议实现步骤

### Milestone A：最小可用（单 cache）

1. 在 `kernel/` 新增 `slab.c/h`；复用 `defs.h` 声明接口。
2. 实现 **页→slab**：从 `kalloc()` 取一页，切成固定大小对象（先做一个 cache，例：`kmem_cache_create("test", 128, ...)`）。
3. 实现 `kmem_cache_alloc/free` 的 **partial/full/empty** 三链维护与 `freelist`。
4. 写一个测试用例：并发循环分配/释放 N 次，校验**无泄漏/无越界**。

### Milestone B：多 cache 与 kmalloc

1. 实现 `kmem_cache_create/destroy`。
2. 预建 **size-class caches**，提供 `kmalloc/kfree`。

### Milestone C：回收与统计

1. 支持把 **空 slab 归还**底层页（策略：当 `cache->empty` 超阈值或内存紧张时回收）。
2. 暴露统计接口（可通过 `procfs` 风格或 `printf`）：
   - 每 cache：`objsize、nr_slabs、nr_objs、nr_free、pages_in_use`。
   - **碎片率**：`allocated_bytes / (pages_in_use * PGSIZE)` 与 `1 - in_use_bytes / total_bytes` 两种视角皆可。
3. 压测：对比（同负载下）**xv6 kalloc 直分配整页** vs **slab** 的内存占用与吞吐（单位时间分配次数）。

## 建议 API

```c
// 建 cache（面向具体类型）
struct kmem_cache *kmem_cache_create(const char *name, uint objsize,
                                     void (*ctor)(void *), void (*dtor)(void *),
                                     uint align /*可为0表示默认*/);
void kmem_cache_destroy(struct kmem_cache *);

// 分配/释放对象
void *kmem_cache_alloc(struct kmem_cache *);
void  kmem_cache_free (struct kmem_cache *, void *obj);

// 通用大小类接口
void *kmalloc(uint size);
void  kfree(void *p);
```

## 代码落点与集成建议

- 新增 `slab.c/h`，在 `kernel/Makefile`/`kernel/defs.h` 注册导出符号。
- 底层仍使用 `kalloc/kfree` 获取/释放页；**不要改动**其对其他内核模块的语义。
- 在 `proc.c`/`file.c`/`bio.c` 等处**逐步替换**为 slab（先一两处，便于调试）。
- 打印统计信息的入口：可在 `kernel/procinfo.c` 或 `sysproc.c` 新增简单 syscall 或 shell 命令。

## 测试与验收

**功能测试**

- 并发 N 线程（在 xv6 内核线程或多个进程上下文中）持续 `alloc/free` 小对象，校验无崩溃、无泄漏（统计应归零）。
- 压力下创建/销毁大量进程、打开/关闭文件，观察是否稳定。

**性能与碎片**

- 随机大小 `kmalloc` 工作负载（Zipf/均匀），记录：
  - 吞吐（ops/s）、平均/尾延迟（cycles 或 ticks）。
  - 内存占用：`pages_in_use * PGSIZE` 与**有效负载**之比。
- 对比：**（基线）整页分配 + 自建对象池** vs **slab**。

**正确性/安全**

- 分配后填充 `0xAA`，释放后填充 `0xDD`（poison），跑完扫描异常。