/* libpsp.c - minimal C runtime for psp-lite programs.
 *
 * printf supports: %d %i %u %x %X %p %c %s %%, flags '-' and '0', a field
 * width (number or '*'), and ignores 'l'/'h' length modifiers (everything is
 * 32-bit on RV32).  Number formatting uses shift/subtract division only, so it
 * does not depend on the ALU adder.
 *
 * Also provides the integer helpers clang calls on RV32I (no M extension):
 * __mulsi3, __udivsi3, __umodsi3, __divsi3, __modsi3.
 */
#include "psp.h"

/* ----------------------------------------------------------- arithmetic */
uint32_t psp_udivmod(uint32_t n, uint32_t d, uint32_t *rem) {
    if (d == 0) { /* RISC-V M-extension semantics */
        if (rem) *rem = n;
        return 0xFFFFFFFFu;
    }
    uint32_t q = 0, r = 0;
    for (int i = 31; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1u);
        if (r >= d) {
            r -= d;
            q |= 1u << i;
        }
    }
    if (rem) *rem = r;
    return q;
}

uint32_t __udivsi3(uint32_t n, uint32_t d) { return psp_udivmod(n, d, 0); }
uint32_t __umodsi3(uint32_t n, uint32_t d) {
    uint32_t r;
    psp_udivmod(n, d, &r);
    return r;
}
int32_t __divsi3(int32_t n, int32_t d) {
    if (d == 0) return -1;
    int neg = (n < 0) != (d < 0);
    uint32_t un = n < 0 ? 0u - (uint32_t)n : (uint32_t)n, ud = d < 0 ? 0u - (uint32_t)d : (uint32_t)d;
    uint32_t q = psp_udivmod(un, ud, 0);
    return neg ? (int32_t)(0u - q) : (int32_t)q;
}
int32_t __modsi3(int32_t n, int32_t d) {
    if (d == 0) return n;
    uint32_t un = n < 0 ? 0u - (uint32_t)n : (uint32_t)n, ud = d < 0 ? 0u - (uint32_t)d : (uint32_t)d, r;
    psp_udivmod(un, ud, &r);
    return n < 0 ? (int32_t)(0u - r) : (int32_t)r;
}
/* shift-and-add multiply: like any compiled code it uses the ALU adder */
uint32_t __mulsi3(uint32_t a, uint32_t b) {
    uint32_t r = 0;
    while (b) {
        if (b & 1u) r += a;
        a <<= 1;
        b >>= 1;
    }
    return r;
}

/* --------------------------------------------------------------- strings */
void *memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = dst;
    const uint8_t *s = src;
    while (n--) *d++ = *s++;
    return dst;
}
void *memmove(void *dst, const void *src, size_t n) {
    uint8_t *d = dst;
    const uint8_t *s = src;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else if (d > s) {
        d += n;
        s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}
void *memset(void *dst, int c, size_t n) {
    uint8_t *d = dst;
    while (n--) *d++ = (uint8_t)c;
    return dst;
}
int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *x = a, *y = b;
    for (; n; n--, x++, y++)
        if (*x != *y) return *x < *y ? -1 : 1;
    return 0;
}
size_t strlen(const char *s) {
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}
int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) a++, b++;
    return (unsigned char)*a - (unsigned char)*b;
}
int strncmp(const char *a, const char *b, size_t n) {
    for (; n; n--, a++, b++) {
        if (*a != *b) return (unsigned char)*a - (unsigned char)*b;
        if (!*a) return 0;
    }
    return 0;
}
char *strchr(const char *s, int c) {
    for (;; s++) {
        if (*s == (char)c) return (char *)s;
        if (!*s) return 0;
    }
}

/* ------------------------------------------------------------------ I/O */
int putchar(int ch) {
    psp_putc(ch);
    return (unsigned char)ch;
}
int getchar(void) { return psp_getc(); }
int puts(const char *s) {
    while (*s) psp_putc(*s++);
    psp_putc('\n');
    return 0;
}
int psp_read_line(char *buf, int max) {
    int n = 0, ch;
    while ((ch = psp_getc()) >= 0 && ch != '\n')
        if (n < max - 1) buf[n++] = (char)ch;
    if (max > 0) buf[n < max ? n : max - 1] = 0;
    if (ch < 0 && n == 0) return -1;
    return n;
}
uint32_t psp_read_bytes(void *buf, uint32_t n) {
    uint8_t *p = buf, *end = p + n;
    while (p != end) {
        int ch = psp_getc();
        if (ch < 0) break;
        *p++ = (uint8_t)ch;
    }
    return (uint32_t)(p - (uint8_t *)buf);
}
uint32_t psp_parse_hex(const char *s, const char **end) {
    uint32_t v = 0;
    while (*s == ' ') s++;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    for (;; s++) {
        uint32_t d; /* the digit maps compile to addi with small/negative immediates */
        if (*s >= '0' && *s <= '9')
            d = (uint32_t)(*s - '0');
        else if (*s >= 'a' && *s <= 'f')
            d = (uint32_t)(*s - 'a' + 10);
        else if (*s >= 'A' && *s <= 'F')
            d = (uint32_t)(*s - 'A' + 10);
        else
            break;
        v = (v << 4) | d; /* accumulate with shift/or */
    }
    if (end) *end = s;
    return v;
}

void psp_read_flag(char buf[33]) {
    uint32_t w[8];
    w[0] = csr_read(0xFC0);
    w[1] = csr_read(0xFC1);
    w[2] = csr_read(0xFC2);
    w[3] = csr_read(0xFC3);
    w[4] = csr_read(0xFC4);
    w[5] = csr_read(0xFC5);
    w[6] = csr_read(0xFC6);
    w[7] = csr_read(0xFC7);
    memcpy(buf, w, 32);
    buf[32] = 0;
}

/* --------------------------------------------------------------- printf */
typedef struct {
    char *buf; /* NULL: serial port */
    size_t cap, len;
} sink;

static void emit(sink *s, char ch) {
    if (!s->buf)
        psp_putc(ch);
    else if (s->len + 1 < s->cap)
        s->buf[s->len] = ch;
    s->len++;
}

static void emit_padded(sink *s, const char *str, int len, int width, int left, char pad) {
    int fill = width > len ? width - len : 0;
    if (!left)
        while (fill-- > 0) emit(s, pad);
    for (int i = 0; i < len; i++) emit(s, str[i]);
    if (left)
        while (fill-- > 0) emit(s, ' ');
}

static int format(sink *s, const char *fmt, va_list ap) {
    static const char hexl[] = "0123456789abcdef", hexu[] = "0123456789ABCDEF";
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            emit(s, *fmt);
            continue;
        }
        fmt++;
        int left = 0, width = 0;
        char pad = ' ';
        for (;; fmt++) {
            if (*fmt == '-')
                left = 1;
            else if (*fmt == '0')
                pad = '0';
            else
                break;
        }
        if (*fmt == '*') {
            width = va_arg(ap, int);
            fmt++;
        } else {
            while (*fmt >= '0' && *fmt <= '9') {
                width = width * 10 + (*fmt - '0');
                fmt++;
            }
        }
        while (*fmt == 'l' || *fmt == 'h' || *fmt == 'z') fmt++;
        char tmp[16];
        int n = 0;
        switch (*fmt) {
        case 'd':
        case 'i':
        case 'u': {
            uint32_t v;
            int neg = 0;
            if (*fmt == 'u') {
                v = va_arg(ap, uint32_t);
            } else {
                int32_t sv = va_arg(ap, int32_t);
                neg = sv < 0;
                v = neg ? 0u - (uint32_t)sv : (uint32_t)sv;
            }
            char rev[12];
            int k = 0;
            do {
                uint32_t r;
                v = psp_udivmod(v, 10, &r);
                rev[k++] = (char)('0' | r);
            } while (v);
            if (neg) tmp[n++] = '-';
            while (k) tmp[n++] = rev[--k];
            if (neg && pad == '0' && !left) { /* "-0042" */
                emit(s, '-');
                emit_padded(s, tmp + 1, n - 1, width - 1, left, pad);
            } else {
                emit_padded(s, tmp, n, width, left, pad);
            }
            break;
        }
        case 'p':
            emit(s, '0');
            emit(s, 'x');
            width = 8;
            pad = '0';
            /* fall through */
        case 'x':
        case 'X': {
            uint32_t v = va_arg(ap, uint32_t);
            const char *digits = (*fmt == 'X') ? hexu : hexl;
            int started = 0;
            for (int sh = 28; sh >= 0; sh -= 4) {
                uint32_t d = (v >> sh) & 15u;
                if (d || started || sh == 0) {
                    tmp[n++] = digits[d];
                    started = 1;
                }
            }
            emit_padded(s, tmp, n, width, left, pad);
            break;
        }
        case 'c':
            tmp[0] = (char)va_arg(ap, int);
            emit_padded(s, tmp, 1, width, left, ' ');
            break;
        case 's': {
            const char *str = va_arg(ap, const char *);
            if (!str) str = "(null)";
            emit_padded(s, str, (int)strlen(str), width, left, ' ');
            break;
        }
        case '%': emit(s, '%'); break;
        case 0: return (int)s->len;
        default:
            emit(s, '%');
            emit(s, *fmt);
            break;
        }
    }
    return (int)s->len;
}

int vprintf(const char *fmt, va_list ap) {
    sink s = {0, 0, 0};
    return format(&s, fmt, ap);
}
int printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vprintf(fmt, ap);
    va_end(ap);
    return r;
}
int snprintf(char *buf, size_t n, const char *fmt, ...) {
    sink s = {buf, n, 0};
    va_list ap;
    va_start(ap, fmt);
    int r = format(&s, fmt, ap);
    va_end(ap);
    if (n) buf[s.len < n ? s.len : n - 1] = 0;
    return r;
}
