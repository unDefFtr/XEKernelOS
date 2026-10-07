; ============================================================
; XEKernelOS - Stage 2: 最简保护模式加载器
;
; 什么都不画, 只做:
;   1. 屏蔽 PIC
;   2. 安装 GDT
;   3. 进入保护模式
;   4. 跳转到 C 内核 (0x20000)
; ============================================================

[org 0x0000]
[bits 16]

entry:
    mov ax, cs              ; CS=0x1000
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0xFFFE
    mov [cs:boot_drv], dl   ; MBR 通过 DL 传进来的启动盘号

    ; 引导链进度标记 (文本模式, 切图形模式后即被覆盖):
    ;   看到 "stage2"     → MBR 已成功读到 stage2
    ;   看到 "kernel"     → 内核已读入 0x20000
    ;   看到 "vbe"        → 即将设置 VBE 模式 (之后屏幕应变图形)
    ;   停在某个标记        → 后面的步骤卡住, 用最后看到的标记定位
    mov si, msg_s2
    call s2_puts

    ; Enable A20 gate for memory above 1MB
    in al, 0x92
    or al, 2
    out 0x92, al

    ; ---- Load kernel via extended INT 13h ----
    movzx ecx, word [cs:0x1FFE]  ; kernel sectors (written by build_img.py)
    test cx, cx
    jz  .skip_kernel

    ; DAP setup in stage2's data area
    mov byte [cs:dap_size], 0x10
    mov dword [cs:dap_lba], 17     ; kernel starts at LBA 17
    mov dword [cs:dap_dst], 0x20000 ; 32 位线性目标地址 (每轮换算成 seg:off)

    ; Extended INT 13h supports up to 127 sectors per call.
    ; Split large kernels into 127-sector chunks.
    ;
    ; 两个可移植性要点 (VMware 上曾导致引导失败):
    ;  1) 不能用 BIOS 返回的 AL(=本次传输扇区数) 推进循环 — SeaBIOS 会回填,
    ;     VMware 的 BIOS 不回填 → cx 减零不收敛 + LBA 累加错乱 → 一路读到
    ;     盘尾越界 (日志: ide0:0 numIOs=1647, I/O out of range)。
    ;     改为按"本次请求值"推进 (标准引导器做法, 不依赖返回语义)。
    ;  2) 目标地址用 32 位线性值换算 seg:off, 内核超过 127 扇区时 16 位
    ;     offset 不会溢出。
.kload:
    mov ax, cx
    cmp ax, 127
    jbe .last
    mov ax, 127
.last:
    mov [cs:dap_count], ax
    push ax                 ; 本次请求扇区数 (下面只用它推进)
    ; 线性地址 → 实模式 seg:off: 物理 = seg*16 + off
    ;   即 seg = L >> 4, off = L & 0xF (不能当成 32 位高低位直接拆!)
    mov eax, [cs:dap_dst]
    mov edx, eax
    and eax, 0x0F
    mov [cs:dap_buf_off], ax
    shr edx, 4
    mov [cs:dap_buf_seg], dx
    mov dl, [cs:boot_drv]   ; 启动盘号由 MBR 通过 DL 传入 (不再硬编码 0x80)
    mov si, dap
    mov ah, 0x42
    int 0x13
    pop ax
    jc  boot_stop

    sub cx, ax              ; remaining sectors
    jz  .skip_kernel

    movzx eax, ax
    add [cs:dap_lba], eax   ; 按请求扇区数推进 LBA
    shl eax, 9              ; ×512 → 字节
    add [cs:dap_dst], eax
    jmp .kload

.skip_kernel:
    mov si, msg_k
    call s2_puts

    ; ============================================================
    ; VBE 模式选择 — 枚举控制器模式列表并按优先级打分
    ;   旧实现硬编码模式号 0x4144 (QEMU/SeaBIOS 专有), 在 VMware /
    ;   真实 BIOS 上取不到该模式 → 直接停机 (黑屏无输出)。
    ;   改为枚举 VideoModePtr 列表, 要求:
    ;     ModeAttributes: supported | graphics | linear-framebuffer
    ;     MemoryModel 4 (packed) 或 6 (direct)
    ;     BitsPerPixel 32 或 24 (gfx 驱动按 4 / 3 字节像素写)
    ;     >= 640x480
    ;   打分: 32bpp > 24bpp, 1024x768 额外加权 (桌面布局按此分辨率设计),
    ;         同档取面积更大者。
    ; ============================================================
    mov si, msg_v
    call s2_puts
    mov byte [cs:vbe_info + 0], 'V'      ; 写入 "VBE2" 以请求 VBE 2.0+ 结构
    mov byte [cs:vbe_info + 1], 'B'
    mov byte [cs:vbe_info + 2], 'E'
    mov byte [cs:vbe_info + 3], '2'
    mov di, vbe_info
    mov ax, 0x4F00
    int 0x10
    cmp ax, 0x004F
    jne vbe_novbe

    ; 模式列表指针: VBE 2.0+ = 远指针 (偏移@14, 段@16); 1.x = 同段偏移
    mov ax, [cs:vbe_info + 14]
    mov [cs:ml_off], ax
    mov ax, [cs:vbe_info + 16]
    mov [cs:ml_seg], ax
    mov ax, [cs:vbe_info + 4]            ; VBE version
    cmp ax, 0x0200
    jae .ptr_ok
    mov [cs:ml_seg], cs
.ptr_ok:
    mov word [cs:best_mode], 0xFFFF
    mov dword [cs:best_score], 0

.vloop:
    mov ax, [cs:ml_seg]
    mov fs, ax
    mov si, [cs:ml_off]
    mov cx, [fs:si]                      ; mode number (0xFFFF = 列表结束)
    cmp cx, 0xFFFF
    je .vdone
    mov [cs:cur_mode], cx
    add word [cs:ml_off], 2

    mov di, mode_info
    mov ax, 0x4F01
    int 0x10
    cmp ax, 0x004F
    jne .vloop

    mov ax, [cs:mode_info + 0]           ; ModeAttributes
    and ax, 0x0091                       ; supported(1) | graphics(0x10) | linear(0x80)
    cmp ax, 0x0091
    jne .vloop

    mov al, [cs:mode_info + 27]          ; MemoryModel
    cmp al, 4
    je .vm_ok
    cmp al, 6
    jne .vloop
.vm_ok:
    movzx eax, byte [cs:mode_info + 25]  ; BitsPerPixel
    cmp eax, 32
    je .bpp32
    cmp eax, 24
    jne .vloop
    mov ebx, 0x20000000
    jmp .bpp_ok
.bpp32:
    mov ebx, 0x40000000
.bpp_ok:
    movzx eax, word [cs:mode_info + 18]  ; XResolution
    cmp eax, 640
    jb .vloop
    movzx edx, word [cs:mode_info + 20]  ; YResolution
    cmp edx, 480
    jb .vloop
    imul eax, edx                        ; 面积
    add ebx, eax
    cmp word [cs:mode_info + 18], 1024
    jne .score
    cmp word [cs:mode_info + 20], 768
    jne .score
    add ebx, 0x08000000                  ; 1024x768 加权
.score:
    cmp ebx, [cs:best_score]
    jbe .vloop
    mov [cs:best_score], ebx
    mov ax, [cs:cur_mode]
    mov [cs:best_mode], ax
    jmp .vloop

.vdone:
    mov ax, [cs:best_mode]
    cmp ax, 0xFFFF
    je vbe_nomode
    mov bx, ax
    or bx, 0x4000                        ; 请求线性帧缓冲
    mov ax, 0x4F02
    int 0x10
    cmp ax, 0x004F
    jne vbe_nomode
    mov di, mode_info                    ; 重新取最终模式信息
    mov ax, 0x4F01
    mov cx, [cs:best_mode]
    int 0x10
    cmp ax, 0x004F
    jne vbe_nomode

    push ds
    xor ax, ax
    mov ds, ax
    mov eax, [cs:mode_info + 40]
    mov [0x500], eax
    movzx eax, word [cs:mode_info + 18]
    mov [0x504], eax
    movzx eax, word [cs:mode_info + 20]
    mov [0x508], eax
    movzx eax, byte [cs:mode_info + 25]
    mov [0x50C], eax
    movzx eax, word [cs:mode_info + 16]
    mov [0x510], eax
    pop ds

    ; 物理内存上界 (字节) → 0x514: 内核 mm 据此设定上界, 不再硬编码 64MB
    ; (否则 mm 可能分配出实际不存在的物理页)。
    ; 优先 E820 (准确); 失败则回退 INT 15h/AX=88h (16 位, 会少算 1MB);
    ; 都失败写 0, 由内核用默认值。
    mov dword [cs:mem_end], 0
    mov byte  [cs:e820_cnt], 0
    xor ebx, ebx
.e820_loop:
    push cs
    pop es
    mov di, e820_buf
    mov eax, 0xE820
    mov edx, 0x534D4150               ; 'SMAP'
    mov ecx, 24
    int 0x15
    jc  .e820_done
    cmp eax, 0x534D4150
    jne .e820_done
    inc byte [cs:e820_cnt]
    cmp byte [cs:e820_cnt], 40        ; 防御: BIOS 乱返回时不死循环
    ja  .e820_done

    cmp dword [cs:e820_buf + 16], 1   ; type == 1 (usable)
    jne .e820_next
    cmp dword [cs:e820_buf + 4], 0    ; base 高 32 位必须为 0 (<4GB)
    jne .e820_next
    mov eax, [cs:e820_buf + 0]        ; base_lo
    cmp eax, 0x4000000
    jae .e820_next
    add eax, [cs:e820_buf + 8]        ; + length_lo
    jnc .e820_cmp
.e820_cap:
    mov eax, 0x4000000
.e820_cmp:
    cmp eax, 0x4000000
    jbe .e820_store
    jmp .e820_cap
.e820_store:
    cmp eax, [cs:mem_end]
    jbe .e820_next
    mov [cs:mem_end], eax
.e820_next:
    test ebx, ebx
    jz  .e820_done
    jmp .e820_loop
.e820_done:
    mov eax, [cs:mem_end]
    test eax, eax
    jnz .store_mem
    mov ah, 0x88                      ; 回退: 扩展内存 KB (1MB 以上) → 字节
    int 0x15
    jc  .no_ext
    movzx eax, ax
    add eax, 1024
    shl eax, 10
    jmp .store_mem
.no_ext:
    xor eax, eax
.store_mem:
    push ds
    xor bx, bx
    mov ds, bx
    mov [0x514], eax
    pop ds

    ; 禁 PIC
    mov al, 0xFF
    out 0x21, al
    out 0xA1, al
    mov si, msg_pm
    call s2_puts
    cli
    lgdt [cs:gdt_desc]
    mov eax, cr0
    or al, 1
    mov cr0, eax
    jmp 0x08:pm_entry

; ============================================================
; GDT: 4 entries (selector ×8)
;   0x08 = stage2 code (base=0x10000)
;   0x10 = flat data   (base=0)
;   0x18 = kernel code (base=0)
; ============================================================
gdt_start:
    dq 0                    ; null (0x00)

gdt_code_stage2:
    dw 0xFFFF               ; limit[15:0]
    dw 0x0000               ; base[15:0]
    db 0x01                 ; base[23:16] = 1  →  base = 0x00010000
    db 0x9A                 ; access: P=1, DPL=0, code, R=1
    db 0xCF                 ; flags: G=1, D/B=1, L=0, limit[19:16]=0xF
    db 0x00                 ; base[31:24] = 0

gdt_data:
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 0x92                 ; access: P=1, DPL=0, data, W=1
    db 0xCF
    db 0x00

gdt_code_kernel:
    dw 0xFFFF               ; limit[15:0]
    dw 0x0000               ; base[15:0]
    db 0x00                 ; base[23:16] = 0  →  base = 0x00000000
    db 0x9A                 ; access: P=1, DPL=0, code, R=1
    db 0xCF                 ; flags: G=1, D/B=1, L=0, limit[19:16]=0xF
    db 0x00                 ; base[31:24] = 0

gdt_user_data:
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 0x92 | 0x60          ; access: DPL=3, data, W=1
    db 0xCF
    db 0x00

gdt_user_code:
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 0x9A | 0x60          ; access: DPL=3, code, R=1
    db 0xCF
    db 0x00

gdt_tss:
    dw 0x0067               ; limit = 103
    dw 0x0000               ; base[15:0]  (filled at runtime)
    db 0x00                 ; base[23:16] (filled at runtime)
    db 0x89                 ; access: P=1, DPL=0, TSS available
    db 0x00                 ; flags: G=0, reserved D/B=0
    db 0x00                 ; base[31:24] (filled at runtime)

gdt_end:
gdt_desc:
    dw gdt_end - gdt_start - 1
    dd gdt_start + 0x10000

; ============================================================
; 32 位保护模式入口
; ============================================================
[bits 32]
pm_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x9FBFF

    ; 跳转到 C 内核 (物理 0x20000)
    jmp 0x18:0x20000

; 进度标记输出 (SI = 0 结尾字符串, DS:SI)
s2_puts:
    push ax
    mov ah, 0x0E
.s2lp:
    lodsb
    test al, al
    jz .s2end
    int 0x10
    jmp .s2lp
.s2end:
    pop ax
    ret

msg_s2: db "stage2", 13, 10, 0
msg_k:  db "kernel", 13, 10, 0
msg_v:  db "vbe", 13, 10, 0
msg_pm: db "pm", 13, 10, 0

boot_stop:
    mov si, msg_kload
    jmp vfail

; ============================================================
; 失败路径: 回 80x25 文本模式, 打印原因后停机
; (旧实现直接 cli/hlt → 在 VMware 上表现为黑屏, 无从判断原因)
; ============================================================
vbe_novbe:
    mov si, msg_novbe
    jmp vfail
vbe_nomode:
    mov si, msg_nomode
vfail:
    push si                      ; BIOS 调用可能破坏 SI, 先存
    push cs
    pop ds
    mov ax, 0x0003               ; 文本模式, 保证消息可见
    int 0x10
    pop si
    mov ah, 0x0E
.vf_lp:
    lodsb
    test al, al
    jz .vf_halt
    int 0x10
    jmp .vf_lp
.vf_halt:
    cli
    hlt
    jmp .vf_halt

msg_novbe:  db "XEKernelOS: VBE int 10h/4F00 failed - no VBE BIOS", 0
msg_nomode: db "XEKernelOS: no usable VBE mode (need 24/32bpp, >=640x480, linear FB)", 0
msg_kload:  db "XEKernelOS: kernel load failed (BIOS int 13h)", 0

; Extended INT 13h Disk Address Packet (used by kernel loader)
boot_drv:   db 0x80    ; 启动盘号 (MBR 通过 DL 传入)
dap_dst:    dd 0       ; 内核加载目标线性地址 (32 位, 换算成 DAP 的 seg:off)

dap:
dap_size:    db 0
             db 0
dap_count:   dw 0
dap_buf_off: dw 0
dap_buf_seg: dw 0
dap_lba:     dq 0

; VBE 缓冲区
vbe_info:  times 512 db 0
mode_info: times 256 db 0
mode_num:  dw 0

; VBE 模式枚举状态
ml_off:     dw 0        ; 模式列表偏移
ml_seg:     dw 0        ; 模式列表段
cur_mode:   dw 0        ; 当前候选模式号
best_mode:  dw 0xFFFF   ; 最佳模式号
best_score: dd 0        ; 最佳得分

; E820 探测状态
mem_end:    dd 0        ; 可用内存上界 (字节)
e820_cnt:   db 0
e820_buf:   times 24 db 0

times 8192 - ($ - $$) db 0
