default rel
section .text

global snake_step_asm

; ==============================================================================
; int snake_step_asm(Snake* s, int max_w, int max_h, int grow);
; System V AMD64 ABI: RDI = s, ESI = max_w, EDX = max_h, ECX = grow
; ==============================================================================
snake_step_asm:
    test rdi, rdi
    jz .invalid_arg

    movsxd rax, dword [rdi + 8192]
    test ecx, ecx
    jz .check_min_len
    inc eax
    mov dword [rdi + 8192], eax

.check_min_len:
    cmp eax, 1
    jl .invalid_arg

    mov r8d, dword [rdi]
    add r8d, dword [rdi + 8196]

    mov r9d, dword [rdi + 4096]
    add r9d, dword [rdi + 8200]

    test r8d, r8d
    jl .hit_wall
    cmp r8d, esi
    jge .hit_wall
    test r9d, r9d
    jl .hit_wall
    cmp r9d, edx
    jge .hit_wall

    movsxd r10, eax
    dec r10

.shift_loop:
    test r10, r10
    jle .shift_done

    mov r11d, dword [rdi + r10 * 4 - 4]
    mov dword [rdi + r10 * 4], r11d

    mov r11d, dword [rdi + 4096 + r10 * 4 - 4]
    mov dword [rdi + 4096 + r10 * 4], r11d

    dec r10
    jmp .shift_loop

.shift_done:
    mov dword [rdi], r8d
    mov dword [rdi + 4096], r9d

    movsxd rax, dword [rdi + 8192]
    cmp eax, 4
    jl .success

    mov r10, 1
.collision_loop:
    cmp r10, rax
    jge .success

    cmp dword [rdi + r10 * 4], r8d
    jne .next_segment
    cmp dword [rdi + 4096 + r10 * 4], r9d
    je .hit_self

.next_segment:
    inc r10
    jmp .collision_loop

.hit_wall:
    mov eax, 1
    ret

.hit_self:
    mov eax, 2
    ret

.invalid_arg:
    mov eax, -1
    ret

.success:
    xor eax, eax
    ret
