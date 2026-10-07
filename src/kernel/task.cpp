#include "kernel/task.h"
#include "kernel/paging.h"
#include "kernel/user.h"
#include "kernel/syscall.h"
#include "lib/heap.h"
#include "lib/ports.h"
#include "lib/uaccess.h"
#include "drivers/serial.h"

static u32 next_pid = 1;
static struct task_struct *main_task;

/* 自增分配 — 旧实现只读不自增, 所有 fork 子进程拿到同一个 pid
   (日志实证: 连续三次 RUN 的 child 都是 pid 2) */
u32 task_next_pid(void) { return next_pid++; }

struct list_head ready_queue;
struct list_head all_tasks;
struct task_struct *current_task;

/* 僵尸回收链: 已退出任务在此等待下一次 schedule() 释放。
   退出任务不能在自身栈上释放自己的内核栈 — 挂入本链,
   由 (必然运行在别的栈上的) 下一次 schedule 统一 kfree */
static struct list_head zombie_reap;

static void task_wrapper(void) {
    current_task->entry(current_task->arg);
    task_exit();
}

void task_init(void) {
    list_init(&ready_queue);
    list_init(&all_tasks);
    list_init(&zombie_reap);
    main_task = (task_struct *)kmalloc(sizeof(struct task_struct));
    main_task->pid = 0;
    main_task->state = TASK_RUNNING;
    main_task->eax = 0;
    main_task->gs = 0x10; main_task->fs = 0x10;
    main_task->es = 0x10; main_task->ds = 0x10;
    main_task->cs  = 0x18;   /* kernel code */
    main_task->user_esp = 0;
    main_task->user_ss  = 0x10;  /* kernel data */
    main_task->kernel_stack = 0;
    main_task->paging = PagingManager::get_kernel_paging();
    main_task->user_stack = 0;
    main_task->parent = nullptr;
    main_task->exit_code = 0;
    main_task->caps = CAP_ALL;  /* kernel shell has all privileges */
    /* fd 表必须显式初始化: kmalloc 不做清零, 而"物理 RAM 非全零"的宿主
       (VMware) 会让未初始化字段带上上电残留 → 被当作已打开的 fd 使用 */
    main_task->output_fd = -1;
    main_task->eip = 0; main_task->esp = 0; main_task->eflags = 0x202;
    for (int i = 0; i < MAX_FD; i++) {
        main_task->fd_buf[i] = nullptr; main_task->fd_size[i] = 0;
        main_task->fd_pos[i] = 0; main_task->fd_type[i] = 0;
    }
    for (int i = 0; i < 32; i++) main_task->sig_handlers[i] = 0;
    main_task->pending_signals = 0; main_task->blocked_signals = 0;
    main_task->sig_saved_eip = 0; main_task->sig_saved_esp = 0;
    main_task->priority = 128; main_task->dynamic_boost = 0;
    main_task->boost_expire = 0; main_task->wake_tick = 0;
    main_task->quantum = TASK_QUANTUM;
    list_init(&main_task->children);
    list_add_tail(&main_task->all_list, &all_tasks);
    current_task = main_task;
}

int task_create(void (*entry)(void *), void *arg) {
    struct task_struct *t = (task_struct *)kmalloc(sizeof(struct task_struct));
    if (!t) return -1;

    u32 *stack = (u32 *)kmalloc(KSTACK_SIZE);
    if (!stack) { kfree(t); return -1; }
    for (u32 i = 0; i < KSTACK_SIZE / 4; i++) stack[i] = 0xCCCCCCCC;

    u32 *sp = stack + KSTACK_SIZE / 4;

    *(--sp) = 0x202;             // eflags
    *(--sp) = 0x18;              // cs (kernel code selector)
    *(--sp) = (u32)task_wrapper; // eip
    *(--sp) = 0;                 // err
    *(--sp) = 0x20;              // vec
    *(--sp) = 0;                 // eax
    *(--sp) = 0;                 // ecx
    *(--sp) = 0;                 // edx
    *(--sp) = 0;                 // ebx
    sp--;                        // _esp slot
    *sp = 0;                     // initialized below to &frame->vec
    *(--sp) = 0;                 // ebp
    *(--sp) = 0;                 // esi
    *(--sp) = 0;                 // edi
    *(--sp) = 0x10;              // ds
    *(--sp) = 0x10;              // es
    *(--sp) = 0x10;              // fs
    *(--sp) = 0x10;              // gs
    registers_t *frame = (registers_t *)sp;
    frame->_esp = (u32)&frame->vec;
    t->pid = next_pid++;
    t->ecx = 0; t->edx = 0; t->ebx = 0; t->ebp = 0; t->esi = 0; t->edi = 0;
    t->eax = 0;
    t->eip = (u32)task_wrapper;
    t->cs  = 0x18;    /* kernel code */
    t->gs = 0x10; t->fs = 0x10; t->es = 0x10; t->ds = 0x10;
    t->esp = (u32)&frame->vec;
    t->eflags = 0x202;
    t->user_esp = 0;
    t->user_ss  = 0x10;
    t->state = TASK_READY;
    t->kernel_stack = (u32)stack;
    t->entry = entry;
    t->arg = arg;
    t->paging = PagingManager::get_kernel_paging();
    t->user_stack = 0;
    t->parent = nullptr;
    t->exit_code = 0;
    t->pending_signals = 0;
    t->blocked_signals = 0;
    for (int i = 0; i < 32; i++) t->sig_handlers[i] = 0;
    t->sig_saved_eip = 0;
    t->sig_saved_esp = 0;
    list_init(&t->children);
    t->priority = 128;
    t->dynamic_boost = 0;
    t->boost_expire = 0;
    t->wake_tick = 0;
    t->quantum = TASK_QUANTUM;
    t->caps = CAP_ALL;
    t->output_fd = -1;
    for (int i = 0; i < MAX_FD; i++) {
        t->fd_buf[i] = nullptr; t->fd_size[i] = 0;
        t->fd_pos[i] = 0; t->fd_type[i] = 0;
    }
    list_add_tail(&t->all_list, &all_tasks);
    list_add_tail(&t->list, &ready_queue);

    return t->pid;
}

int task_create_user(void *entry, u32 user_stack_top, PagingManager *user_pd) {
    struct task_struct *t = (task_struct *)kmalloc(sizeof(struct task_struct));
    if (!t) return -1;

    u32 *kstack = (u32 *)kmalloc(KSTACK_SIZE);
    if (!kstack) { kfree(t); return -1; }
    for (u32 i = 0; i < KSTACK_SIZE / 4; i++) kstack[i] = 0xCCCCCCCC;

    /* The kernel stack top: when entering from ring3 via interrupt,
       the CPU pushes SS, ESP, EFLAGS, CS, EIP onto this stack.
       We set ESP0 in the TSS to point here. */
    u32 *sp = kstack + KSTACK_SIZE / 4;

    /* Build initial interrupt frame for returning to user mode via iretd.
       The frame format from top to bottom:
       SS, user_ESP, EFLAGS, CS, EIP, err=0, vec */
    *(--sp) = 0x23;                 // SS (user data selector)
    *(--sp) = user_stack_top;       // ESP
    *(--sp) = 0x202;                // EFLAGS (IF set)
    *(--sp) = 0x2B;                 // CS (user code selector)
    *(--sp) = (u32)entry;           // EIP
    *(--sp) = 0;                    // err_code
    *(--sp) = 0x20;                 // vec (timer, will be overwritten)

    /* pusha slots */
    *(--sp) = 0;                    // eax
    *(--sp) = 0;                    // ecx
    *(--sp) = 0;                    // edx
    *(--sp) = 0;                    // ebx
    sp--;                           // _esp slot
    *sp = 0;                        // initialized below to &frame->vec
    *(--sp) = 0;                    // ebp
    *(--sp) = 0;                    // esi
    *(--sp) = 0;                    // edi
    *(--sp) = 0x23;                 // ds
    *(--sp) = 0x23;                 // es
    *(--sp) = 0x23;                 // fs
    *(--sp) = 0x23;                 // gs
    registers_t *frame = (registers_t *)sp;
    frame->_esp = (u32)&frame->vec;

    t->pid = next_pid++;
    t->ecx = 0; t->edx = 0; t->ebx = 0; t->ebp = 0; t->esi = 0; t->edi = 0;
    t->gs = 0x23; t->fs = 0x23; t->es = 0x23; t->ds = 0x23;
    t->eax = 0;
    t->eip = (u32)entry;
    t->cs  = 0x2B;    /* user code (ring3) */
    t->esp = (u32)&frame->vec;
    t->eflags = 0x202;
    t->user_esp = user_stack_top;   /* iretd 恢复 ring3 时使用 */
    t->user_ss  = 0x23;
    t->state = TASK_READY;
    t->kernel_stack = (u32)kstack;
    t->entry = nullptr;
    t->arg = nullptr;
    t->paging = user_pd;
    t->user_stack = user_stack_top;
    t->parent = current_task;
    t->exit_code = 0;
    t->pending_signals = 0;
    t->blocked_signals = 0;
    for (int i = 0; i < 32; i++) t->sig_handlers[i] = 0;
    t->sig_saved_eip = 0;
    t->sig_saved_esp = 0;
    list_init(&t->children);
    t->priority = 128;
    t->dynamic_boost = 0;
    t->boost_expire = 0;
    t->wake_tick = 0;
    t->quantum = TASK_QUANTUM;
    t->caps = (current_task ? current_task->caps : CAP_ALL); /* 准则四: inherit */
    t->output_fd = -1;
    for (int i = 0; i < MAX_FD; i++) {
        t->fd_buf[i] = nullptr; t->fd_size[i] = 0;
        t->fd_pos[i] = 0; t->fd_type[i] = 0;
    }
    /* fd 0 = stdout (framebuffer) — 准则一: 万物皆 fd */
    t->fd_buf[0]  = (u8 *)1;  /* sentinel (non-null) */
    t->fd_type[0] = 4;        /* framebuffer type */
    list_add_tail(&t->sibling, &current_task->children);
    list_add_tail(&t->list, &ready_queue);
    list_add_tail(&t->all_list, &all_tasks);

    current_task = t;
    t->state = TASK_RUNNING;
    /* 关键修复: 任务随即被直接运行 (enter_user_mode), 必须立刻出队。
       调度器不变式: 运行中的任务绝不挂在 ready_queue 上 —
       否则它阻塞时 (state=BLOCKED 但仍在队列) 被子进程退出唤醒时
       list_add_tail 二次挂接, 节点脱离链表成孤立环, 调度器扫描死循环 */
    list_del(&t->list);

    return t->pid;
}


void task_exit(void) {
    if (!current_task) return;

    /* Mark DEAD and remove from ready queue.
       Do NOT free kernel_stack or current_task here — we're still
       running on this stack! Cleanup is handled in schedule(). */
    current_task->state = TASK_DEAD;
    /* 任务被调度选中时 list_del 已把节点移出队列 (next/prev 置 0),
       只有仍挂接队列时才需要删除, 避免二次 list_del 解引用空指针 */
    if (current_task->list.next)
        list_del(&current_task->list);

    /* Wake up parent if it's waiting (waitpid) */
    if (current_task->parent) {
        list_del(&current_task->sibling);
        if (current_task->parent->state == TASK_BLOCKED) {
            current_task->parent->state = TASK_READY;
            list_add_tail(&current_task->parent->list, &ready_queue);
        }
    }

    /* Switch back to kernel page table (safe: we're still on kernel stack) */
    PagingManager::get_kernel_paging()->load();

    /* Build a minimal interrupt frame and yield to scheduler.
       schedule() will see TASK_DEAD, skip it, and free resources
       from the NEXT task's stack. */
    registers_t fake;
    fake.gs = 0x10; fake.fs = 0x10; fake.es = 0x10; fake.ds = 0x10;
    fake.cs = 0x18;
    fake.eflags = 0x202;
    fake.eip = 0;
    fake._esp = 0;
    __asm__ volatile("mov %%esp, %0" : "=m"(fake._esp));
    schedule(&fake);

    /* Should never reach here */
    for (;;) __asm__ volatile("hlt");
}

/* Clean up the current user task and restore the idle task.
   Called from cmd_usersh() after enter_user_mode() returns
   (i.e., SYS_EXIT restored g_entry_esp). Without this cleanup,
   current_task still points to the exited user task, causing:
   - PIT ISR calls task_boost_priority() on stale task
   - schedule() context-switches to stale task → crash/black screen
   - mcursor_update() appears frozen because PIT IRQ never returns
     cleanly after schedule() corrupts the stack */
void task_cleanup_user(void) {
    if (!current_task || current_task->pid == 0) return;

    /* Remove from ready queue and sibling list.
       list 节点可能已出队 (运行中任务不在 ready_queue), 空指针守卫 */
    if (current_task->list.next)
        list_del(&current_task->list);
    if (current_task->all_list.next)
        list_del(&current_task->all_list);
    if (current_task->parent)
        list_del(&current_task->sibling);

    /* Free user page directory (PD + page tables).
       Do NOT free the identity-mapped physical pages — they
       belong to the kernel PSE identity map. */
    if (current_task->paging && current_task->paging != PagingManager::get_kernel_paging())
        delete current_task->paging;

    /* Free kernel stack */
    if (current_task->kernel_stack)
        kfree((void *)current_task->kernel_stack);

    /* Free task struct and restore idle task */
    kfree(current_task);
    current_task = main_task;
}

void task_yield(void) {
    __asm__ volatile("int $0x20");
}

void schedule(registers_t *r) {
    if (!current_task) return;

    /* 先缓存 state: 退出任务 (DEAD) 的本体已挂入 zombie_reap,
       下面的释放会 kfree 它 — 之后任何 current_task 解引用都是 UAF */
    u8 cur_state = current_task->state;

    /* DEAD (退出蹦床路径): 不保存上下文 — 保存无意义且属
       use-after-free 写, 会踩坏堆元数据波及后续任务 (实测:
       间歇性 #GP + 垃圾 EFLAGS) */
    if (cur_state != TASK_DEAD) {
        current_task->ecx = r->ecx;
        current_task->edx = r->edx;
        current_task->ebx = r->ebx;
        current_task->ebp = r->ebp;
        current_task->esi = r->esi;
        current_task->edi = r->edi;
        current_task->gs = r->gs;
        current_task->fs = r->fs;
        current_task->es = r->es;
        current_task->ds = r->ds;
        current_task->eax = r->eax;
        current_task->eip = r->eip;
        current_task->cs  = r->cs;
        current_task->esp = r->_esp;
        current_task->eflags = r->eflags;
        /* 用户栈指针只在 ring3 帧上有效 (ring0 帧该位置是栈外数据) */
        if (r->cs & 3) {
            current_task->user_esp = r->user_esp;
            current_task->user_ss  = r->user_ss;
        }
    }

    /* ---- 释放僵尸 (含刚退出的 current 本体; 运行在蹦床栈上安全) ---- */
    {
        struct list_head *pos, *tmp;
        list_for_each_safe(pos, tmp, &zombie_reap) {
            struct task_struct *t = container_of(pos, struct task_struct, list);
            list_del(pos);
            if (t->all_list.next)
                list_del(&t->all_list);
            /* paging 已在 task_do_exit 里 delete 并指回内核页目录 */
            kfree((void *)t->kernel_stack);
            kfree(t);
        }
    }

    /* ---- 防御: ready_queue 上残留的 DEAD (正常不应有) ---- */
    {
        struct list_head *pos, *tmp;
        list_for_each_safe(pos, tmp, &ready_queue) {
            struct task_struct *t = container_of(pos, struct task_struct, list);
            if (t->state == TASK_DEAD) {
                list_del(pos);
                if (t->all_list.next)
                    list_del(&t->all_list);
                if (t->paging && t->paging != PagingManager::get_kernel_paging())
                    delete t->paging;
                kfree((void *)t->kernel_stack);
                kfree(t);
            }
        }
    }

    /* Re-queue running tasks (skip DEAD)。
       cur_state 为 RUNNING 时 current 未进 reap 链、未被释放 ✓ */
    if (cur_state == TASK_RUNNING) {
        current_task->state = TASK_READY;
        list_add_tail(&current_task->list, &ready_queue);
    }

    if (list_empty(&ready_queue)) {
        if (cur_state == TASK_DEAD) {
            /* 队列空且 current 已释放 — 正常不会发生 (父进程已入队);
               防御性停机避免 UAF 解引用 */
            for (;;) __asm__ volatile("hlt");
        }
        /* 全员 BLOCKED (如唯一任务 sys_sleep): 开中断等待。
           本上下文为 ring0 (内核页表, cs=0x18), PIT ISR 的
           from_user=0 → 不做 paging->load, 嵌套安全 */
        while (list_empty(&ready_queue))
            __asm__ volatile("sti; hlt; cli");
        /* 有任务被唤醒入队 → 落入下方正常挑选 */
    }

    /* ---- 准则三: O(1) dynamic priority pick ----
       取队首第一个最高优先级项 (用 > 而非 >=):
       时间片耗尽的 current 刚被 list_add_tail 塞到队尾,
       同优先级下必须轮到队首那个任务 — 否则永远选中自己,
       抢占式调度退化成"只重入不切换"。 */
    struct list_head *pos;
    struct task_struct *nt = nullptr;
    u8 best_prio = 0;

    list_for_each(pos, &ready_queue) {
        struct task_struct *t = container_of(pos, struct task_struct, list);
        u8 eff = t->priority + t->dynamic_boost;
        if (eff > 255) eff = 255;
        if (eff > best_prio) {
            best_prio = eff;
            nt = t;
        }
    }

    if (!nt) { nt = container_of(ready_queue.next, struct task_struct, list); }

    list_del(&nt->list);
    nt->state = TASK_RUNNING;
    nt->quantum = TASK_QUANTUM;   /* 新获得 CPU 的任务重置时间片 */

    /* 仅在真正切换任务时输出 (sys_sleep 的 tick 级自切换不打印) */
    if (nt->pid != current_task->pid) {
        serial_write_str("sched: pid ");
        serial_write_u32(current_task->pid);
        serial_write_str(" -> pid ");
        serial_write_u32(nt->pid);
        /* 诊断 (ring0 兼容模式排查): 恢复目标的上下文取值 */
        serial_write_str(" eip=0x"); serial_write_u32(nt->eip);
        serial_write_str(" cs=0x");  serial_write_u32(nt->cs);
        serial_write_str(" esp=0x"); serial_write_u32(nt->esp);
        serial_write_str(" uesp=0x");serial_write_u32(nt->user_esp);
        serial_write_char('\n');
    }

    /* ---- 准则三: decay dynamic boost each tick ---- */
    static u32 decay_counter = 0;
    if (++decay_counter >= 10) {  /* ~100ms at 100Hz */
        decay_counter = 0;
        list_for_each(pos, &ready_queue) {
            struct task_struct *t = container_of(pos, struct task_struct, list);
            if (t->dynamic_boost > 0) t->dynamic_boost--;
        }
    }

    /* ---- 修复 V9: CR3 BEFORE EIP ---- */
    if (nt->paging && nt->paging != current_task->paging) {
        nt->paging->load();
    }

    /* ---- 关键: 切换任务后必须更新 TSS ESP0 ----
       否则子进程在用户态触发 syscall/中断时, CPU 会使用
       父进程的 ESP0 (旧任务内核栈顶) → 两个任务共用同一内核栈
       → 栈互相覆盖, 触发#DF/#PF 崩溃. */
    if (nt->kernel_stack)
        user_tss_set_esp0(nt->kernel_stack + KSTACK_SIZE);

    /* Restore context (EIP restored AFTER CR3) */
    r->ecx = nt->ecx;
    r->edx = nt->edx;
    r->ebx = nt->ebx;
    r->ebp = nt->ebp;
    r->esi = nt->esi;
    r->edi = nt->edi;
    r->eax = nt->eax;   /* 还原各任务自己的 eax (fork 子进程首次=0) */
    r->eip = nt->eip;
    r->cs  = nt->cs;
    r->gs = nt->gs;
    r->fs = nt->fs;
    r->es = nt->es;
    r->ds = nt->ds;
    r->_esp = nt->esp;
    r->eflags = nt->eflags;
    /* 诊断: 恢复帧的关键字段 (默认关闭 — 100Hz 串口洪泛会触发
       QEMU tcp chardev 背压, 串口 putc 忙等 (IF=0) 冻结整个内核) */
    if (0 && (nt->cs & 3)) {
        serial_write_str("sched-restore: pid ");
        serial_write_u32(nt->pid);
        serial_write_str(" eip=");
        serial_write_u32(nt->eip);
        serial_write_str(" uesp=");
        serial_write_u32(nt->user_esp);
        serial_write_char('\n');
    }
    /* 恢复目标任务的用户栈: iretd 弹出到 ring3 时会从帧的
       eflags 之上取 ESP/SS — 不写则沿用被中断任务的用户栈,
       目标任务在错误的栈上运行 (旧代码靠 fork 同 esp 巧合掩盖) */
    if (nt->cs & 3) {
        r->user_esp = nt->user_esp;
        r->user_ss  = nt->user_ss;
    }

    current_task = nt;
}

/* ---- 统一退出路径 (SYS_EXIT / 信号杀死共用) ---- */

/* 自释放蹦床所需的静态栈与帧副本 (见 task_do_exit) */
static u8 reaper_stack__[4096] __attribute__((aligned(16)));
static registers_t g_reaper_frame;

/* extern "C" 包装: 内联 asm 通过未修饰符号调用 */
extern "C" void task_schedule_c(registers_t *r) { schedule(r); }

void task_do_exit(registers_t *r, u32 exit_code) {
    if (!current_task) return;

    task_struct *self   = current_task;
    task_struct *parent = self->parent;

    self->exit_code = exit_code;
    self->state = TASK_DEAD;
    /* 运行中任务不在 ready_queue — 出队守卫防空指针解引用 */
    if (self->list.next)
        list_del(&self->list);

    if (parent && parent->pid != 0) {
        /* 有父进程 — 收割工作在此全部完成。
           背景: schedule() 原地改帧, waitpid 阻塞后被唤醒时 iretd
           直接回用户态, schedule 之后的 C 代码永远不执行 —
           旧实现把收割留在 waitpid 的唤醒路径, 僵尸永远残留,
           下一次 waitpid 扫到旧僵尸立即返回 → RUN 第一次不等待
           直接回提示符 (串口日志实证)。 */
        list_del(&self->sibling);        /* 从 parent->children 摘除 */

        if (parent->state == TASK_BLOCKED) {
            /* waitpid 返回值经父进程保存帧的 eax 传递 */
            parent->eax = self->pid;
            parent->state = TASK_READY;
            list_add_tail(&parent->list, &ready_queue);
            serial_write_str("exit-wake: parent pid ");
            serial_write_u32(parent->pid);
            serial_write_str(" uesp=");
            serial_write_u32(parent->user_esp);
            serial_write_str(" ebp=");
            serial_write_u32(parent->ebp);
            serial_write_char('\n');
        }

        /* 释放地址空间 (已先切回内核页目录) */
        PagingManager::get_kernel_paging()->load();
        if (self->paging && self->paging != PagingManager::get_kernel_paging()) {
            delete self->paging;
            self->paging = PagingManager::get_kernel_paging();
        }

        /* 自释放: 自身 kstack/task_struct 挂入 zombie_reap,
           由下一次 schedule (必然运行在别的栈上) kfree。
           本帧切到静态蹦床栈再调 schedule — schedule 会把父进程
           上下文写进帧副本并经 popa/iretd 直接切回父进程用户态 */
        list_add_tail(&self->list, &zombie_reap);

        g_reaper_frame = *r;
        g_reaper_frame.eflags = 0x202;
        g_reaper_frame.eip = 0;
        g_reaper_frame._esp = 0;

        u32 sp = (u32)(reaper_stack__ + sizeof(reaper_stack__) - 64);
        u32 fr = (u32)&g_reaper_frame;
        __asm__ volatile(
            "movl %1, %%ebx\n\t"    /* 帧地址存入被调用者保存寄存器 —
                                       call 按 ABI 破坏 eax/ecx/edx,
                                       旧版 %1 若分配在其中, call 后
                                       mov %1,esp 读到垃圾 → popa/iret 跑飞 */
            "movl %0, %%esp\n\t"    /* 切到蹦床栈 */
            "pushl %%ebx\n\t"       /* schedule(&g_reaper_frame) */
            "call task_schedule_c\n\t"
            "movl %%ebx, %%esp\n\t" /* 帧已被填成下一任务的上下文 */
            "jmp isr_return\n\t"
            "ud2\n"
            :
            : "r"(sp), "r"(fr)
            : "ebx", "memory"
        );
        __builtin_unreachable();
    }

    /* 孤儿进程 (初始 Shell): 跳回 boot loop 重启。
       mov 切回 kernel_main 栈后返回, task_cleanup_user 负责释放 */
    PagingManager::get_kernel_paging()->load();
    __asm__ volatile(
        "movw $0x10, %%ax\n"
        "movw %%ax, %%ds\n"
        "movw %%ax, %%es\n"
        "movw %%ax, %%fs\n"
        "movw %%ax, %%gs\n"
        "cld\n"
        "mov %0, %%esp\n"
        "pop %%ebp\n"
        "ret\n"
        :
        : "m"(g_entry_esp)
        : "eax", "memory"
    );
    __builtin_unreachable();
}

/* ---- Signal support ---- */

int task_send_signal(u32 pid, int sig) {
    if (sig < 1 || sig > 31) return -1;

    /* 遍历全局任务链 (含 BLOCKED — waitpid 阻塞的进程也能收到信号;
       旧实现只扫 ready_queue, 阻塞任务永远收不到) */
    struct list_head *pos;
    list_for_each(pos, &all_tasks) {
        struct task_struct *t = container_of(pos, struct task_struct, all_list);
        if (t->pid == pid) {
            t->pending_signals |= (1u << sig);
            return 0;
        }
    }
    return -1;  /* task not found */
}

/* Deliver pending signal to current task.
   Called from isr.cpp before returning to user mode.
   Modifies r to redirect execution to signal handler. */
void task_check_signals(registers_t *r) {
    if (!current_task || current_task->pid == 0) return;
    if (current_task->pending_signals == 0) return;

    /* Find first pending signal not blocked */
    for (int sig = 1; sig <= 31; sig++) {
        if (!(current_task->pending_signals & (1u << sig))) continue;
        if (current_task->blocked_signals & (1u << sig)) continue;

        /* Clear the pending bit */
        current_task->pending_signals &= ~(1u << sig);

        u32 handler = current_task->sig_handlers[sig];

        /* 默认动作 = 终止的信号: SIGKILL/SIGSEGV/SIGINT/SIGTERM。
           SIGINT 必须在此终止 — 旧实现把 SIG_DFL 一律 continue,
           Ctrl+C 被静默忽略 (实测: 死循环任务无法用 Ctrl+C 结束) */
        if (handler == SIG_DFL && (sig == SIGKILL || sig == SIGSEGV ||
                                   sig == SIGINT  || sig == SIGTERM)) {
            serial_write_str("signal: killing pid ");
            serial_write_char('0' + (current_task->pid / 10) % 10);
            serial_write_char('0' + current_task->pid % 10);
            serial_write_str(" with signal ");
            serial_write_char('0' + (sig / 10) % 10);
            serial_write_char('0' + sig % 10);
            serial_write_char('\n');

            /* 清 fd 表 (含管道引用计数) — 修复信号杀死路径 fd 泄漏 */
            syscall_cleanup_fds(current_task);
            /* 统一退出: 唤醒父进程走调度器 / 孤儿跳回 shell 循环。
               旧实现无条件 list_del 已出队节点 → 空指针崩溃 */
            task_do_exit(r, (u32)sig);
            return;
        }

        /* SIG_IGN or default (non-fatal) → ignore */
        if (handler == SIG_IGN || handler == SIG_DFL) continue;

        /* handler 地址按当前任务页表校验 (要求 USER 可达) —
           旧实现的 0x20000~0x410000 区间判断包含大片未映射区 */
        if (!current_task->paging ||
            !current_task->paging->translate_user(handler)) {
            serial_write_str("sig: bad handler ");
            serial_write_char('0' + sig % 10);
            serial_write_str(" pid=");
            serial_write_char('0' + (current_task->pid / 10) % 10);
            serial_write_char('0' + current_task->pid % 10);
            serial_write_str(" ignored\n");
            continue;
        }

        /* Custom handler — 仅 ring3 上下文可投递自定义 handler */
        if (!(r->cs & 3)) continue;

        serial_write_str("sig: ");
        serial_write_char('0' + sig % 10);
        serial_write_str(" -> pid ");
        serial_write_char('0' + (current_task->pid / 10) % 10);
        serial_write_char('0' + current_task->pid % 10);
        serial_write_char('\n');

        /* Save current user context */
        current_task->sig_saved_eip = r->eip;
        current_task->sig_saved_esp = r->user_esp;

        /* Build signal frame on USER stack (旧实现写在 r->_esp —
           那是 pusha 时的内核栈指针, 对用户态完全无效):
           [ESP] = signum, [ESP+4] = 0 (sigreturn 标记) */
        u32 frame[2] = { (u32)sig, 0 };
        if (!copy_to_user(r->user_esp - 8, frame, 8)) {
            /* 用户栈不可写 (最底页) → 无法投递, 忽略 */
            continue;
        }

        /* Redirect to handler */
        r->eip = handler;
        r->user_esp -= 8;

        break;  /* deliver one signal per interrupt return */
    }
}

/* ---- Capability management (准则四) ---- */

bool task_has_cap(u32 pid, u32 cap) {
    struct list_head *pos;
    list_for_each(pos, &all_tasks) {
        struct task_struct *t = container_of(pos, struct task_struct, all_list);
        if (t->pid == pid) return (t->caps & cap) != 0;
    }
    return false;
}

int task_drop_cap(u32 cap) {
    if (!current_task) return -1;
    current_task->caps &= ~cap;
    return 0;
}

/* ---- Dynamic priority boost (准则三) ---- */

void task_boost_priority(u32 pid, u8 amount) {
    /* Boost the task with given PID (called from IRQ1/IRQ12 handler) */
    struct list_head *pos;
    list_for_each(pos, &all_tasks) {
        struct task_struct *t = container_of(pos, struct task_struct, all_list);
        if (t->pid == pid) {
            t->dynamic_boost += amount;
            if (t->dynamic_boost > 100) t->dynamic_boost = 100;
            return;
        }
    }
}

/* ---- 任务管理器快照 ---- */

int task_snapshot(struct task_info *out, int max) {
    if (!out || max <= 0) return 0;
    if (max > TASK_INFO_MAX) max = TASK_INFO_MAX;

    int n = 0;
    struct list_head *pos;
    list_for_each(pos, &all_tasks) {
        if (n >= max) break;
        struct task_struct *t = container_of(pos, struct task_struct, all_list);
        if (t->state == TASK_DEAD) continue;
        u32 eff = (u32)t->priority + t->dynamic_boost;
        if (eff > 255) eff = 255;
        out[n].pid      = t->pid;
        out[n].state    = t->state;
        out[n].priority = eff;
        out[n].ring3    = (t->cs & 3) ? 1 : 0;
        n++;
    }
    return n;
}
