"""Memory map of the specsim machine (all addresses are physical; there is no MMU).

Data memory (byte addressable, one flat space):

    0x0000_0000 - 0x000F_FFFF   unmapped: any access faults
    0x0010_0000 - 0x001F_FFFF   kernel data          (kernel mode only)
    0x0020_0000 - 0x009F_FFFF   user memory          (user and kernel)
        0x0020_0000 - 0x003F_FFFF   program .data (each loaded program gets its own slice)
        0x0040_0000 - 0x004F_FFFF   PROBE: 256 x 4096-byte slots shared with the kernel
        0x0050_0000 - 0x009E_FFFF   HEAP: free scratch memory for your programs
        0x009F_0000 - 0x009F_FFFF   stack (sp starts at STACK_TOP)

Instruction memory is separate (Harvard style) and cannot be read as data:

    0x4000_0000 ...   user text
    0x8000_0000 ...   kernel text (fetching it in user mode faults)
"""

KERNEL_BASE = 0x0010_0000
KERNEL_END = 0x0020_0000
USER_BASE = 0x0020_0000
USER_END = 0x00A0_0000
MEM_END = USER_END

USER_DATA_BASE = 0x0020_0000
USER_DATA_END = 0x0040_0000

PROBE_BASE = 0x0040_0000
PROBE_STRIDE = 4096
PROBE_ENTRIES = 256

HEAP_BASE = 0x0050_0000
HEAP_END = 0x009F_0000

STACK_TOP = 0x00A0_0000

UTEXT_BASE = 0x4000_0000
KTEXT_BASE = 0x8000_0000

# syscall numbers (in a7)
SYS_NOP = 0
SYS_FR = 1        # Part 1: architectural access probe[secret1[i] * 4096]
SYS_V1 = 2        # Part 2: Spectre v1 gadget, bound flushed by the kernel
SYS_V1_HARD = 3   # Part 3: same gadget, bound cached, slow index computation

PUBLIC_CONSTANTS = {
    "KERNEL_BASE": KERNEL_BASE, "KERNEL_END": KERNEL_END,
    "USER_BASE": USER_BASE, "USER_END": USER_END,
    "PROBE_BASE": PROBE_BASE, "PROBE_STRIDE": PROBE_STRIDE, "PROBE_ENTRIES": PROBE_ENTRIES,
    "HEAP_BASE": HEAP_BASE, "HEAP_END": HEAP_END, "STACK_TOP": STACK_TOP,
    "LINE_SIZE": 64,
    "SYS_NOP": SYS_NOP, "SYS_FR": SYS_FR, "SYS_V1": SYS_V1, "SYS_V1_HARD": SYS_V1_HARD,
}
