#include "kernel/user.h"
#include "kernel/paging.h"
#include "kernel/mm.h"
#include "lib/ports.h"
#include "drivers/serial.h"
#include "kernel/task.h"

static u32 tss_page;
PagingManager *g_user_pd = nullptr;
u32 g_entry_esp = 0;
char g_user_args[256];

/* 默认启用 Ring3 中断与时间片抢占；false 仅用于 VMware 诊断。 */
bool g_ring3_irq_on = true;

/* 默认进入 Ring3；true 仅用于 VMware Ring0 兼容/演示。 */
bool g_ring0_mode = false;

void user_tss_set_esp0(u32 esp0) {
    u8 *tss = (u8 *)tss_page;
    *(u32 *)(tss + 4) = esp0;
}

void user_init(void) {
    tss_page = mm_alloc_page();
    u8 *tss = (u8 *)tss_page;
    for (int i = 0; i < 104; i++) tss[i] = 0;
    *(u32 *)(tss + 4)  = 0x9F000;
    *(u32 *)(tss + 8)  = 0x10;
    /* I/O 位图基址必须 >= TSS 限制(103) 才表示"禁止 ring3 端口 I/O";
       置 0 会被解释成"位图从 TSS 偏移 0 开始", 属于未定义摆法 */
    *(u16 *)(tss + 102) = 104;

    struct { u16 limit; u32 base; } __attribute__((packed)) gdtr;
    __asm__ volatile("sgdt %0" : "=m"(gdtr));
    u8 *gdt = (u8 *)gdtr.base;
    int idx = 6;
    u32 base = tss_page;
    gdt[idx*8 + 2] = base & 0xFF;
    gdt[idx*8 + 3] = (base >> 8) & 0xFF;
    gdt[idx*8 + 4] = (base >> 16) & 0xFF;
    gdt[idx*8 + 7] = (base >> 24) & 0xFF;
    __asm__ volatile("ltr %%ax" : : "a"(0x30));
}

void enter_user_mode(u32 entry, u32 stack_top, PagingManager *pd,
                     int argc, const char *args) {
    __asm__ volatile("mov %%ebp, %0" : "=m"(g_entry_esp));

    if (args && args[0]) {
        /* Copy args into global buffer (accessible from user CR3) */
        int i = 0;
        while (args[i] && i < 255) { g_user_args[i] = args[i]; i++; }
        g_user_args[i] = 0;
    } else {
        g_user_args[0] = 0;
    }

    if (pd) {
        g_user_pd = pd;
        pd->load();
        __asm__ volatile("wbinvd");
    }

    /* 每任务独立内核栈 */
    u32 esp0_val = (current_task && current_task->kernel_stack)
                 ? current_task->kernel_stack + KSTACK_SIZE : 0;
    if (esp0_val)
        user_tss_set_esp0(esp0_val);
    else
        serial_write_str("enter_user: WARN no kernel stack (TSS ESP0 stale!\n");

    /* 进 ring3 前的现场记录: 若紧跟其后 triple fault, 这几行就是最后线索 */
    serial_write_str("enter_user: cr3=0x");
    { u32 cr3; __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
      serial_write_u32(cr3); }
    serial_write_str(" esp0=0x");
    serial_write_u32(esp0_val);
    serial_write_str(" entry=0x");
    serial_write_u32(entry);
    serial_write_str(" ustack=0x");
    serial_write_u32(stack_top);
    serial_write_char('\n');

    /* ================= iret 前完整取证转储 =================
       这一行之后如果 VM 直接断电 (VMware "virtual CPU shutdown state"),
       下面这份转储就是全部线索。它把真 MMU 在 iret 里要做/会检查的东西
       全部显式打出来:
         1) CR0/CR4/EFLAGS/TR 与 GDTR/IDTR 的 base+limit
         2) GDT 里 SS(0x23)/CS(0x2B)/TSS(0x30) 三个描述符的原始 8 字节
            (旧排查只验过 TSS 描述符, 从没验过 4/5)
         3) 在**当前用户页目录** (CR3 已切换) 下, EIP / 用户栈 / 内核栈 /
            GDT / IDT 的 VA→PA 解析 + present/4MB/保留位情况
       ====================================================== */
    {
        struct { u16 limit; u32 base; } __attribute__((packed)) gdtr, idtr;
        __asm__ volatile("sgdt %0" : "=m"(gdtr));
        __asm__ volatile("sidt %0" : "=m"(idtr));
        u16 tr; u32 cr0, cr4, esp_now, efl_now;
        __asm__ volatile("str %0" : "=r"(tr));
        __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
        __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
        __asm__ volatile("mov %%esp, %0" : "=r"(esp_now));
        __asm__ volatile("pushf; pop %0" : "=r"(efl_now));

        serial_write_str("pre-iret: esp=0x");   serial_write_u32(esp_now);
        serial_write_str(" cr0=0x");            serial_write_u32(cr0);
        serial_write_str(" cr4=0x");            serial_write_u32(cr4);
        serial_write_str(" eflags=0x");         serial_write_u32(efl_now);
        serial_write_char('\n');
        serial_write_str("pre-iret: gdtr=0x");  serial_write_u32(gdtr.base);
        serial_write_str(" lim=0x");            serial_write_u32((u32)gdtr.limit);
        serial_write_str(" idtr=0x");           serial_write_u32(idtr.base);
        serial_write_str(" lim=0x");            serial_write_u32((u32)idtr.limit);
        serial_write_str(" tr=0x");             serial_write_u32((u32)tr);
        serial_write_str(" tss=0x");            serial_write_u32(tss_page);
        serial_write_char('\n');

        const u8 *g = (const u8 *)gdtr.base;
        serial_write_str("pre-iret: GDT[4] SS/0x23 = ");
        for (int k = 0; k < 8; k++) {
            serial_write_char("0123456789ABCDEF"[g[32 + k] >> 4]);
            serial_write_char("0123456789ABCDEF"[g[32 + k] & 15]);
        }
        serial_write_char('\n');
        serial_write_str("pre-iret: GDT[5] CS/0x2B = ");
        for (int k = 0; k < 8; k++) {
            serial_write_char("0123456789ABCDEF"[g[40 + k] >> 4]);
            serial_write_char("0123456789ABCDEF"[g[40 + k] & 15]);
        }
        serial_write_char('\n');
        serial_write_str("pre-iret: GDT[6] TSS/0x30= ");
        for (int k = 0; k < 8; k++) {
            serial_write_char("0123456789ABCDEF"[g[48 + k] >> 4]);
            serial_write_char("0123456789ABCDEF"[g[48 + k] & 15]);
        }
        serial_write_char('\n');

        /* 用户态专用: 走一遍当前 CR3 的页表 (真 MMU 的动作) */
        {
            u32 cr3;
            __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
            const u32 *pd = (const u32 *)cr3;
            struct { const char *tag; u32 va; } t[] = {
                {"EIP  0x00400000", entry},
                {"USTK 0x00440000", stack_top},
                {"KSTK(当前 esp)", esp_now},
                {"GDT ", gdtr.base},
                {"IDT ", idtr.base},
                {"TSS ", tss_page},
                /* ring3 触发中断/异常时, CPU 会切到 TSS ESP0 指定的内核栈。
                   这一条以前从没验过 —— 若它没映射, ring3 的第一次 int 0x80
                   会在**特权栈切换**里 #PF, 而 #DF 又要往同一个坏栈压帧
                   → 零输出的三重故障 (QEMU 与 VMware 的观感差异正在这里)。 */
                {"ESP0(TSS栈顶)", esp0_val},
            };
            for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
                u32 va = t[i].va;
                u32 pde = pd[va >> 22];
                serial_write_str("pre-iret: map ");
                serial_write_str(t[i].tag);
                serial_write_str(" pde=0x"); serial_write_u32(pde);
                if (!(pde & 1)) {
                    serial_write_str("  !!PDE-NOT-PRESENT\n");
                } else if (pde & 0x80) {
                    serial_write_str(" PA=0x");
                    serial_write_u32((pde & 0xFFC00000) + (va & 0x3FFFFF));
                    serial_write_str(" (4MB)\n");
                } else {
                    const u32 *pt = (const u32 *)(pde & 0xFFFFF000);
                    u32 pte = pt[(va >> 12) & 0x3FF];
                    serial_write_str(" pte=0x"); serial_write_u32(pte);
                    if (!(pte & 1)) serial_write_str("  !!PTE-NOT-PRESENT\n");
                    else {
                        serial_write_str(" PA=0x");
                        serial_write_u32((pte & 0xFFFFF000) + (va & 0xFFF));
                        serial_write_char('\n');
                    }
                }
            }
        }
    }

    /* ---- Ring0 兼容模式: 用远跳转代替 iret ----
       程序以 CS=0x18 (ring0) 运行, 因此后续 `int 0x80` 不发生特权级切换,
       也就不会走 VMware 上失败的那条 TSS 特权栈切换路径。
       EFLAGS 沿用当前值 (IF 按 g_ring3_irq_on, 默认关)。 */
    if (g_ring0_mode) {
        serial_write_str("pre-iret: RING0 兼容模式 entry=0x");
        serial_write_u32(entry);
        serial_write_str(" CS=0x18 (int 0x80 不切栈)\n");
        struct { u32 off; u16 sel; } __attribute__((packed)) fj;
        fj.off = entry;
        fj.sel = 0x18;
        __asm__ volatile(
            "movl %0, %%esp\n\t"
            "movl %1, %%eax\n\t"
            "ljmpl *(%%eax)\n\t"
            :
            : "r"(stack_top), "r"((u32)&fj)
            : "eax"
        );
        __builtin_unreachable();
    }

    /* ---- 进 ring3 的 EFLAGS: IF 由 g_ring3_irq_on 决定 ----
       旧实现是 `pushf` + `orl $0x200,(%esp)` — 硬开 IF, 没法关。
       现在显式算好再压栈, 并在串口留一行, 便于事后判断走的是哪条路。 */
    u32 efl;
    __asm__ volatile("pushf; pop %0" : "=r"(efl));
    if (g_ring3_irq_on) efl |= 0x200u;
    else                efl &= ~0x200u;
    serial_write_str("pre-iret: ring3 IF=");
    serial_write_str(g_ring3_irq_on ? "1 (中断开)\n" : "0 (中断关)\n");

    if (argc > 0 && g_user_args[0]) {
        /* 把参数字符串本体先拷到用户栈顶下方, 再压 argv 指针数组。
           旧实现把内核 g_user_args 的地址直接作为 argv — 用户页表
           无此映射 (且无 USER 位), 程序访问 argv 必 #PF。
           用户栈为恒等映射 (VA==PA), 内核经 PSE 可直接写。 */
        u32 sp = stack_top;

        /* 1. 自底向上逐字拷贝参数串 (原地分割为 NUL 结尾) */
        u32 str_addrs[16];
        int ac = 0;
        char *argp = g_user_args;
        while (*argp && ac < 16) {
            while (*argp == ' ') argp++;
            if (!*argp) break;
            str_addrs[ac++] = sp;   /* 该串在用户栈上的地址 */
            while (*argp && *argp != ' ')
                *(char *)(sp++) = *argp++;
            *(char *)(sp++) = 0;
        }

        /* 2. 对齐后压 argv 数组 + NULL + argc */
        sp &= ~3u;
        u32 *stk = (u32 *)sp;
        *(--stk) = 0;                       /* argv[ac] = NULL */
        for (int i = ac - 1; i >= 0; i--)
            *(--stk) = str_addrs[i];        /* argv[i] */
        *(--stk) = (u32)ac;                 /* argc */
        if (current_task)
            current_task->user_esp = (u32)stk;   /* 调度恢复时用真实 esp */

        __asm__ volatile(
            "pushl $0x23\n"          /* SS  (用户数据选择子) */
            "pushl %[usp]\n"         /* ESP (argc 所在地址) */
            "pushl %[efl]\n"         /* EFLAGS (IF 按 g_ring3_irq_on) */
            "pushl $0x2B\n"          /* CS  (用户代码选择子) */
            "pushl %[eip]\n"         /* EIP */
            "movw $0x23, %%ax\n"
            "movw %%ax, %%ds\n"
            "movw %%ax, %%es\n"
            "movw %%ax, %%fs\n"
            "movw %%ax, %%gs\n"
            "iret\n"
            :
            : [usp] "r"((u32)stk), [efl] "r"(efl), [eip] "r"(entry)
            : "eax", "memory"
        );
    } else {
        __asm__ volatile(
            "pushl $0x23\n"          /* SS */
            "pushl %[usp]\n"         /* ESP — 用户栈顶 (来自 stack_top 参数) */
            "pushl %[efl]\n"         /* EFLAGS (IF 按 g_ring3_irq_on) */
            "pushl $0x2B\n"          /* CS */
            "pushl %[eip]\n"         /* EIP */
            "movw $0x23, %%ax\n"
            "movw %%ax, %%ds\n"
            "movw %%ax, %%es\n"
            "movw %%ax, %%fs\n"
            "movw %%ax, %%gs\n"
            "iret\n"
            :
            : [usp] "r"(stack_top), [efl] "r"(efl), [eip] "r"(entry)
            : "eax", "memory"
        );
    }
    __builtin_unreachable();
}
