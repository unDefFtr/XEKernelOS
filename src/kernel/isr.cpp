#include "kernel/isr.h"
#include "kernel/panic.h"
#include "kernel/task.h"
#include "kernel/paging.h"
#include "kernel/user.h"
#include "kernel/syscall.h"
#include "drivers/gfx.h"
#include "drivers/keyboard.h"
#include "drivers/serial.h"
#include "drivers/pit.h"
#include "shell/shell.h"
#include "lib/ports.h"
#include "lib/heap.h"

IsrManager isr_mgr;

void IsrManager::register_handler(int vec, void (*fn)(void)) {
    if (vec >= 0 && vec < 256) handlers_[vec] = fn;
}

extern "C" void syscall_handler(registers_t *r);

extern "C" void c_isr_handler(registers_t *r) {
    int vec = r->vec;

    /* 不再切换 CR3: 任务页表已克隆全部内核 PSE 映射 (supervisor 可
       访问), 中断/syscall 直接在被打断任务的页表下运行 — 消除嵌套
       中断时 CR3 翻转导致的交错 (sys_sleep 旧 sti/hlt 实测 #GP) */
    int from_user = ((r->cs & 3) == 3);

    /* ⚠ 这里**不要**给 ring0(同级) 帧"补" user_esp/user_ss！
       同级帧只有 17 个字/68B (gs..ds, edi..eax, vec, err, eip, cs, eflags)，`user_esp`
       位于偏移 68 —— 那正好是**程序自己的栈顶** (r 是帧起点, r+68 = 中断发生
       时的 ESP)。往那里写 8 字节会踩掉程序当前帧最底部的局部变量 ——
       实测把桌面 `text_cn()` 正在构造的 ioctl 结构体头 8 字节 (p.x/p.y/p.c)
       改坏, 导致 x 变成 0xFFFFF994 的负数 → 文字被画到屏幕外
       (图标名/窗口标题/按钮/右键菜单整体消失)。真正的消费方都已经自己
       按 `(r->cs & 3)` 分支处理了 (见 sys_fork / schedule / task_check_signals)。 */

    void (*h)(void) = isr_mgr.lookup(vec);
    if (h) h();

    if (vec == 0x80) {
        syscall_handler(r);
    }

    if (vec == 0x20) {
        outb(0x20, 0x20);
        /* 唤醒到期的 sys_sleep 任务 (阻塞调度, 替代旧 sti/hlt 忙等) */
        {
            struct list_head *pos;
            u32 now = pit.ticks();
            list_for_each(pos, &all_tasks) {
                struct task_struct *t = container_of(pos, struct task_struct, all_list);
                if (t->state == TASK_BLOCKED && t->wake_tick &&
                    (i32)(now - t->wake_tick) >= 0) {
                    t->wake_tick = 0;
                    t->state = TASK_READY;
                    list_add_tail(&t->list, &ready_queue);
                }
            }
        }
        /* PS/2 FIFO drain @100Hz — GUI/桌面任务不调用 kb_readline,
           鼠标/键盘字节必须由此进入驱动与事件流 (旧版只有 shell
           轮询, GUI 下鼠标死、ESC 失灵)。与 syscall 内的 drain 互斥:
           int 0x80 中断门清 IF, PIT 不在 syscall 中途触发 */
        kb.drain();
        if (kb_ctrl_c()) {
            if (current_task && (r->cs & 3) == 3) {
                task_boost_priority(current_task->pid, 5);
                task_send_signal(current_task->pid, SIGINT);
            }
        }
        /* Mouse cursor update — safe in both kernel and user mode:
           all text output from ring3 goes through int 0x80 syscalls,
           whose interrupt gate (flags 0xEE) clears IF, so PIT cannot
           fire during framebuffer writes. XOR cursor and text output
           are mutually exclusive. */
        gfx.mcursor_update();

        /* 信号投递必须也走 PIT 返回路径: 纯计算死循环 (SPIN.BIN) 从不
           发起 syscall, 而旧实现只在 syscall/异常返回路径调
           task_check_signals → Ctrl+C/SIGKILL 对它完全失效。
           SIG_DFL 的终止类信号会在此 task_do_exit (不返回)。 */
        if (r->cs & 3) task_check_signals(r);

        /* 抢占式调度: ring3 时间片耗尽 (TASK_QUANTUM tick) 即切换。
           守卫 state==TASK_RUNNING: sys_sleep 后任务 BLOCKED, 其用户态
           上下文已由 syscall 路径保存 — 此处再调 schedule 会把 PIT 的
           ISR 帧二次保存进任务, 覆盖用户态上下文 (实测: 任务被恢复成
           "内核态 hlt", 永远回不去 ring3 → GUI 冻结)。
           ring0 内核帧保持旧行为 (每 tick 重调度)。 */
        if (current_task && current_task->state == TASK_RUNNING) {
            int preempt = 0;
            if ((r->cs & 3) == 3 && current_task->pid != 0) {
                if (current_task->quantum > 0) current_task->quantum--;
                if (current_task->quantum == 0) preempt = 1;
            }
            if ((r->cs & 3) == 0 || preempt)
                schedule(r);
        }
        return;
    }

    if (vec == 14) {
        u32 cr2;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));

        /* User-mode page fault: send SIGSEGV, then let signal handler deal */
        if (r->err_code & 4) {
            serial_write_str("user #PF at EIP=0x");
            for (int i = 28; i >= 0; i -= 4)
                serial_write_char("0123456789ABCDEF"[(r->eip >> i) & 15]);
            serial_write_str(" CR2=0x");
            for (int i = 28; i >= 0; i -= 4)
                serial_write_char("0123456789ABCDEF"[(cr2 >> i) & 15]);
            serial_write_str(" SIGSEGV\n");

            if (current_task && current_task->pid != 0) {
                task_send_signal(current_task->pid, SIGSEGV);
            }
        } else {
            /* Kernel-mode page fault: fatal */
            gfx_puts("\n#PF at EIP=0x");
            gfx_put_hex_u32(r->eip);
            gfx_puts(" fault_addr=0x");
            gfx_put_hex_u32(cr2);
            gfx_puts(" err=");
            gfx_put_hex_u32(r->err_code);
            if (r->err_code & 1) gfx_puts(" present");
            else gfx_puts(" not-present");
            if (r->err_code & 2) gfx_puts(" write");
            else gfx_puts(" read");
            if (r->err_code & 4) gfx_puts(" user");
            else gfx_puts(" supervisor");
            if (r->err_code & 8) gfx_puts(" reserved");
            gfx_putc('\n');
            kernel_panic(r, "Page Fault");
        }
    }

    if (vec >= 0x20 && vec <= 0x2F) {
        outb(0x20, 0x20);
        if (vec >= 0x28) outb(0xA0, 0x20);
    }

    if (vec < 0x20) {
        static const char *names[] = {
            "#DE","#DB","NMI","#BP","#OF","#BR","#UD","#NM",
            "#DF","","#TS","#NP","#SS","#GP","#PF","",
            "#MF","#AC","#MC","#XM","#VE"
        };
        const char *n = (vec <= 20 && names[vec][0]) ? names[vec] : "?";
        kernel_panic(r, n);
    }

    if (from_user) {
        /* Check and deliver pending signals before returning to user */
        task_check_signals(r);
    }
}
