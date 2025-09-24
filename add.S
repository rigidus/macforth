/* add.S : int add_one(int x);  -> x+1 */

#ifdef __APPLE__
  /* Mach-O (macOS, в т.ч. arm64/x86_64): все C-символы с '_' */
  #define SYM(x) _##x
#else
  /* ELF/COFF (Linux/Windows): без '_' */
  #define SYM(x) x
#endif

    .text

/* ===== arm64 ===== */
#if defined(__aarch64__)
    .globl  SYM(add_one)
SYM(add_one):
    /* input: w0, output: w0 (AAPCS64 / macOS arm64 ABI) */
    add     w0, w0, #1
    ret

/* ===== x86_64 ===== */
#elif defined(__x86_64__)
    .globl  SYM(add_one)
SYM(add_one):
  #ifdef _WIN32
    /* Windows x64 ABI: 1-th int in ecx, result in eax */
    mov     eax, ecx
  #else
    /* System V (Linux/macOS Intel): 1-th int in edi, result eax */
    mov     eax, edi
  #endif
    add     eax, 1
    ret

/* ===== fallback ===== */
#else
    .globl  SYM(add_one)
SYM(add_one):
    /* просто ret, чтобы не падать при сборке под экзотическую цель */
    ret
#endif
