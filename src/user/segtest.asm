[bits 32]
[org 0x400000]

%define SYS_WRITE 1
%define SYS_EXIT 2
%define SYS_GETCWD 7
%define SYS_SLEEP 12
%define SYS_FORK 24
%define SYS_EXEC 25
%define SYS_WAITPID 26
%define SYS_GETPID 27

%macro SET_USER_SEGS 0
    mov ax, 0x23
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
%endmacro
%macro SET_NULL_SEGS 0
    xor eax, eax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
%endmacro
%macro FAIL 2
    SET_USER_SEGS
    cld
    mov eax, SYS_WRITE
    mov ebx, %1
    mov ecx, %2
    int 0x80
    mov eax, SYS_EXIT
    mov ebx, 1
    int 0x80
%endmacro

entry:
%ifdef EXEC_PROBE
    mov ax, cs
    and eax, 3
    cmp eax, 3
    jne .exec_fail
    mov ax, ds
    cmp ax, 0x23
    jne .exec_fail
    mov ax, fs
    cmp ax, 0x23
    jne .exec_fail
    mov ax, gs
    cmp ax, 0x23
    jne .exec_fail
    mov ax, es
    cmp ax, 0x23
    jne .exec_fail
    mov al, [sentinel]
    cmp al, 0x5a
    jne .exec_fail
    mov al, [fs:sentinel]
    cmp al, 0x5a
    jne .exec_fail
    mov al, [gs:sentinel]
    cmp al, 0x5a
    jne .exec_fail
    mov edi, probe_buffer
    mov al, 0xa5
    stosb
    cmp byte [probe_buffer], 0xa5
    jne .exec_fail
    mov eax, SYS_WRITE
    mov ebx, segexec_pass
    mov ecx, segexec_pass_len
    int 0x80
    mov eax, SYS_EXIT
    xor ebx, ebx
    int 0x80
.exec_fail:
    FAIL segexec_fail, segexec_fail_len
%else
    ; Phase 1: the initial task must be a real Ring3 context with usable segments.
    mov ax, cs
    and eax, 3
    cmp eax, 3
    jne .phase1
    mov ax, ds
    cmp ax, 0x23
    jne .phase1
    mov ax, fs
    cmp ax, 0x23
    jne .phase1
    mov ax, gs
    cmp ax, 0x23
    jne .phase1
    mov ax, es
    cmp ax, 0x23
    jne .phase1
    cmp byte [sentinel], 0x5a
    jne .phase1
    cmp byte [fs:sentinel], 0x5a
    jne .phase1
    cmp byte [gs:sentinel], 0x5a
    jne .phase1
    mov edi, initial_buffer
    mov al, 0xa5
    stosb
    cmp byte [initial_buffer], 0xa5
    jne .phase1
    mov eax, SYS_GETPID
    int 0x80
    test eax, eax
    jle .phase1
    mov ebp, eax

    ; Phase 2: no memory access through null data selectors until all are checked.
    SET_NULL_SEGS
    mov eax, SYS_GETPID
    int 0x80
    mov esi, eax
    mov ax, ds
    test ax, ax
    jnz .phase2_null
    mov ax, es
    test ax, ax
    jnz .phase2_null
    mov ax, fs
    test ax, ax
    jnz .phase2_null
    mov ax, gs
    test ax, ax
    jnz .phase2_null
    cmp esi, ebp
    jne .phase2_null
    SET_USER_SEGS

    ; Phase 3: preserve user DF across GETCWD, then compare exact NUL-terminated data.
    cld
    mov eax, SYS_GETCWD
    mov ebx, cwd_base
    mov ecx, 256
    int 0x80
    test eax, eax
    jle .phase3
    cmp eax, 255
    ja .phase3
    cmp byte [cwd_base + eax], 0
    jne .phase3
    mov [cwd_len], eax
    std
    mov eax, SYS_GETCWD
    mov ebx, cwd_df
    mov ecx, 256
    int 0x80
    pushfd
    pop edx
    cld
    test edx, 0x400
    jz .phase3
    cmp eax, [cwd_len]
    jne .phase3
    mov ecx, eax
    inc ecx
    mov esi, cwd_base
    mov edi, cwd_df
    repe cmpsb
    jne .phase3

    ; Phase 4: parent and child both validate inherited mixed selectors.
    mov ax, 0x23
    mov ds, ax
    xor eax, eax
    mov es, ax
    xor eax, eax
    mov fs, ax
    mov ax, 0x23
    mov gs, ax
    mov eax, SYS_FORK
    int 0x80
    test eax, eax
    js .phase4
    jz .child_one
    mov [child_pid], eax
    call check_parent_tuple
    test eax, eax
    jz .phase4
    jmp .wait_one
.child_one:
    call check_parent_tuple
    test eax, eax
    jz .phase4
    mov ax, 0x23
    mov ds, ax
    mov es, ax
    xor eax, eax
    mov fs, ax
    mov gs, ax
    mov ecx, 3
.sleep_loop:
    mov eax, SYS_SLEEP
    mov ebx, 20
    int 0x80
    call check_child_tuple
    test eax, eax
    jz .phase4
    dec ecx
    jnz .sleep_loop
    mov eax, SYS_WRITE
    mov ebx, child_pass
    mov ecx, child_pass_len
    int 0x80
    mov eax, SYS_EXIT
    xor ebx, ebx
    int 0x80
.wait_one:
    mov eax, SYS_WAITPID
    int 0x80
    cmp eax, [child_pid]
    jne .phase5
    call check_parent_tuple
    test eax, eax
    jz .phase5

    ; Phase 6: exec must replace even deliberately null segments.
    mov eax, SYS_FORK
    int 0x80
    test eax, eax
    js .phase6
    jz .exec_child
    mov [exec_pid], eax
    mov eax, SYS_WAITPID
    int 0x80
    cmp eax, [exec_pid]
    jne .phase5
    call check_parent_tuple
    test eax, eax
    jz .phase5
    SET_USER_SEGS
    cld
    mov eax, SYS_WRITE
    mov ebx, segtest_pass
    mov ecx, segtest_pass_len
    int 0x80
    mov eax, SYS_EXIT
    xor ebx, ebx
    int 0x80
.exec_child:
    SET_NULL_SEGS
    mov eax, SYS_EXEC
    mov ebx, exec_path
    int 0x80
    jmp .phase6
.phase1:
    FAIL fail1, fail_len
.phase2_null:
    SET_USER_SEGS
.phase2:
    FAIL fail2, fail_len
.phase3:
    FAIL fail3, fail_len
.phase4:
    FAIL fail4, fail_len
.phase5:
    FAIL fail5, fail_len
.phase6:
    FAIL fail6, fail_len

; EAX=1 only when the inherited parent tuple remains DS=23, ES=0, FS=0, GS=23.
check_parent_tuple:
    xor eax, eax
    mov dx, ds
    cmp dx, 0x23
    jne .done
    mov dx, es
    test dx, dx
    jnz .done
    mov dx, fs
    test dx, dx
    jnz .done
    mov dx, gs
    cmp dx, 0x23
    jne .done
    inc eax
.done:
    ret

; EAX=1 only for child tuple DS=23, ES=23, FS=0, GS=0.
check_child_tuple:
    xor eax, eax
    mov dx, ds
    cmp dx, 0x23
    jne .done
    mov dx, es
    cmp dx, 0x23
    jne .done
    mov dx, fs
    test dx, dx
    jnz .done
    mov dx, gs
    test dx, dx
    jnz .done
    inc eax
.done:
    ret
%endif

sentinel: db 0x5a
initial_buffer: db 0
probe_buffer: db 0
cwd_base: times 256 db 0xa5
cwd_df: times 256 db 0x5a
cwd_len: dd 0
child_pid: dd 0
exec_pid: dd 0
exec_path: db 'SEGEXEC.BIN', 0
child_pass: db 'SEGTEST: CHILD PASS', 10
child_pass_len equ $ - child_pass
segexec_pass: db 'SEGEXEC: PASS', 10
segexec_pass_len equ $ - segexec_pass
segtest_pass: db 'SEGTEST: PASS', 10
segtest_pass_len equ $ - segtest_pass
segexec_fail: db 'SEGEXEC: FAIL 6', 10
segexec_fail_len equ $ - segexec_fail
fail1: db 'SEGTEST: FAIL 1', 10
fail2: db 'SEGTEST: FAIL 2', 10
fail3: db 'SEGTEST: FAIL 3', 10
fail4: db 'SEGTEST: FAIL 4', 10
fail5: db 'SEGTEST: FAIL 5', 10
fail6: db 'SEGTEST: FAIL 6', 10
fail_len equ $ - fail1
