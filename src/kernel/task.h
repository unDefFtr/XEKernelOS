#pragma once
#include "lib/types.h"
#include "lib/list.h"
#include "kernel/isr.h"

enum task_state {
    TASK_RUNNING,
    TASK_READY,
    TASK_BLOCKED,
    TASK_DEAD,
};

class PagingManager;  /* forward declaration */

/* 每个任务的内核栈大小。必须是 4096 的倍数 (页对齐, TSS ESP0 用栈顶)。
   原为 4KB — 实测 syscall 48 (SYS_VFS_DIR → fat.dir → read_root_sec →
   bc_read) 的调用链已溢出几个字节, 溢出的字落在本块 kmalloc 头部的
   magic 字段上, 破坏堆块链 → 之后每次 kmalloc 都失败 (open: e4)。
   8KB 留足余量。 */
#define KSTACK_SIZE 8192

/* 抢占式调度时间片 (PIT tick 数, 100Hz → 4 tick = 40ms)。
   时间片耗尽时 PIT 会抢占 ring3 任务并轮转到下一个同优先级任务,
   纯计算死循环 (不发起 syscall) 无法再饿死 GUI/其他任务。 */
#define TASK_QUANTUM 4

/* ---- 准则四：Capability Tokens ---- */
#define CAP_DISK_READ    (1 << 0)
#define CAP_DISK_WRITE   (1 << 1)
#define CAP_SCREEN       (1 << 2)   /* write to framebuffer / gfx */
#define CAP_SHUTDOWN     (1 << 3)
#define CAP_SIGNAL       (1 << 4)   /* send signals to other tasks */
#define CAP_SYSCALL      (1 << 5)   /* call any syscall */
#define CAP_FILE_READ    (1 << 6)   /* open/read files via fd */
#define CAP_FILE_WRITE   (1 << 7)   /* write/sync files via fd */
#define CAP_ALL          (CAP_DISK_READ | CAP_DISK_WRITE | CAP_SCREEN | \
                          CAP_SHUTDOWN | CAP_SIGNAL | CAP_SYSCALL | \
                          CAP_FILE_READ | CAP_FILE_WRITE)

#define MAX_FD 16

struct task_struct {
    u32 pid;
    u32 gs, fs, es, ds;
    u32 ecx, edx, ebx, ebp, esi, edi;
    u32 eax;          /* 调度恢复时还原的 eax (fork 子进程首次返回为 0) */
    u32 eip, cs, esp, eflags;
    u32 user_esp;     /* ring3 用户栈指针 (iretd 需要与 cs/ss 配对恢复) */
    u32 user_ss;      /* 0x23 */
    u8  state;
    struct list_head list;
    struct list_head all_list;   /* 挂入全局任务链 (含 BLOCKED, 供信号投递) */
    u32 kernel_stack;
    void (*entry)(void *);
    void *arg;
    PagingManager *paging;
    u32 user_stack;
    struct task_struct *parent;
    int  exit_code;
    struct list_head children;
    struct list_head sibling;

    /* ---- 准则三：动态优先级调度 ---- */
    u8   priority;        /* base priority 0-255 (higher = more CPU) */
    u8   dynamic_boost;   /* temporary boost from IRQ interaction */
    u32  boost_expire;    /* tick count when boost decays */
    u32  wake_tick;       /* >0: sys_sleep 到期 tick (PIT 唤醒) */
    u32  quantum;         /* 剩余时间片 (PIT tick), 0 → 抢占轮转 */

    /* ---- 准则四：Capability Tokens ---- */
    u32  caps;            /* capability bitmask */

    /* ---- 准则一：per-task output redirect ---- */
    int  output_fd;       /* -1=screen, >=0=FD for gfx output redirect */

    /* ---- FD table (准则二: per-task, was global V18) ---- */
    u8  *fd_buf[MAX_FD];
    u32  fd_size[MAX_FD];
    u32  fd_pos[MAX_FD];
    u8   fd_type[MAX_FD];

    /* Signal support */
    u32 pending_signals;
    u32 blocked_signals;
    u32 sig_handlers[32];
    u32 sig_saved_eip;
    u32 sig_saved_esp;
};

#define SIGKILL   9
#define SIGINT    2
#define SIGSEGV  11
#define SIGCHLD  17
#define SIGTERM  15
#define SIG_DFL   0   /* default action */
#define SIG_IGN   1   /* ignore */

/* ---- 任务管理器快照 (SYS_TASK_LIST) ---- */
struct task_info {
    u32 pid;
    u32 state;      /* task_state: 0=RUNNING 1=READY 2=BLOCKED 3=DEAD */
    u32 priority;   /* priority + dynamic_boost */
    u32 ring3;      /* 1 = 用户态任务 */
};
#define TASK_INFO_MAX 32

extern struct list_head ready_queue;
extern struct list_head all_tasks;    /* 全部任务 (含 BLOCKED/运行中) — 信号/能力查询用 */
extern struct task_struct *current_task;

void task_init(void);
int  task_create(void (*entry)(void *), void *arg);
int  task_create_user(void *entry, u32 user_stack_top, PagingManager *user_pd);
void task_exit(void);
void task_cleanup_user(void);  /* cleanup user task after SYS_EXIT, restore idle */
void task_yield(void);
void schedule(registers_t *r);
u32  task_next_pid(void);
int  task_send_signal(u32 pid, int sig);
void task_check_signals(registers_t *r);
/* 统一退出路径: 置 DEAD + 唤醒 waitpid 父进程 + 孤儿则跳回 shell 重启循环。
   SYS_EXIT 与信号杀死共用 — 修 shell_recover 复用死亡任务内核栈的 UAF */
void task_do_exit(registers_t *r, u32 exit_code);

/* 任务快照 (任务管理器用): 写入 out[], 返回条目数 (<= max) */
int  task_snapshot(struct task_info *out, int max);

/* Capability management (准则四) */
void task_boost_priority(u32 pid, u8 amount);  /* IRQ-triggered boost (准则三) */
void task_decay_boosts(void);                   /* periodic decay (准则三) */
bool task_has_cap(u32 pid, u32 cap);
int  task_drop_cap(u32 cap);                    /* remove capability from self */
