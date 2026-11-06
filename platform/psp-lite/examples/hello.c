/* hello.c - the smallest psp-lite program: serial output, input and exit code. */
#include "psp.h"

int main(void) {
    char line[64];
    printf("hello from psp-lite (platform id %x)\n", 0x50535031u);
    printf("dec %d %u, hex %08x %X, char %c, str [%5s] [%-5s], ptr %p\n", -42, 42u, 0xBEEFu, 0xCAFEu, 'Z', "ab",
           "cd", (void *)main);
    int n = psp_read_line(line, sizeof line);
    if (n >= 0)
        printf("you typed %d bytes: %s\n", n, line);
    else
        printf("no input\n");
    return 7;
}
