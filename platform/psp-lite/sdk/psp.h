/* psp.h - bare-metal SDK for psp-lite programs (RV32I, M/U privilege).
 *
 * Everything a lab program needs: CSR access, serial I/O, exit, a tiny printf
 * and string library (libpsp.c), and helpers to enter user mode.  Build with
 * sdk.mk (clang --target=riscv32 -march=rv32i -mabi=ilp32, ld.lld, llvm-objcopy).
 */
#ifndef PSP_SDK_H
#define PSP_SDK_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

/* ---------------------------------------------------------------- CSRs */
#define PSP_CSR_SEROUT 0x800   /* U RW: write a byte to the serial port         */
#define PSP_CSR_SERIN 0x801    /* U RW: read next input byte, -1 at end of input */
#define PSP_CSR_EXIT 0x802     /* U RW: write -> stop the simulator, exit code   */
#define PSP_CSR_LOADBASE 0xCC0 /* U RO: address the image was loaded at          */
#define PSP_CSR_FLAG0 0xFC0    /* M RO: 0xFC0..0xFC7 = 32-byte flag string       */

#define MSTATUS_MPP_MASK 0x1800u
#define MCAUSE_ILLEGAL_INSN 2u
#define MCAUSE_BREAKPOINT 3u
#define MCAUSE_ECALL_U 8u
#define MCAUSE_ECALL_M 11u

#define PSP_STR_(x) #x
#define PSP_STR(x) PSP_STR_(x)
/* csr_read(0x800) / csr_read(mstatus) / csr_read(PSP_CSR_LOADBASE) */
#define csr_read(csr)                                                                                              \
    ({                                                                                                             \
        uint32_t v_;                                                                                               \
        __asm__ volatile("csrr %0, " PSP_STR(csr) : "=r"(v_)::"memory");                                         \
        v_;                                                                                                        \
    })
#define csr_write(csr, val) __asm__ volatile("csrw " PSP_STR(csr) ", %0" ::"r"((uint32_t)(val)) : "memory")
#define csr_set(csr, val) __asm__ volatile("csrs " PSP_STR(csr) ", %0" ::"r"((uint32_t)(val)) : "memory")
#define csr_clear(csr, val) __asm__ volatile("csrc " PSP_STR(csr) ", %0" ::"r"((uint32_t)(val)) : "memory")

/* ------------------------------------------------------- platform calls */
static inline void psp_putc(int ch) { csr_write(PSP_CSR_SEROUT, ch); }
static inline int psp_getc(void) { return (int)csr_read(PSP_CSR_SERIN); }
static inline __attribute__((noreturn)) void psp_exit(int code) {
    csr_write(PSP_CSR_EXIT, code);
    for (;;) {
    }
}
static inline uint32_t psp_load_base(void) { return csr_read(PSP_CSR_LOADBASE); }
static inline uint32_t psp_cycles(void) { return csr_read(cycle); }
static inline uint32_t psp_instret(void) { return csr_read(instret); }

/* The CPU's own ADD instruction, guaranteed to be emitted (the compiler cannot
 * fold or replace it).  Use it when you want to *test* the hardware adder. */
static inline uint32_t psp_hw_add(uint32_t a, uint32_t b) {
    uint32_t r;
    __asm__ volatile("add %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
    return r;
}
/* ...and ADDI with an immediate known at compile time. */
#define psp_hw_addi(a, imm)                                                                                        \
    ({                                                                                                             \
        uint32_t r_;                                                                                               \
        __asm__ volatile("addi %0, %1, %2" : "=r"(r_) : "r"((uint32_t)(a)), "i"(imm));                            \
        r_;                                                                                                        \
    })

/* Install a trap handler (direct mode). 0 = none: psp-lite then reports the trap and stops. */
static inline void psp_set_trap_handler(void (*handler)(void)) { csr_write(mtvec, (uint32_t)(uintptr_t)handler); }

/* Drop from M-mode to U-mode and jump to fn (never returns).  user_sp is the
 * user stack pointer to use, or NULL to keep the current one. */
__attribute__((noreturn)) void psp_enter_user(void (*fn)(void), void *user_sp);

/* --------------------------------------------------------------- libpsp */
int printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int vprintf(const char *fmt, va_list ap);
int snprintf(char *buf, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int puts(const char *s); /* appends '\n' */
int putchar(int ch);
int getchar(void); /* -1 at end of input */
/* read one line (without the '\n') into buf; returns its length, or -1 at end of input */
int psp_read_line(char *buf, int max);
/* parse a hex number (optional 0x prefix); uses only shifts/ors, no additions */
uint32_t psp_parse_hex(const char *s, const char **end);
/* read exactly n raw bytes from the serial port into buf; returns bytes read */
uint32_t psp_read_bytes(void *buf, uint32_t n);

void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strchr(const char *s, int c);

/* unsigned division without the (absent) M extension; also used by printf */
uint32_t psp_udivmod(uint32_t n, uint32_t d, uint32_t *rem);

/* read the 32-byte flag (M-mode only!) into buf[33] as a NUL-terminated string */
void psp_read_flag(char buf[33]);

#endif
