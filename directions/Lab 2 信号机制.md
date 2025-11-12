# Lab 2 信号机制

[Linux Signals](https://www.ic.unicamp.br/~celio/mc514/linux/linux_pgsignals.html)

## 实例程序

为了验证信号机制，我们可以设计一个简单的用户程序。这个程序会注册一个信号处理函数，然后`fork`一个子进程。子进程进入无限循环，父进程在短暂休眠后向子进程发送信号。我们以此观察信号处理函数是否被正确执行。

**`sigtest.c` (示例代码):**

```c
#include "types.h"
#include "stat.h"
#include "user.h"

// 信号处理函数
void sighandler(int signum) {
    printf(1, "sigtest: 捕获到信号 %d\n", signum);
    // 信号处理完毕后，调用 sigreturn 恢复现场
    // 注意：这里不能使用 C 函数的 return
    sigreturn();
}

int main(int argc, char *argv[]) {
    int pid;

    printf(1, "sigtest: 开始测试...\n");

    pid = fork();

    if (pid == 0) {
        // 子进程：注册信号处理函数并等待
        // 假设 2 号信号为 SIGINT
        signal(2, sighandler);
        printf(1, "sigtest (子进程 %d): 等待信号...\n", getpid());
        while (1) {
            // 保持运行，等待信号中断
        }
    } else if (pid > 0) {
        // 父进程：等待一会儿，然后向子进程发送信号
        sleep(50); // 等待 5 秒 (xv6 的 sleep 单位是 ticks)
        printf(1, "sigtest (父进程 %d): 向子进程 %d 发送信号 2\n", getpid(), pid);
        kill(pid, 2); // 发送信号
        wait(); // 等待子进程退出（在这个例子中子进程不会主动退出）
        printf(1, "sigtest (父进程): 子进程已结束\n");
    } else {
        printf(2, "sigtest: fork 失败\n");
    }

    exit();
}
```

**预期行为:**

1. 程序启动，打印 "sigtest: 开始测试..."。
2. `fork` 创建子进程。
3. 子进程打印 "sigtest (子进程 X): 等待信号..." 并进入循环。
4. 父进程等待 5 秒。
5. 父进程打印 "sigtest (父进程 Y): 向子进程 X 发送信号 2"。
6. 父进程调用 `kill`。
7. 子进程的循环被中断，执行 `sighandler` 函数，打印 "sigtest: 捕获到信号 2"。
8. `sighandler` 调用 `sigreturn()`，内核恢复子进程的执行现场。
9. 子进程从被中断的地方继续执行（即 `while(1)` 循环）。

## 相关系统调用

为了实现一个基本的信号机制，我们至少需要以下三个系统调用：

1. **`int signal(int signum, void (\*handler)(int))`**
   - **功能:** 注册或修改指定信号 `signum` 的处理函数 `handler`。
   - **参数:** `signum`: 信号编号 (例如，2 代表 `SIGINT`)。 `handler`: 指向信号处理函数的指针。
   - **返回值:** 成功时返回 0，失败时返回 -1。
2. **`int kill(int pid, int signum)`**
   - **功能:** 向指定 `pid` 的进程发送信号 `signum`。
   - **参数:** `pid`: 目标进程的 ID。`signum`: 要发送的信号编号。
   - **返回值:** 成功时返回 0，失败时返回 -1 (例如，进程不存在)。
3. **`int sigreturn(void)`**
   - **功能:** 信号处理函数调用此系统调用，以恢复被信号中断前的进程现场。
   - **参数:** 无。
   - **返回值:** 恢复现场（不返回到调用处），如果失败则返回 -1。

## 3. 设计方案

### 数据结构修改

我们需要在进程控制块 (`struct proc`，位于 `proc.h`) 中添加字段来支持信号：

```
// 在 proc.h 的 struct proc 中添加：

struct proc {
  // ... 其他字段 ...
  int killed;                   // 进程是否被杀死
  struct file *ofile[NOFILE];   // 打开的文件
  struct inode *cwd;            // 当前目录
  char name[16];                // 进程名

  // --- 新增信号相关字段 ---
  uint pending_signals;         // 待处理信号的位掩码 (例如，第 2 位为 1 表示收到了 SIGINT)
  void (*handlers[NSIG])(int); // 信号处理函数指针数组 (NSIG 是最大信号数量，比如 32)
  struct trapframe tf_backup;   // 保存陷阱帧 (注意：是结构体本身，不是指针)
  int tf_backup_valid;        // 标记 tf_backup 是否有效
};
```

*(注意: `NSIG` 需要在 `param.h` 或类似文件中定义，例如 `#define NSIG 32`)*

### 3.1 `signal()` 系统调用设计

**`sys_signal()` (内核实现):**

1. **获取参数:** 从用户栈获取 `signum` 和 `handler` (函数指针)。
2. **参数校验:**
   - 检查 `signum` 是否在有效范围内 (例如 `0` 到 `NSIG-1`)。
   - (可选) 检查 `handler` 指针是否指向有效的用户空间地址。
3. **注册处理函数:**
   - 获取当前进程的 `struct proc` (使用 `myproc()`)。
   - 将处理函数指针存入进程的数组中：`myproc()->handlers[signum] = handler;`
4. **返回值:** 返回 0。

### 3.2 `kill()` 系统调用设计

**`sys_kill()` (内核实现):**

1. **获取参数:** 从用户栈获取 `pid` 和 `signum`。
2. **参数校验:**
   - 检查 `signum` 是否在有效范围内。
3. **查找目标进程:**
   - 遍历进程表 (`ptable`)。
   - 查找 `pid` 匹配的目标进程。
4. **发送信号:**
   - 如果找到目标进程：
     - 获取 `ptable.lock` 锁（确保原子操作）。
     - 使用位运算设置目标进程的 `pending_signals` 字段： `target_proc->pending_signals |= (1 << signum);`
     - 释放 `ptable.lock` 锁。
   - 如果未找到进程，返回 -1。
5. **返回值:** 返回 0。

### 3.3 `sigreturn()` 系统调用设计

**`sys_sigreturn()` (内核实现):**

1. **获取进程:** `struct proc *p = myproc();`
2. **检查备份有效性:**
   - 检查 `p->tf_backup_valid` 标志。如果为 0 (无效)，说明没有可恢复的现场（例如用户在非信号处理时调用了 `sigreturn`），返回 -1。
3. **恢复陷阱帧:**
   - `*p->tf = p->tf_backup;` // 从备份中复制数据，恢复 `tf`
4. **清除备份标志:**
   - `p->tf_backup_valid = 0;` // 标记备份已失效
5. **返回值:**
   - 返回 0。此时内核 `trap` 函数的后半部分会继续执行，但由于 `p->tf` 已被恢复，`iret` 指令将使用恢复后的 `eip`, `esp` 等寄存器，使进程返回到被信号中断前的地方。

### 3.4 核心机制：信号的检查与处理

信号的发送 (`kill`) 只是标记了进程“应该”处理一个信号。真正的处理需要发生在进程从内核态返回用户态的前夕，通常是在 `trap()` 函数（位于 `trap.c`）的末尾。

**修改 `trap()` (在 `trap.c` 中):**

在 `trap()` 函数处理完系统调用或时钟中断，即将返回用户空间之前，添加信号检查逻辑：

```
// 在 trap.c 的 trap() 函数末尾，usertrapret() 之前

void trap(struct trapframe *tf) {
  // ... 原有的中断/系统调用处理 ...

  // 检查是否是从用户空间来的陷阱 (例如系统调用或时钟中断)
  if (myproc() != 0 && myproc()->state == RUNNING && (tf->cs & 3) == 3) {
    
    // 检查当前进程是否有待处理信号
    struct proc *p = myproc();

    // 检查是否有待处理信号，并且我们 *不* 处于一个信号处理函数中
    // (防止在恢复现场时再次被信号中断)
    if (p->pending_signals != 0 && p->tf_backup_valid == 0) {
      // 遍历所有可能的信号
      for (int signum = 0; signum < NSIG; signum++) {
        if (p->pending_signals & (1 << signum)) {
          // 找到了一个待处理信号
          void (*handler)(int) = p->handlers[signum];

          if (handler != 0) { // 检查是否注册了处理函数
            // 1. 清除该待处理信号
            p->pending_signals &= ~(1 << signum);

            // 2. 备份当前陷阱帧 (用于 sigreturn 恢复)
            p->tf_backup = *tf; // 复制整个陷阱帧
            p->tf_backup_valid = 1; // 标记备份有效

            // 3. 篡改陷阱帧，使其返回到用户空间的信号处理函数
            // a. 将信号编号 (signum) 作为参数压入用户栈
            tf->esp -= 4;
            *(uint *)(tf->esp) = signum;

            // b. 将一个假的“返回地址”压入用户栈
            // (这个地址在处理函数结束后不会被使用，
            // 因为 sighandler 必须调用 sigreturn() 来恢复)
            tf->esp -= 4;
            *(uint *)(tf->esp) = 0xFFFFFFFF; // 假的返回地址

            // c. 设置 EIP 指向信号处理函数
            tf->eip = (uint)handler;

            // 4. 函数执行完毕，陷阱返回将“返回”到 handler，而不是原来的 EIP
            return; // 立即返回用户空间执行 handler

          } else {
            // 没有注册处理函数，执行默认操作 (例如，终止进程)
            // 简单实现：清除信号
            p->pending_signals &= ~(1 << signum); 
          }
        }
      }
    }
    
    // ... 原有的 usertrapret() 调用（如果适用）...
  }
  
  // ... 其他内核陷阱处理 ...
}
```

**注意:** 上述 `trap()` 的修正是最关键也是最复杂的部分。一个完整的实现还需要考虑（但在初步实验中可以简化）：

1. **`sigreturn()` 系统调用:** 当用户空间的 `sighandler` 执行完毕后，它如何返回到被中断的主程序流程？它需要调用 `sigreturn()`，该调用会从进程保存的 `tf_backup` 中恢复现场。**关键点:** 用户提供的 `sighandler` 函数 *必须* 在末尾调用 `sigreturn()`，如 `sigtest.c` 示例中所示，而不能正常 `return`。
2. **信号屏蔽 (Signal Masking):** 在执行信号处理函数期间，应暂时阻塞同类型的信号（甚至所有信号），防止重入。