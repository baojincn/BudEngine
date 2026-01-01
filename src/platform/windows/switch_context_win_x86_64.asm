.code
bud_switch_context_win64 PROC
    ; Save general purpose registers
    push rbp
    push rbx
    push rdi
    push rsi
    push r12
    push r13
    push r14
    push r15

	; Allocate space for XMM registers on the stack
    sub rsp, 160

    ; Use movups to store XMM registers to the stack
    movups [rsp + 0],   xmm6
    movups [rsp + 16],  xmm7
    movups [rsp + 32],  xmm8
    movups [rsp + 48],  xmm9
    movups [rsp + 64],  xmm10
    movups [rsp + 80],  xmm11
    movups [rsp + 96],  xmm12
    movups [rsp + 112], xmm13
    movups [rsp + 128], xmm14
    movups [rsp + 144], xmm15

    ; Switch stack pointers
    mov [rcx], rsp
    mov rsp, rdx

    ; restore XMM registers
    movups xmm6,  [rsp + 0]
    movups xmm7,  [rsp + 16]
    movups xmm8,  [rsp + 32]
    movups xmm9,  [rsp + 48]
    movups xmm10, [rsp + 64]
    movups xmm11, [rsp + 80]
    movups xmm12, [rsp + 96]
    movups xmm13, [rsp + 112]
    movups xmm14, [rsp + 128]
    movups xmm15, [rsp + 144]

    ; Release the space allocated for XMM registers
    add rsp, 160

	; recover general purpose registers
    pop r15
    pop r14
    pop r13
    pop r12
    pop rsi
    pop rdi
    pop rbx
    pop rbp

    ret
bud_switch_context_win64 ENDP
END
