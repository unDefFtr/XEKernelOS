#include "kernel/syscall.h"
#include "kernel/user.h"
#include "kernel/paging.h"
#include "kernel/mm.h"
#include "kernel/task.h"
#include "drivers/serial.h"
#include "drivers/keyboard.h"
#include "drivers/gfx.h"
#include "drivers/mouse.h"
#include "drivers/pit.h"
#include "drivers/ata.h"
#include "drivers/bcache.h"
#include "drivers/input.h"
#include "fs/fat12.h"
#include "fs/vfs.h"
#include "fs/ramdisk.h"
#include "lib/heap.h"
#include "lib/ports.h"
#include "lib/uaccess.h"

#define PIPE_BUF_SZ 4096

/* Pipe ring buffer shared by read/write FDs */
struct pipe_t {
    u8   buf[PIPE_BUF_SZ];
    u32  rpos;    /* read position */
    u32  wpos;    /* write position */
    u32  count;   /* bytes available to read */
    int  refs;    /* reference count (2 when both ends open) */
    bool broken;  /* one end closed → pipe broken */
};

/* Heap break — starts at 0x10000000 (PDE 64, clear of kernel PDEs) */
static u32 user_break = 0x10000000;

/* syscall 期间 IF 关闭 (中断门), 无重入 — 内核侧暂存缓冲用 static 避免撑爆 4KB 内核栈 */
static char g_ustr[4096];
static u8   g_ubin[4096];

/* Per-task output redirect via task_struct.output_fd (准则一) */

static void sys_write(registers_t *r) {
    u32 str = r->ebx;
    u32 len = r->ecx;
    if (len > 4096) { r->eax = (u32)-1; return; }
    if (!copy_from_user(g_ustr, str, len)) { r->eax = (u32)-1; return; }
    serial_write_str_len(g_ustr, len);
    serial_write_char('\n');
    r->eax = len;
}

static void sys_fwrite(registers_t *r) {
    u32 fd  = r->ebx;
    u32 str = r->ecx;
    u32 len = r->edx;
    if (len > 4096) len = 4096;

    if (fd >= MAX_FD || !current_task->fd_buf[fd]) { r->eax = (u32)-1; return; }

    u8 typ = current_task->fd_type[fd];

    /* null/zero 设备: 不接触用户缓冲, 直接丢弃 */
    if (typ == 5 || typ == 6) { r->eax = len; return; }

    if (!copy_from_user(g_ubin, str, len)) { r->eax = (u32)-1; return; }
    const u8 *data = g_ubin;

    if (typ == 3) {  /* pipe write-end */
        pipe_t *pipe = (pipe_t *)current_task->fd_buf[fd];
        if (pipe->broken) { r->eax = (u32)-1; return; }  /* 准则二: broken pipe */
        u32 avail = PIPE_BUF_SZ - pipe->count;
        if (len > avail) len = avail;
        if (len == 0) { r->eax = 0; return; }
        for (u32 i = 0; i < len; i++) {
            pipe->buf[pipe->wpos] = data[i];
            pipe->wpos = (pipe->wpos + 1) % PIPE_BUF_SZ;
        }
        pipe->count += len;
        r->eax = len;
        return;
    }

    if (typ == 1) {  /* file: append — check CAP_FILE_WRITE */
        if (current_task && !(current_task->caps & CAP_FILE_WRITE)) {
            r->eax = (u32)-1; return;
        }
        u32 space = current_task->fd_size[fd] - current_task->fd_pos[fd];
        if (len > space) len = space;
        if (len == 0) { r->eax = 0; return; }
        for (u32 i = 0; i < len; i++)
            current_task->fd_buf[fd][current_task->fd_pos[fd] + i] = data[i];
        current_task->fd_pos[fd] += len;
        r->eax = len;
        return;
    }

    if (typ == 4) {  /* framebuffer stdout (准则一) */
        g_ubin[len] = 0;
        serial_write_str_len((const char *)g_ubin, len);  /* 调试镜像 */
        gfx.puts_utf8((const char *)g_ubin);
        r->eax = len;
        return;
    }

    r->eax = (u32)-1;
}

static void sys_read(registers_t *r) {
    /* 准则一: SYS_READ = keyboard readline only.
       FD-based reads (files & pipes) go through SYS_FREAD. */
    u32 buf = r->ebx;
    int max = (int)r->ecx;
    if (max <= 0 || max > 4096) { r->eax = 0; return; }
    kb_readline(g_ustr, max - 1);
    g_ustr[max - 1] = 0;
    int n = 0; while (g_ustr[n]) n++;
    /* 调试: 串口回显内核收到的命令行, 区分"输入丢键"与"命令执行失败" */
    serial_write_str("[READ] \"");
    serial_write_str_len(g_ustr, n);
    serial_write_str("\" len=");
    serial_write_u32((u32)n);
    serial_write_char('\n');
    if (!copy_to_user(buf, g_ustr, (u32)n + 1)) { r->eax = (u32)-1; return; }
    r->eax = n;
}

static void sys_open(registers_t *r) {
    char name[128];
    if (!copy_str_from_user(name, r->ebx, sizeof(name))) { r->eax = (u32)-1; return; }

    /* Find free fd slot */
    int fd = -1;
    for (int i = 0; i < MAX_FD; i++) {
        if (!current_task->fd_buf[i]) { fd = i; break; }
    }
    if (fd < 0) { r->eax = (u32)-1; return; }

    /* Device nodes (准则一: everything is an fd) */
    auto str_eq = [](const char *a, const char *b) -> bool {
        while (*a && *b && *a == *b) { a++; b++; }
        return *a == *b;
    };

    if (str_eq(name, "/dev/null")) {
        current_task->fd_buf[fd] = (u8 *)1;
        current_task->fd_type[fd] = 5;  /* null device */
        current_task->fd_size[fd] = 0;
        current_task->fd_pos[fd] = 0;
        r->eax = fd;
        return;
    }
    if (str_eq(name, "/dev/zero")) {
        current_task->fd_buf[fd] = (u8 *)1;
        current_task->fd_type[fd] = 6;  /* zero device */
        current_task->fd_size[fd] = ~0u;
        current_task->fd_pos[fd] = 0;
        r->eax = fd;
        return;
    }
    if (str_eq(name, "/dev/input")) {
        /* 准则一: 输入设备也是 fd — 键盘/鼠标统一事件流,
           GUI 程序经 SYS_FREAD 逐事件读取 */
        current_task->fd_buf[fd] = (u8 *)1;
        current_task->fd_type[fd] = 7;  /* input device */
        current_task->fd_size[fd] = ~0u;
        current_task->fd_pos[fd] = 0;
        r->eax = fd;
        return;
    }

    /* 准则四: file read requires CAP_FILE_READ */
    if (current_task && !(current_task->caps & CAP_FILE_READ)) {
        r->eax = (u32)-1; return;
    }

    /* 先 stat 拿文件大小, 按需分配缓冲 — 旧实现固定 64KB,
       fd 表占满可达 1MB; 文件不存在时也不再白分配 */
    int is_dir = 0;
    int fsize = vfs_stat(name, &is_dir);
    if (fsize < 0) {
        serial_write_str("open: not found: ");
        serial_write_str(name);
        serial_write_char('\n');
        r->eax = (u32)-1; return;
    }
    u32 alloc = ((u32)fsize + 0xFFF) & ~0xFFFu;   /* 4KB 对齐 */
    if (alloc < 4096) alloc = 4096;
    if (alloc > 65536) alloc = 65536;

    u8 *fb = (u8 *)kmalloc(alloc);
    if (!fb) { r->eax = (u32)-1; return; }

    int sz = vfs_open(name, fb, alloc);
    if (sz <= 0) { kfree(fb); r->eax = (u32)-1; return; }

    current_task->fd_buf[fd] = fb;
    current_task->fd_size[fd] = (u32)sz;
    current_task->fd_pos[fd] = 0;
    current_task->fd_type[fd] = 1;  /* file */
    r->eax = fd;
}

static void sys_fread(registers_t *r) {
    u32 fd = r->ebx;
    u32 buf = r->ecx;
    u32 len = r->edx;

    if (fd >= MAX_FD || !current_task->fd_buf[fd]) { r->eax = (u32)-1; return; }

    /* Device fds */
    if (current_task->fd_type[fd] == 5) {  /* /dev/null: always EOF */
        r->eax = 0; return;
    }
    if (current_task->fd_type[fd] == 6) {  /* /dev/zero: fill with zeros */
        if (len > 4096) len = 4096;
        for (u32 i = 0; i < len; i++) g_ubin[i] = 0;
        if (!copy_to_user(buf, g_ubin, len)) { r->eax = (u32)-1; return; }
        r->eax = len; return;
    }
    if (current_task->fd_type[fd] == 7) {  /* /dev/input: 弹一个事件 */
        if (len < sizeof(input_event)) { r->eax = (u32)-1; return; }
        input_event ev;
        int n = input_pop(&ev);
        if (n == 0) { r->eax = 0; return; }   /* 空: GUI 循环重试 */
        if (!copy_to_user(buf, &ev, sizeof(ev))) { r->eax = (u32)-1; return; }
        r->eax = (u32)n;
        return;
    }

    if (len > 4096) len = 4096;

    /* 准则一: unified FD read — handles files AND pipes */
    if (current_task->fd_type[fd] == 2) {  /* pipe read-end */
        pipe_t *pipe = (pipe_t *)current_task->fd_buf[fd];
        u32 n = pipe->count;
        if (n > len) n = len;
        if (n == 0) {
            r->eax = pipe->broken ? (u32)-1 : 0;
            return;
        }
        for (u32 i = 0; i < n; i++) {
            g_ubin[i] = pipe->buf[pipe->rpos];
            pipe->rpos = (pipe->rpos + 1) % PIPE_BUF_SZ;
        }
        pipe->count -= n;
        if (!copy_to_user(buf, g_ubin, n)) { r->eax = (u32)-1; return; }
        r->eax = n;
        return;
    }

    /* File read */
    if (current_task && current_task->fd_type[fd] == 1 &&
        !(current_task->caps & CAP_FILE_READ)) { r->eax = (u32)-1; return; }
    u32 remain = current_task->fd_size[fd] - current_task->fd_pos[fd];
    if (len > remain) len = remain;
    for (u32 i = 0; i < len; i++)
        g_ubin[i] = current_task->fd_buf[fd][current_task->fd_pos[fd] + i];
    if (len && !copy_to_user(buf, g_ubin, len)) { r->eax = (u32)-1; return; }
    current_task->fd_pos[fd] += len;
    r->eax = len;
}

static void sys_sbrk(registers_t *r) {
    u32 bytes = r->ebx;
    if (bytes == 0) { r->eax = user_break; return; }

    u32 pages = (bytes + 0xFFF) / 0x1000;
    u32 old_break = user_break;

    /* 用当前任务的页表 (而非全局 g_user_pd):
       fork 后子进程有自己的 PagingManager, 若仍映射到父进程页表,
       子进程 sbrk 的页会落入父进程地址空间 → 子进程缺页/堆错乱 */
    PagingManager *pd = (current_task && current_task->paging)
                        ? current_task->paging : g_user_pd;
    if (!pd) { r->eax = (u32)-1; return; }

    for (u32 i = 0; i < pages; i++) {
        u32 phys = mm_alloc_page();
        if (!phys) { r->eax = (u32)-1; return; }
        pd->map_page(user_break, phys, PT_FLAGS);
        pd->track_owned(phys);   /* 任务销毁时随页目录统一释放 */
        user_break += 0x1000;
    }

    r->eax = old_break;
}

static void sys_getcwd(registers_t *r) {
    u32 buf = r->ebx;
    int max = (int)r->ecx;
    if (max <= 0 || max > 256) { r->eax = (u32)-1; return; }
    fat.cwd_str(g_ustr, max);
    int n = 0; while (g_ustr[n]) n++;
    if (!copy_to_user(buf, g_ustr, (u32)n + 1)) { r->eax = (u32)-1; return; }
    r->eax = n;
}

static void sys_time(registers_t *r) {
    u32 buf = r->ebx;
    auto bcd = [](u8 v) -> u8 { return ((v >> 4) & 0x0F) * 10 + (v & 0x0F); };
    outb(0x70, 0x04); u8 h = bcd(inb(0x71));
    outb(0x70, 0x02); u8 m = bcd(inb(0x71));
    outb(0x70, 0x00); u8 s = bcd(inb(0x71));
    char tb[9];
    tb[0] = '0' + (h / 10); tb[1] = '0' + (h % 10); tb[2] = ':';
    tb[3] = '0' + (m / 10); tb[4] = '0' + (m % 10); tb[5] = ':';
    tb[6] = '0' + (s / 10); tb[7] = '0' + (s % 10); tb[8] = 0;
    if (!copy_to_user(buf, tb, 9)) { r->eax = (u32)-1; return; }
    r->eax = 8;
}

/* ---- FAT filesystem syscalls for user-space shell ---- */

static void sys_fat_dir(registers_t *r) {
    fat.dir();
    r->eax = 0;
}

static void sys_fat_cd(registers_t *r) {
    char name[128];
    if (!copy_str_from_user(name, r->ebx, sizeof(name))) { r->eax = (u32)-1; return; }
    r->eax = fat.cd(name);
}

static void sys_fat_mkdir(registers_t *r) {
    char name[128];
    if (!copy_str_from_user(name, r->ebx, sizeof(name))) { r->eax = (u32)-1; return; }
    r->eax = vfs_mkdir(name);
}

static void sys_fat_rmdir(registers_t *r) {
    char name[128];
    if (!copy_str_from_user(name, r->ebx, sizeof(name))) { r->eax = (u32)-1; return; }
    r->eax = vfs_rmdir(name);
}

static void sys_fat_delete(registers_t *r) {
    char name[128];
    if (!copy_str_from_user(name, r->ebx, sizeof(name))) { r->eax = (u32)-1; return; }
    r->eax = vfs_remove(name);
}

static void sys_fat_rename(registers_t *r) {
    char old_name[128], new_name[128];
    if (!copy_str_from_user(old_name, r->ebx, sizeof(old_name)) ||
        !copy_str_from_user(new_name, r->ecx, sizeof(new_name))) {
        r->eax = (u32)-1; return;
    }
    r->eax = vfs_rename(old_name, new_name);
}

static void sys_fat_write(registers_t *r) {
    char name[128];
    u32 size = r->edx;
    if (size > 4096) { r->eax = (u32)-1; return; }
    if (!copy_str_from_user(name, r->ebx, sizeof(name)) ||
        !copy_from_user(g_ubin, r->ecx, size)) { r->eax = (u32)-1; return; }
    r->eax = vfs_write(name, g_ubin, size);
}

/* ---- fork: clone current task with copied address space ---- */

static void sys_fork(registers_t *r) {
    /* ring0 兼容模式: CPL0 下 `iretd` 不会弹出 SS/ESP, 而本内核的任务切换
       (schedule 原地改帧 → common_isr 的 popa/add 8/iretd) 正是靠 ring3 帧里
       的 SS/ESP 来恢复目标任务的应用栈。同特权帧没有这两个字 → 恢复后 ESP
       落在目标任务的内核栈上 → 子进程必然跑飞 (实测 EIP 落到 0x43FE9F)。
       这是该模式的结构性限制, 因此直接拒绝: 让 shell 报错, 而不是把 VM 打死。 */
    if (g_ring0_mode) {
        serial_write_str("fork: ring0 兼容模式下不支持 (返回 -1)\n");
        r->eax = (u32)-1;
        return;
    }
    if (!current_task || current_task->pid == 0) {
        r->eax = (u32)-1;
        return;
    }

    PagingManager *parent_pd = current_task->paging;
    if (!parent_pd) { r->eax = (u32)-1; return; }

    PagingManager *child_pd = new PagingManager();

    /* Copy user-space page tables (PDE 1+, 用户程序在 0x400000 = PDE[1]!),
       skipping kernel PSE pages.
       旧实现从 PDE[32] 起 — PG 未开启年代 PDE[1] 的缺失被纯段式恒等
       掩盖; PG 开启后 child PDE[1] 仍是克隆的内核 PSE (无 USER),
       ring3 恢复到 fork 返回点取指即 #PF (实测 0x4019CA)。 */
    for (int pde = 1; pde < 1024; pde++) {
        u32 src_pde = parent_pd->get_pde(pde);
        if (!(src_pde & 1)) continue;
        if (src_pde & 0x80) {
            /* 用户态 4MB PSE 页 (如显存映射): 直接共享同一物理大页 */
            if (src_pde & 0x04)
                child_pd->set_pde(pde, src_pde);
            continue;
        }
        u32 *src_pt = (u32 *)(src_pde & 0xFFFFF000);
        for (int pte = 0; pte < 1024; pte++) {
            u32 entry = src_pt[pte];
            if (!(entry & 1)) continue;
            u32 new_phys = mm_alloc_page();
            if (!new_phys) { delete child_pd; r->eax = (u32)-1; return; }
            u32 *s = (u32 *)(entry & 0xFFFFF000), *d = (u32 *)new_phys;
            for (int k = 0; k < 1024; k++) d[k] = s[k];
            child_pd->map_page((pde << 22) | (pte << 12), new_phys, entry & 0xFFF);
            /* 登记为子进程拥有的数据页 — 失败回滚/正常回收都释放 */
            child_pd->track_owned(new_phys);
        }
    }

    task_struct *child = (task_struct *)kmalloc(sizeof(task_struct));
    u32 *kstack = (u32 *)kmalloc(KSTACK_SIZE);
    if (!child || !kstack) {
        if (child)   kfree(child);
        if (kstack)  kfree(kstack);
        delete child_pd;   /* owned 页随析构统一释放, 无泄漏 */
        r->eax = (u32)-1;
        return;
    }

    child->pid   = task_next_pid();
    child->ecx   = r->ecx;  child->edx   = r->edx;
    child->ebx   = r->ebx;  child->ebp   = r->ebp;
    child->esi   = r->esi;  child->edi   = r->edi;
    child->eax   = 0;   /* fork 子进程首次返回 0 (通过 schedule 恢复) */
    child->gs = r->gs; child->fs = r->fs;
    child->es = r->es; child->ds = r->ds;
    child->eip   = r->eip;  child->cs    = r->cs;
    child->eflags = r->eflags;
    child->user_esp = (r->cs & 3) ? r->user_esp : current_task->user_esp;
    child->user_ss  = 0x23;
    child->state = TASK_READY;
    child->kernel_stack = (u32)kstack;
    child->entry = nullptr;  child->arg = nullptr;
    child->paging = child_pd;
    child->user_stack = current_task->user_stack;
    child->parent = current_task;
    child->exit_code = 0;
    child->pending_signals = 0;
    child->blocked_signals = 0;
    for (int i = 0; i < 32; i++) child->sig_handlers[i] = 0;
    child->sig_saved_eip = 0;
    child->sig_saved_esp = 0;
    child->caps = current_task->caps;
    child->output_fd = -1;
    child->priority = current_task->priority;
    child->dynamic_boost = 0;
    child->boost_expire = 0;
    child->wake_tick = 0;
    child->quantum = TASK_QUANTUM;
    for (int i = 0; i < MAX_FD; i++) {
        child->fd_size[i] = current_task->fd_size[i];
        child->fd_pos[i]  = current_task->fd_pos[i];
        child->fd_type[i] = current_task->fd_type[i];
        child->fd_buf[i]  = current_task->fd_buf[i];

        if (child->fd_type[i] == 1 && child->fd_buf[i]) {
            /* 文件 fd: 深拷贝缓冲 — 父子共享指针会在双方 close 时双 free */
            u8 *nb = (u8 *)kmalloc(current_task->fd_size[i] ?
                                   ((current_task->fd_size[i] + 0xFFF) & ~0xFFFu) : 4096);
            if (!nb) {
                /* 回滚: 已深拷贝的 fd 与管道引用计数 */
                for (int j = 0; j < i; j++) {
                    if (child->fd_type[j] == 1 && child->fd_buf[j]) kfree(child->fd_buf[j]);
                    else if ((child->fd_type[j] == 2 || child->fd_type[j] == 3) && child->fd_buf[j])
                        ((pipe_t *)child->fd_buf[j])->refs--;
                }
                kfree(child); kfree(kstack);
                delete child_pd;
                r->eax = (u32)-1;
                return;
            }
            /* 拷贝整个已加载内容 (vfs_open 填满到 fd_size) —
               子进程 exec_fd 依赖完整文件数据 */
            u32 blen = current_task->fd_size[i];
            for (u32 k = 0; k < blen; k++) nb[k] = child->fd_buf[i][k];
            child->fd_buf[i] = nb;
        }
        /* 管道 fd 共享同一 pipe_t: 引用计数 +1, 防止父子任一关闭时提前 kfree */
        if ((child->fd_type[i] == 2 || child->fd_type[i] == 3) && child->fd_buf[i]) {
            pipe_t *p = (pipe_t *)child->fd_buf[i];
            p->refs++;
        }
    }
    list_init(&child->children);
    list_add_tail(&child->sibling, &current_task->children);
    list_add_tail(&child->list, &ready_queue);
    list_add_tail(&child->all_list, &all_tasks);

    /* 帧长取决于是否发生特权切换: ring3 帧含 user_esp/user_ss, Ring0 不含；
       目标空间始终预留完整的用户帧大小。 */
    u32 frame_bytes = (r->cs & 3) ? REGISTER_FRAME_USER_BYTES : REGISTER_FRAME_KERNEL_BYTES;
    u32 *csp = (u32 *)(child->kernel_stack + KSTACK_SIZE);
    u32 *dst = csp - (REGISTER_FRAME_USER_BYTES / sizeof(u32));
    for (u32 i = 0; i < frame_bytes / sizeof(u32); i++)
        dst[i] = ((u32 *)r)[i];

    registers_t *cr = (registers_t *)dst;
    cr->_esp = (u32)&cr->vec;
    cr->eax = 0;                        /* fork 子进程首次返回 0 */
    if (!(r->cs & 3)) {
        cr->user_esp = child->user_esp;
        cr->user_ss  = child->user_ss;
    }
    child->esp = (u32)&cr->vec;

    serial_write_str("fork: child pid ");
    serial_write_u32(child->pid);
    serial_write_str(" eip=0x"); serial_write_u32(child->eip);
    serial_write_str(" cs=0x");  serial_write_u32(child->cs);
    serial_write_str(" esp=0x"); serial_write_u32(child->esp);
    serial_write_str(" frame=0x"); serial_write_u32((u32)cr);
    serial_write_char('\n');
    r->eax = child->pid;
}

/* exec 公共路径: 用平坦二进制替换当前任务地址空间。
   修复三件事:
   1) 恒等映射 (VA==PA) — 与 loader 一致; 旧实现 mm_alloc_page 的
      随机物理页 + 用户态 syscall 传指针 → 内核经 PSE 解引用读到旧镜像
   2) 先完整构建新地址空间再切换 — 旧实现先 delete 旧页目录,
      中途失败 (页表 OOM) 任务回到已销毁的地址空间 → #PF 崩溃
   3) 0x400000~0x450000 物理区由 mm 统一预留 (loader 恒等映射路径用,
      不经 mm_alloc_page), exec 自身只用 mm 私有页, 不产生不可回收的页 */
static bool exec_replace_address_space(registers_t *r, const u8 *data, u32 sz) {
    u32 load_addr = 0x400000;
    u32 entry     = 0x400000;   /* 调试结束恢复正位 */
    /* 栈顶 0x440000 (与 loader/shell_launch_user 一致): 代码+.bss 区
       可用 0x400000~0x430000。旧值 0x420000 只剩 128KB, 桌面
       (代码 39KB + .bss 60KB) 的 wins 越界落进栈区 */
    u32 stack_top = 0x440000;
    u32 stack_base = stack_top - 0x10000;

    PagingManager *old_pd = current_task->paging;
    PagingManager *new_pd = new PagingManager();

    /* 1. 代码/栈各分配 mm 私有物理页 (不再恒等映射)。
          旧实现 VA 0x400000 → PA 0x400000, exec 拷贝代码直接覆盖
          物理恒等区 — 而 fork 出的父进程 (如 ushell RUN DESKTOP) 的
          代码/栈就在同一物理页! 子进程 exec 踩毁父进程内存, 父进程
          waitpid 恢复后执行被覆盖字节 → #GP (实测 EIP=0x4006A9)。
          gfxdemo(474B) 只踩文件开头侥幸不炸, desktop(2588B) 必炸。
          mm 私有页经 track_owned 登记, 退出/失败随页目录统一释放 */
    u32 map_sz = (sz < 0x1000) ? 0x1000 : ((sz + 0xFFF) & ~0xFFFu);
    map_sz += 0x10000;   /* .bss 预留 64KB — 桌面等大全局数组需额外虚址 */
    /* 代码+.bss 区不得与用户栈重叠: 重叠时栈生长会踩坏全局数组,
       且拷贝循环会经 translate_user 把 .bss 数据写进栈物理页 */
    if (map_sz > stack_base - load_addr) {
        serial_write_str("exec: program too large\n");
        delete new_pd;
        return false;
    }
    for (u32 off = 0; off < map_sz; off += 0x1000) {
        u32 pa = mm_alloc_page();
        if (!pa) { delete new_pd; return false; }
        new_pd->map_page(load_addr + off, pa, PT_FLAGS);
        new_pd->track_owned(pa);
    }
    for (u32 va = stack_base; va < stack_top; va += 0x1000) {
        u32 pa = mm_alloc_page();
        if (!pa) { delete new_pd; return false; }
        new_pd->map_page(va, pa, PT_FLAGS);
        new_pd->track_owned(pa);
    }

    /* 显存 4MB 用户映射 (与 loader 的 map_user_fb 一致) —
       ring3 图形程序可直接写 framebuffer; fork 的 PSE 克隆已支持 */
    u32 fbaddr = *(u32 *)0x500;
    if (fbaddr >= 0x100000)
        new_pd->map_user_4mb(fbaddr, fbaddr);

    /* 校验映射确实建立 (OOM 时 map_page 静默返回) */
    if (!new_pd->translate_user(load_addr) ||
        !new_pd->translate_user(load_addr + map_sz - 4) ||
        !new_pd->translate_user(stack_top - 4)) {
        delete new_pd;
        return false;
    }

    /* 2. 经内核 PSE 恒等映射写新物理页: 拷代码 + 清 .bss 尾部 + 清栈 —
          此后不再有失败路径。chunk 必须先判 off < sz: 旧实现
          `sz - off` 在 off > sz 时无符号下溢成巨值 → 越界读堆
          (desktop: 越读 30KB) 并把垃圾写进 .bss 页 */
    for (u32 off = 0; off < map_sz; off += 0x1000) {
        u8 *d = (u8 *)new_pd->translate_user(load_addr + off);
        u32 chunk = 0;
        if (off < sz) {
            chunk = (sz - off > 0x1000) ? 0x1000 : (sz - off);
            for (u32 k = 0; k < chunk; k++) d[k] = data[off + k];
        }
        for (u32 k = chunk; k < 0x1000; k++) d[k] = 0;
    }
    for (u32 va = stack_base; va < stack_top; va += 0x1000) {
        u8 *d = (u8 *)new_pd->translate_user(va);
        for (u32 k = 0; k < 0x1000; k++) d[k] = 0;
    }

    /* 3. 切换 */
    if (old_pd && old_pd != PagingManager::get_kernel_paging())
        delete old_pd;   /* owned 页 (sbrk/fork 拷贝) 随析构释放 */
    current_task->paging = new_pd;
    /* 必须同步加载新 CR3: 旧恒等实现里新旧页表 VA 0x400000 指向同一
       物理页, 不切也侥幸正确; 私有页实现下不切 CR3, iret 回 ring3
       时仍走旧页表 — 新程序入口落进 fork 拷贝的旧镜像, 子进程
       "跑着别人的代码" (实测: exec gfxdemo 后实跑 ushell 副本,
       多副本并行抢键盘, open: not found 乱象) */
    new_pd->load();

    /* 重建内核栈上的保存帧 (供后续 schedule 恢复) */
    u32 *csp = (u32 *)(current_task->kernel_stack + KSTACK_SIZE);
    *(--csp) = 0x23;         /* SS */
    *(--csp) = stack_top;    /* ESP */
    *(--csp) = 0x202;        /* EFLAGS */
    *(--csp) = 0x2B;         /* CS */
    *(--csp) = entry;        /* EIP */
    *(--csp) = 0;            /* err_code */
    *(--csp) = 0x20;         /* vec */
    *(--csp) = 0;             /* eax */
    *(--csp) = 0;             /* ecx */
    *(--csp) = 0;             /* edx */
    *(--csp) = 0;             /* ebx */
    csp--;
    *(--csp) = 0;            /* ebp */
    *(--csp) = 0;            /* esi */
    *(--csp) = 0;            /* edi */
    *(--csp) = 0x23;          /* ds */
    *(--csp) = 0x23;          /* es */
    *(--csp) = 0x23;          /* fs */
    *(--csp) = 0x23;          /* gs */

    registers_t *frame = (registers_t *)csp;
    frame->_esp = (u32)&frame->vec;
    current_task->eip = entry;
    current_task->cs = 0x2B;
    current_task->esp = (u32)&frame->vec;
    current_task->eflags = 0x202;
    current_task->user_stack = stack_top;
    current_task->user_esp = stack_top;
    current_task->user_ss  = 0x23;
    current_task->gs = current_task->fs = current_task->es = current_task->ds = 0x23;

    /* 改写当前中断帧: 本次 iretd 直接进入新程序 */
    r->eip = entry;
    r->cs = 0x2B;
    r->eflags = 0x202;
    r->user_esp = stack_top;   /* iretd 弹出的 ring3 ESP */
    r->user_ss  = 0x23;
    r->gs = r->fs = r->es = r->ds = 0x23;
    r->eax = 0;
    r->ecx = 0; r->edx = 0; r->ebx = 0;
    r->ebp = 0; r->esi = 0; r->edi = 0;

    serial_write_str("exec: loaded ");
    serial_write_u32(sz);
    serial_write_str("B at 0x400000\n");
    return true;
}

static void sys_exec(registers_t *r) {
    char path[128];
    if (!copy_str_from_user(path, r->ebx, sizeof(path))) { r->eax = (u32)-1; return; }

    /* Read binary from disk */
    u8 *elf_buf = (u8 *)kmalloc(65536);
    if (!elf_buf) { r->eax = (u32)-1; return; }
    int sz = vfs_open(path, elf_buf, 65536);
    if (sz <= 0) { kfree(elf_buf); r->eax = (u32)-1; return; }

    if (!exec_replace_address_space(r, elf_buf, (u32)sz))
        { kfree(elf_buf); r->eax = (u32)-1; return; }
    kfree(elf_buf);
}

/* exec_fd(fd) — 准则五: 用户态 open 文件→传 fd，内核不解析路径 */
static void sys_exec_fd(registers_t *r) {
    u32 fd = r->ebx;
    if (fd >= MAX_FD || !current_task->fd_buf[fd]) { r->eax = (u32)-1; return; }
    if (current_task->fd_type[fd] != 1) { r->eax = (u32)-1; return; }  /* must be a file */

    u8 *data = current_task->fd_buf[fd];
    u32 sz   = current_task->fd_size[fd];
    if (sz == 0 || sz > 65536) {
        serial_write_str("exec_fd: bad size\n");
        r->eax = (u32)-1; return;
    }

    /* 注意: exec 成功不返回, fd 由 SYS_EXIT 统一清理 */
    if (!exec_replace_address_space(r, data, sz)) {
        serial_write_str("exec_fd: map failed\n");
        r->eax = (u32)-1; return;
    }
}

static void sys_waitpid(registers_t *r) {
    /* 无子进程直接返回 */
    if (list_empty(&current_task->children)) { r->eax = (u32)-1; return; }

    /* 阻塞等待。收割由子进程退出侧 (task_do_exit) 完成:
       摘除 sibling、把返回值写进本任务保存帧的 eax、重新入队。
       schedule 原地改帧 — 切走后 iretd 直接回到用户态 int 0x80
       之后, eax 即子进程 pid, 本函数后续代码不会执行。
       (旧实现在此残留僵尸: 唤醒路径的收割代码永远不跑,
       下一次 waitpid 扫到旧僵尸立即返回 → RUN 不等待) */
    task_struct *self = current_task;
    current_task->state = TASK_BLOCKED;
    schedule(r);
    /* 仅当 schedule 未发生切换 (就绪队列空的异常情况) 才到达这里 */
    if (current_task == self) {
        current_task->state = TASK_RUNNING;
        r->eax = (u32)-1;
    }
}

static void sys_close(registers_t *r) {
    u32 fd = r->ebx;
    if (fd >= MAX_FD || !current_task->fd_buf[fd]) { r->eax = (u32)-1; return; }

    if (current_task->fd_type[fd] == 2 || current_task->fd_type[fd] == 3) {
        /* Pipe: decrement refcount, mark broken so other end knows */
        pipe_t *pipe = (pipe_t *)current_task->fd_buf[fd];
        pipe->refs--;
        pipe->broken = true;  /* one end closed → pipe broken */
        if (pipe->refs <= 0)
            kfree(pipe);
    } else if (current_task->fd_type[fd] != 4 &&
               current_task->fd_type[fd] != 5 &&
               current_task->fd_type[fd] != 6 &&
               current_task->fd_type[fd] != 7) {
        kfree(current_task->fd_buf[fd]);
    }

    current_task->fd_buf[fd] = nullptr;
    current_task->fd_size[fd] = 0;
    current_task->fd_pos[fd] = 0;
    current_task->fd_type[fd] = 0;
    r->eax = 0;
}

static void sys_lseek(registers_t *r) {
    u32 fd = r->ebx;
    int offset = (int)r->ecx;
    int whence = (int)r->edx;
    if (fd >= MAX_FD || !current_task->fd_buf[fd]) { r->eax = (u32)-1; return; }
    u32 new_pos;
    if (whence == 0) new_pos = (u32)offset;
    else if (whence == 1) new_pos = current_task->fd_pos[fd] + (u32)offset;
    else if (whence == 2) new_pos = current_task->fd_size[fd] + (u32)offset;
    else { r->eax = (u32)-1; return; }
    if (new_pos > current_task->fd_size[fd]) new_pos = current_task->fd_size[fd];
    current_task->fd_pos[fd] = new_pos;
    r->eax = new_pos;
}

static void sys_stat(registers_t *r) {
    char path[128];
    u32 buf = r->ecx; /* {size, flags(0=file,1=dir), 0, 0} */
    if (!copy_str_from_user(path, r->ebx, sizeof(path))) { r->eax = (u32)-1; return; }
    /* 准则四: stat reads file metadata from disk */
    if (current_task && !(current_task->caps & CAP_FILE_READ)) {
        r->eax = (u32)-1; return;
    }
    int sz, is_dir;
    sz = vfs_stat(path, &is_dir);
    if (sz < 0) { r->eax = (u32)-1; return; }
    u32 out[4];
    out[0] = (u32)sz;
    out[1] = is_dir ? 1u : 0u;
    out[2] = 0;
    out[3] = 0;
    if (!copy_to_user(buf, out, sizeof(out))) { r->eax = (u32)-1; return; }
    r->eax = 0;
}

/* 文件 fd 复制 (dup/dup2/fork 共用): 深拷贝缓冲。
   共享指针会导致两个 fd close 时对同一 kmalloc 块双 free */
static bool dup_file_fd(task_struct *t, u32 old_fd, int new_fd) {
    if (t->fd_type[old_fd] != 1) return true;   /* 非文件 fd 不需要 */
    u32 sz = t->fd_size[old_fd];
    u32 alloc = sz ? ((sz + 0xFFF) & ~0xFFFu) : 4096;
    u8 *nb = (u8 *)kmalloc(alloc);
    if (!nb) return false;
    for (u32 k = 0; k < sz; k++) nb[k] = t->fd_buf[old_fd][k];
    t->fd_buf[new_fd]  = nb;
    t->fd_size[new_fd] = t->fd_size[old_fd];
    t->fd_pos[new_fd]  = t->fd_pos[old_fd];
    t->fd_type[new_fd] = t->fd_type[old_fd];
    return true;
}

static void sys_dup(registers_t *r) {
    u32 old_fd = r->ebx;
    if (old_fd >= MAX_FD || !current_task->fd_buf[old_fd]) { r->eax = (u32)-1; return; }
    int new_fd = -1;
    for (int i = 0; i < MAX_FD; i++) {
        if (!current_task->fd_buf[i]) { new_fd = i; break; }
    }
    if (new_fd < 0) { r->eax = (u32)-1; return; }

    if (current_task->fd_type[old_fd] == 1) {
        /* 文件 fd: 深拷贝 (共享指针双 close = 双 free) */
        if (!dup_file_fd(current_task, old_fd, new_fd)) { r->eax = (u32)-1; return; }
    } else {
        current_task->fd_buf[new_fd]  = current_task->fd_buf[old_fd];
        current_task->fd_size[new_fd] = current_task->fd_size[old_fd];
        current_task->fd_pos[new_fd]  = current_task->fd_pos[old_fd];
        current_task->fd_type[new_fd] = current_task->fd_type[old_fd];
    }

    /* Increment pipe refcount (准则二: refcounted FDs) */
    if (current_task->fd_type[old_fd] == 2 || current_task->fd_type[old_fd] == 3) {
        pipe_t *pipe = (pipe_t *)current_task->fd_buf[old_fd];
        pipe->refs++;
    }
    r->eax = new_fd;
}

static void sys_dup2(registers_t *r) {
    u32 old_fd = r->ebx;
    u32 new_fd = r->ecx;
    if (old_fd >= MAX_FD || new_fd >= MAX_FD || !current_task->fd_buf[old_fd]) {
        r->eax = (u32)-1; return;
    }
    if (old_fd == new_fd) { r->eax = new_fd; return; }

    /* Close new_fd if open (with proper pipe refcount cleanup) */
    if (current_task->fd_buf[new_fd]) {
        if (current_task->fd_type[new_fd] == 2 || current_task->fd_type[new_fd] == 3) {
            pipe_t *pipe = (pipe_t *)current_task->fd_buf[new_fd];
            pipe->refs--;
            if (pipe->refs <= 0) kfree(pipe);
        } else if (current_task->fd_type[new_fd] != 4 &&
                   current_task->fd_type[new_fd] != 5 &&
                   current_task->fd_type[new_fd] != 6 &&
                   current_task->fd_type[new_fd] != 7) {
            kfree(current_task->fd_buf[new_fd]);
        }
    }

    if (current_task->fd_type[old_fd] == 1) {
        if (!dup_file_fd(current_task, old_fd, (int)new_fd)) { r->eax = (u32)-1; return; }
    } else {
        current_task->fd_buf[new_fd]  = current_task->fd_buf[old_fd];
        current_task->fd_size[new_fd] = current_task->fd_size[old_fd];
        current_task->fd_pos[new_fd]  = current_task->fd_pos[old_fd];
        current_task->fd_type[new_fd] = current_task->fd_type[old_fd];
    }

    /* Increment pipe refcount */
    if (current_task->fd_type[old_fd] == 2 || current_task->fd_type[old_fd] == 3) {
        pipe_t *pipe = (pipe_t *)current_task->fd_buf[old_fd];
        pipe->refs++;
    }
    r->eax = new_fd;
}

/* Pipe: read_fd (ebx) and write_fd (ecx) returned via user-provided pointers */
static void sys_pipe(registers_t *r) {
    u32 fds_va = r->ebx;  /* int fds[2] */
    /* Allocate shared pipe ring buffer */
    pipe_t *pipe = (pipe_t *)kmalloc(sizeof(pipe_t));
    if (!pipe) { r->eax = (u32)-1; return; }
    pipe->rpos = 0;
    pipe->wpos = 0;
    pipe->count = 0;
    pipe->refs = 2;  /* read-end + write-end */
    pipe->broken = false;

    /* Find two free FDs */
    int rfd = -1, wfd = -1;
    for (int i = 0; i < MAX_FD; i++) {
        if (!current_task->fd_buf[i]) {
            if (rfd < 0) rfd = i;
            else if (wfd < 0) { wfd = i; break; }
        }
    }
    if (wfd < 0) { kfree(pipe); r->eax = (u32)-1; return; }

    /* Both FDs point to the same pipe struct.
       read-end uses type=2, write-end uses type=3 */
    current_task->fd_buf[rfd]   = (u8 *)pipe;
    current_task->fd_size[rfd]  = PIPE_BUF_SZ;
    current_task->fd_pos[rfd]   = 0;
    current_task->fd_type[rfd]  = 2;
    current_task->fd_buf[wfd]   = (u8 *)pipe;
    current_task->fd_size[wfd]  = PIPE_BUF_SZ;
    current_task->fd_pos[wfd]   = 0;
    current_task->fd_type[wfd]  = 3;

    u32 fds[2] = { (u32)rfd, (u32)wfd };
    if (!copy_to_user(fds_va, fds, sizeof(fds))) {
        /* 回滚: 释放 pipe 与 fd 槽位 */
        current_task->fd_buf[rfd] = nullptr;
        current_task->fd_buf[wfd] = nullptr;
        current_task->fd_type[rfd] = 0;
        current_task->fd_type[wfd] = 0;
        kfree(pipe);
        r->eax = (u32)-1;
        return;
    }
    r->eax = 0;
}

static void sys_getfb(registers_t *r) {
    /* Fill user buffer with {fb_addr, w, h, pitch, bpp} (5 × u32) */
    u32 buf = r->ebx;
    u32 out[5];
    out[0] = (u32)gfx.fb_addr();
    out[1] = (u32)gfx.fb_width();
    out[2] = (u32)gfx.fb_height();
    out[3] = (u32)gfx.fb_pitch();
    out[4] = (u32)gfx.fb_bpp();
    if (!copy_to_user(buf, out, sizeof(out))) { r->eax = (u32)-1; return; }
    r->eax = 5;  /* number of u32 values written */
}

static void sys_mouse(registers_t *r) {
    u32 buf = r->ebx;
    int x, y, btn;
    mouse_get(&x, &y, &btn);
    u32 out[3];
    out[0] = (u32)x;
    out[1] = (u32)y;
    out[2] = (u32)btn;
    if (!copy_to_user(buf, out, sizeof(out))) { r->eax = (u32)-1; return; }
    r->eax = 3;
}

static void sys_sleep(registers_t *r) {
    u32 ms = r->ebx;
    if (ms > 60000) ms = 60000;  /* cap at 1 minute */
    /* 阻塞调度版: 旧实现 sti/hlt 在 syscall 中途开中断 — PIT 嵌套
       触发时 ISR 的 from_user 分支做 paging->load() 把 CR3 切回
       用户页表, 嵌套返回后内核在错误页表下继续执行 → #GP
       (实测: 确定性 push #GP, ESP 变内核堆地址)。
       正确做法: 挂起当前任务, PIT tick 到期唤醒 (见 isr.cpp)。
       schedule 原地改帧 — 后续代码不再执行, 返回值先写好 */
    r->eax = ms;
    current_task->wake_tick = pit.ticks() + ms / 10 + 1;
    current_task->state = TASK_BLOCKED;
    schedule(r);
}

static void sys_cls(registers_t *r) {
    u8 color = (u8)r->ebx;  /* palette index or BGRA blue byte */
    gfx_clear(color);
    r->eax = 0;
}

static void sys_gfx_putc(registers_t *r) {
    int ofd = current_task ? current_task->output_fd : -1;
    if (ofd >= 0 && ofd < MAX_FD && current_task->fd_buf[ofd]) {
        if (current_task->fd_pos[ofd] < current_task->fd_size[ofd]) {
            current_task->fd_buf[ofd][current_task->fd_pos[ofd]++] = (u8)(r->ebx);
        }
    } else {
        gfx.putc((char)r->ebx);
    }
    r->eax = 0;
}

static void sys_gfx_puts(registers_t *r) {
    int ofd = current_task ? current_task->output_fd : -1;
    if (ofd >= 0 && ofd < MAX_FD && current_task->fd_buf[ofd]) {
        if (!copy_str_from_user(g_ustr, r->ebx, sizeof(g_ustr))) { r->eax = (u32)-1; return; }
        u8 *buf = current_task->fd_buf[ofd];
        u32 size = current_task->fd_size[ofd];
        u32 *pos = &current_task->fd_pos[ofd];
        for (u32 i = 0; g_ustr[i] && *pos < size; i++)
            buf[(*pos)++] = (u8)g_ustr[i];
    } else {
        if (!copy_str_from_user(g_ustr, r->ebx, sizeof(g_ustr))) { r->eax = (u32)-1; return; }
        gfx.puts_utf8(g_ustr);
    }
    r->eax = 0;
}

static void sys_gfx_set_fg(registers_t *r) {
    gfx.set_fg((u8)r->ebx);
    r->eax = 0;
}

static void sys_set_outfd(registers_t *r) {
    int fd = (int)r->ebx;
    if (!current_task) { r->eax = (u32)-1; return; }
    if (fd < -1) fd = -1;
    if (fd >= MAX_FD) fd = -1;
    if (fd >= 0 && !current_task->fd_buf[fd]) fd = -1;
    r->eax = (u32)current_task->output_fd;  /* return previous */
    current_task->output_fd = fd;             /* 准则一: per-task */
}

static void sys_fsync(registers_t *r) {
    u32 fd = r->ebx;
    char name[128];
    if (fd >= MAX_FD || !current_task->fd_buf[fd] || current_task->fd_type[fd] != 1) {
        r->eax = (u32)-1; return;
    }
    if (!copy_str_from_user(name, r->ecx, sizeof(name))) { r->eax = (u32)-1; return; }
    /* 准则四: syncing file to disk requires CAP_FILE_WRITE */
    if (current_task && !(current_task->caps & CAP_FILE_WRITE)) {
        r->eax = (u32)-1; return;
    }
    /* Write FD buffer to disk file */
    int result = vfs_write(name, current_task->fd_buf[fd], (int)current_task->fd_pos[fd]);
    r->eax = (u32)result;
}

/* ---- ramdisk syscalls ---- */

static void sys_rd_create(registers_t *r) {
    char name[RAMDISK_NAME_LEN + 4];
    u32 size = r->edx;
    if (size > 4096 ||
        !copy_str_from_user(name, r->ebx, sizeof(name)) ||
        !copy_from_user(g_ubin, r->ecx, size)) { r->eax = (u32)-1; return; }
    r->eax = (u32)rd_create(name, g_ubin, size);
}

static void sys_rd_read(registers_t *r) {
    char name[RAMDISK_NAME_LEN + 4];
    u32 max = r->edx;
    if (max > 4096) max = 4096;
    if (!copy_str_from_user(name, r->ebx, sizeof(name))) { r->eax = (u32)-1; return; }
    int n = rd_read(name, g_ubin, max);
    if (n > 0 && !copy_to_user(r->ecx, g_ubin, (u32)n)) { r->eax = (u32)-1; return; }
    r->eax = (u32)n;
}

static void sys_rd_list(registers_t *r) {
    u32 max = r->ecx;
    if (max > 4096) max = 4096;
    int n = rd_list(g_ustr, max);
    if (n > 0 && !copy_to_user(r->ebx, g_ustr, (u32)n + 1)) { r->eax = (u32)-1; return; }
    r->eax = (u32)n;
}

static void sys_rd_remove(registers_t *r) {
    char name[RAMDISK_NAME_LEN + 4];
    if (!copy_str_from_user(name, r->ebx, sizeof(name))) { r->eax = (u32)-1; return; }
    r->eax = (u32)rd_remove(name);
}

/* ioctl for fd type 4 (framebuffer) — 准则一 */
static void sys_ioctl(registers_t *r) {
    u32 fd  = r->ebx;
    u32 cmd = r->ecx;
    u32 arg = r->edx;

    if (fd >= MAX_FD || !current_task->fd_buf[fd]) { r->eax = (u32)-1; return; }
    if (current_task->fd_type[fd] != 4) { r->eax = (u32)-1; return; }

    switch (cmd) {
    case IOCTL_GFX_SET_FG:
        gfx.set_fg((u8)arg);
        r->eax = 0;
        break;
    case IOCTL_GFX_CLS:
        gfx.clear((u8)arg);
        r->eax = 0;
        break;
    case IOCTL_GFX_PIXEL: {
        /* arg → {i16 x, i16 y; u8 color} (5 bytes) — 经页表翻译拷入,
           fork/exec 后用户页非恒等映射, 直接解引用会读错物理页 */
        u8 b[5];
        if (!copy_from_user(b, arg, 5)) { r->eax = (u32)-1; return; }
        i16 x = (i16)(b[0] | (b[1] << 8));
        i16 y = (i16)(b[2] | (b[3] << 8));
        gfx.set_pixel(x, y, b[4]);
        r->eax = 0;
        break;
    }
    case IOCTL_GFX_FILL: {
        /* arg → {i16 x, y, w, h; u8 color} (9 bytes) */
        u8 b[9];
        if (!copy_from_user(b, arg, 9)) { r->eax = (u32)-1; return; }
        i16 x = (i16)(b[0] | (b[1] << 8));
        i16 y = (i16)(b[2] | (b[3] << 8));
        i16 w = (i16)(b[4] | (b[5] << 8));
        i16 h = (i16)(b[6] | (b[7] << 8));
        gfx.fill_rect(x, y, w, h, b[8]);
        r->eax = 0;
        break;
    }
    case IOCTL_GFX_LINE: {
        /* arg → {i16 x1, y1, x2, y2; u8 color} (9 bytes) */
        u8 b[9];
        if (!copy_from_user(b, arg, 9)) { r->eax = (u32)-1; return; }
        i16 x1 = (i16)(b[0] | (b[1] << 8));
        i16 y1 = (i16)(b[2] | (b[3] << 8));
        i16 x2 = (i16)(b[4] | (b[5] << 8));
        i16 y2 = (i16)(b[6] | (b[7] << 8));
        gfx.draw_line(x1, y1, x2, y2, b[8]);
        r->eax = 0;
        break;
    }
    case IOCTL_GFX_RECT: {
        u8 b[9];
        if (!copy_from_user(b, arg, 9)) { r->eax = (u32)-1; return; }
        i16 x = (i16)(b[0] | (b[1] << 8));
        i16 y = (i16)(b[2] | (b[3] << 8));
        i16 w = (i16)(b[4] | (b[5] << 8));
        i16 h = (i16)(b[6] | (b[7] << 8));
        gfx.draw_rect(x, y, w, h, b[8]);
        r->eax = 0;
        break;
    }
    case IOCTL_GFX_TEXT: {
        /* arg → {i16 x, y; u8 color; char text[]} (最多 68 字节)
           拷入内核缓冲再解析, 不再解引用用户指针 */
        u8 b[68];
        if (!copy_from_user(b, arg, 68)) { r->eax = (u32)-1; return; }
        i16 tx = (i16)(b[0] | (b[1] << 8));
        i16 ty = (i16)(b[2] | (b[3] << 8));
        char tb[64];
        for (int i = 0; i < 63; i++) {
            tb[i] = (char)b[5 + i];
            if (!tb[i]) break;
        }
        tb[63] = 0;
        gfx.puts_at(tx, ty, tb, b[4]);
        r->eax = 0;
        break;
    }
    case IOCTL_GFX_TEXT_UTF8: {
        /* 中文绘制: arg → {i16 x,y; u8 color; char utf8[]} */
        u8 b[84];
        if (!copy_from_user(b, arg, 84)) { r->eax = (u32)-1; return; }
        i16 tx = (i16)(b[0] | (b[1] << 8));
        i16 ty = (i16)(b[2] | (b[3] << 8));
        char tb[80];
        for (int i = 0; i < 79; i++) {
            tb[i] = (char)b[5 + i];
            if (!tb[i]) break;
        }
        tb[79] = 0;
        gfx.puts_at_utf8(tx, ty, tb, b[4]);
        r->eax = 0;
        break;
    }
    case IOCTL_GFX_BITBLT: {
        /* 位块传输 (Phase 4b): edx → {i16 x,y,w,h; u8 pixels[w*h]}
           像素为调色板索引, 逐点经 set_pixel 自带裁剪。
           数据上限 4088 字节 (g_ubin 4096 - 8 头), 64x64 图标足够 */
        u8 b[8];
        if (!copy_from_user(b, arg, 8)) { r->eax = (u32)-1; return; }
        i16 x = (i16)(b[0] | (b[1] << 8));
        i16 y = (i16)(b[2] | (b[3] << 8));
        i16 w = (i16)(b[4] | (b[5] << 8));
        i16 h = (i16)(b[6] | (b[7] << 8));
        if (w <= 0 || h <= 0 || (u32)w * (u32)h > 4088) { r->eax = (u32)-1; return; }
        if (!copy_from_user(g_ubin, arg + 8, (u32)w * (u32)h)) { r->eax = (u32)-1; return; }
        for (int j = 0; j < h; j++)
            for (int i = 0; i < w; i++)
                gfx.set_pixel(x + i, y + j, g_ubin[j * w + i]);
        r->eax = 0;
        break;
    }
    default:
        r->eax = (u32)-1;
        break;
    }
}

/* 关闭任务全部 fd — SYS_EXIT 与信号杀死路径共用 */
void syscall_cleanup_fds(task_struct *t) {
    if (!t) return;
    for (int i = 0; i < MAX_FD; i++) {
        if (!t->fd_buf[i]) continue;
        if (t->fd_type[i] == 2 || t->fd_type[i] == 3) {
            pipe_t *pipe = (pipe_t *)t->fd_buf[i];
            pipe->refs--;
            pipe->broken = true;
            if (pipe->refs <= 0) kfree(pipe);
        } else if (t->fd_type[i] != 4 &&
                   t->fd_type[i] != 5 &&
                   t->fd_type[i] != 6 &&
                   t->fd_type[i] != 7) {
            kfree(t->fd_buf[i]);
        }
        t->fd_buf[i] = nullptr;
        t->fd_size[i] = 0;
        t->fd_pos[i] = 0;
        t->fd_type[i] = 0;
    }
}

extern "C" void syscall_handler(registers_t *r) {
    /* 诊断: 记录 ring3 任务最早几次系统调用 — 用于定位"切到用户态即
       triple fault"这类问题 (能打印出来就说明 ring3 已经真正跑起来) */
    if ((r->cs & 3) || g_ring0_mode) {
        static int n = 0;
        if (n < 6) {
            n++;
            serial_write_str("ring3 sys=");
            serial_write_u32(r->eax);
            serial_write_str(" pid=");
            serial_write_u32(current_task ? current_task->pid : 0);
            serial_write_char('\n');
        }
    }

    switch (r->eax) {
    case SYS_WRITE: sys_write(r); break;
    case SYS_READ:  sys_read(r);  break;
    case SYS_OPEN:  sys_open(r);  break;
    case SYS_FREAD: sys_fread(r); break;
    case SYS_SBRK:  sys_sbrk(r);  break;
    case SYS_GETCWD: sys_getcwd(r); break;
    case SYS_TIME:  sys_time(r);  break;
    case SYS_GETFB: sys_getfb(r); break;
    case SYS_CLOSE: sys_close(r); break;
    case SYS_MOUSE: sys_mouse(r); break;
    case SYS_SLEEP: sys_sleep(r); break;
    case SYS_CLS:   sys_cls(r);   break;
    case SYS_GFX_PUTC: sys_gfx_putc(r); break;
    case SYS_GFX_PUTS: sys_gfx_puts(r); break;
    case SYS_GFX_SET_FG: sys_gfx_set_fg(r); break;
    case SYS_FAT_DIR:    sys_fat_dir(r);    break;
    case SYS_FAT_CD:     sys_fat_cd(r);     break;
    case SYS_FAT_MKDIR:  sys_fat_mkdir(r);  break;
    case SYS_FAT_RMDIR:  sys_fat_rmdir(r);  break;
    case SYS_FAT_DELETE: sys_fat_delete(r); break;
    case SYS_FAT_RENAME: sys_fat_rename(r); break;
    case SYS_FAT_WRITE:  sys_fat_write(r);  break;
    case SYS_FORK:       sys_fork(r);       break;
    case SYS_EXEC:       sys_exec(r);       break;
    case SYS_WAITPID:    sys_waitpid(r);    break;
    case SYS_EXIT:
        serial_write_str("exit: pid ");
        serial_write_u32(current_task ? current_task->pid : 0);
        serial_write_char('\n');
        syscall_cleanup_fds(current_task);
        task_do_exit(r, r->ebx);
        break;
    case SYS_GETPID:
        if (current_task)
            r->eax = current_task->pid;
        else
            r->eax = (u32)-1;
        break;
    case SYS_KILL:
        /* 准则四: caller must have CAP_SIGNAL */
        if (current_task && !(current_task->caps & CAP_SIGNAL)) {
            r->eax = (u32)-1; break;
        }
        r->eax = task_send_signal((u32)r->ebx, (int)r->ecx);
        break;
    case SYS_SIGACTION: {
        /* ebx=signum, ecx=handler (0=SIG_DFL, 1=SIG_IGN, or user addr) */
        int sig = (int)r->ebx;
        u32 handler = r->ecx;
        if (sig < 1 || sig > 31 || !current_task) { r->eax = (u32)-1; break; }
        u32 old = current_task->sig_handlers[sig];
        current_task->sig_handlers[sig] = handler;
        r->eax = old;
        break;
    }
    case SYS_SIGRETURN:
        /* Restore user context saved before signal handler was called */
        if (current_task) {
            r->eip = current_task->sig_saved_eip;
            r->user_esp = current_task->sig_saved_esp;   /* ring3 真实用户栈 */
            r->eax = 0;
        }
        break;
    case SYS_STAT:
        sys_stat(r);
        break;
    case SYS_LSEEK:
        sys_lseek(r);
        break;
    case SYS_DUP:
        sys_dup(r);
        break;
    case SYS_DUP2:
        sys_dup2(r);
        break;
    case SYS_PIPE:
        sys_pipe(r);
        break;
    case SYS_FWRITE:
        sys_fwrite(r);
        break;
    case SYS_SET_OUTFD:
        sys_set_outfd(r);
        break;
    case SYS_FSYNC:
        sys_fsync(r);
        break;
    case SYS_RD_CREATE:
        sys_rd_create(r);
        break;
    case SYS_RD_READ:
        sys_rd_read(r);
        break;
    case SYS_RD_LIST:
        sys_rd_list(r);
        break;
    case SYS_RD_REMOVE:
        sys_rd_remove(r);
        break;
    case SYS_DROP_CAP:
        r->eax = (u32)(current_task ? task_drop_cap((u32)r->ebx) : -1);
        break;
    case SYS_DISK_READ:
        /* 准则五: raw sector read — direct ATA PIO to avoid
           bcache interference with user-space FS operations */
        if (current_task && !(current_task->caps & CAP_DISK_READ)) {
            r->eax = (u32)-1; break;
        }
        {
            static u8 kbuf[512] __attribute__((aligned(4)));
            int res = ata_read((u32)r->ebx, 1, (u16 *)kbuf);
            if (res == 0 && !copy_to_user(r->ecx, kbuf, 512))
                res = -1;
            r->eax = (u32)res;
        }
        break;
    case SYS_DISK_WRITE:
        if (current_task && !(current_task->caps & CAP_DISK_WRITE)) {
            r->eax = (u32)-1; break;
        }
        {
            static u8 kwbuf[512] __attribute__((aligned(4)));
            if (!copy_from_user(kwbuf, r->ecx, 512)) { r->eax = (u32)-1; break; }
            int res = ata_write((u32)r->ebx, 1, (const u16 *)kwbuf);
            /* 用户态 FS 直写磁盘后, 内核 bcache 里的旧扇区必须失效,
               否则内核侧 (fat12/ext2) 读到过期数据 (缓存一致性) */
            if (res == 0)
                bc_invalidate((u32)r->ebx, 1);
            r->eax = (u32)res;
        }
        break;
    case SYS_IOCTL:
        sys_ioctl(r);
        break;
    case SYS_EXEC_FD:
        sys_exec_fd(r);
        break;
    case SYS_VFS_DIR: {
        char path[128];
        if (!copy_str_from_user(path, r->ebx, sizeof(path))) { r->eax = (u32)-1; break; }
        r->eax = vfs_dir(path);
        break;
    }
    case SYS_VFS_LIST: {
        /* ebx=path, ecx=DirEntry[] (用户缓冲), edx=max 条目数 */
        char path[128];
        u32 ubuf = r->ecx;
        u32 max  = r->edx;
        if (!copy_str_from_user(path, r->ebx, sizeof(path)) || max == 0 || max > 64) {
            r->eax = (u32)-1; break;
        }
        DirEntry entries[64];
        int n = vfs_list_dir(path, entries, max);
        if (n > 0) {
            if (!copy_to_user(ubuf, entries, (u32)n * sizeof(DirEntry))) {
                r->eax = (u32)-1; break;
            }
        }
        r->eax = (u32)n;
        break;
    }
    case SYS_MEMINFO:
        r->eax = mm_free_count();
        r->ebx = mm_total_pages();
        break;
    case SYS_TASK_LIST: {
        /* ebx=task_info[] 用户缓冲, ecx=max 条目数 */
        u32 ubuf = r->ebx;
        u32 max  = r->ecx;
        if (max == 0 || max > TASK_INFO_MAX) { r->eax = (u32)-1; break; }
        struct task_info buf[TASK_INFO_MAX];
        int n = task_snapshot(buf, (int)max);
        if (n > 0 && !copy_to_user(ubuf, buf, (u32)n * sizeof(struct task_info))) {
            r->eax = (u32)-1; break;
        }
        r->eax = (u32)n;
        break;
    }
    default:
        r->eax = (u32)-1;
        break;
    }
}
