
/*
void get_output_size(void *win, int *out_w, int *out_h);
*/

/* int asm_sum_output_size(void *win);
   Внутри вызывает C-функцию:
   void get_output_size(void *win, int *out_w, int *out_h);
   и возвращает w + h в виде int.

   Поддержка:
   - macOS (Mach-O) arm64/x86_64 — лидирующее '_' у символов
   - Linux x86_64 (ELF)
   - Windows x64 (COFF)
*/

#ifdef __APPLE__
  #define SYM(x) _##x      /* Mach-O: символы с '_' */
#else
  #define SYM(x) x         /* ELF/COFF: без '_' */
#endif

    .text
    .extern SYM(get_output_size)
    .globl  SYM(asm_sum_output_size)

/* ===================== AArch64 (arm64) ===================== */
#if defined(__aarch64__)
/* AAPCS64: арг1=x0, арг2=x1, арг3=x2, возврат в x0 (или w0 для int) */
SYM(asm_sum_output_size):
    // Пролог: сохранить fp/lr и выделить стек под локалы (минимум 16 байт для выравнивания)
    stp     x29, x30, [sp, -16]!
    mov     x29, sp
    sub     sp, sp, #16

    // Локальные int: w по [sp], h по [sp, #4]
    // У нас уже x0=win (от вызывающего кода)
    mov     x1, sp            // &w
    add     x2, sp, #4        // &h

    bl      SYM(get_output_size)

    // w + h
    ldr     w0, [sp]          // w
    ldr     w1, [sp, #4]      // h
    add     w0, w0, w1        // результат в w0

    add     sp, sp, #16
    ldp     x29, x30, [sp], #16
    ret

/* ===================== x86_64 SysV (Linux/macOS Intel) ===================== */
#elif defined(__x86_64__) && !defined(_WIN32)
/* SysV: rdi, rsi, rdx, rcx, r8, r9 — аргументы; возврат в rax */
SYM(asm_sum_output_size):
    push    rbp
    mov     rbp, rsp
    sub     rsp, 16           // место под два int (8+8 для выравнивания)

    // rdi уже содержит win (аргумент функции)
    lea     rsi, [rbp-8]      // &w
    lea     rdx, [rbp-4]      // &h
    call    SYM(get_output_size)

    mov     eax, DWORD PTR [rbp-8]  // w
    add     eax, DWORD PTR [rbp-4]  // w += h

    leave
    ret

/* ===================== Windows x64 (MS ABI) ===================== */
#elif defined(_WIN32) && defined(__x86_64__)
/* Windows x64: rcx, rdx, r8, r9 — аргументы; возврат в rax */
SYM(asm_sum_output_size):
    push    rbp
    mov     rbp, rsp
    sub     rsp, 32           // shadow space (16) + локалы, кратно 16

    // rcx уже содержит win
    lea     rdx, [rbp-8]      // &w
    lea     r8,  [rbp-4]      // &h
    call    SYM(get_output_size)

    mov     eax, DWORD PTR [rbp-8]
    add     eax, DWORD PTR [rbp-4]

    add     rsp, 32
    pop     rbp
    ret

/* ===================== Fallback: просто 0 ===================== */
#else
SYM(asm_sum_output_size):
    mov     w0, #0
    ret
#endif
