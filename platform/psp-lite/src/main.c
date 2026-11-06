/* main.c - command line front end of psp-lite. Run `psp-lite --help`. */
#define _GNU_SOURCE
#include <getopt.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "iss.h"

static void usage(FILE *f) {
    fprintf(f,
            "psp-lite " PSP_VERSION " - RV32I platform-security-processor simulator (shd-labs)\n"
            "usage: psp-lite [options] program.elf|program.bin\n"
            "  --golden          disable every seeded defect (reference behaviour)\n"
            "  --trace           print one line per retired instruction (and per trap) to stderr\n"
            "  --trace-file F    write the trace to F instead of stderr\n"
            "  --dump-regs       print the register file to stderr when the program stops\n"
            "  --max-instr N     stop after N executed instructions (exit status 124)\n"
            "  --input F         serial input comes from file F (default: stdin)\n"
            "  --flag STR        contents of the M-mode-only flag CSRs 0xFC0-0xFC7 (max 32 bytes)\n"
            "  --aslr            load the image at a random 4 KiB-aligned address\n"
            "  --aslr-seed N     like --aslr but with a reproducible choice\n"
            "  --base ADDR       load the image at ADDR (4 KiB aligned)\n"
            "  --ram-mb N        RAM size in MiB (default %u, RAM starts at 0x%08x)\n"
            "  --shadow-stack    enable the shadow-stack hooks (optional exercise; see src/shadow_stack.h)\n"
            "  --flat            treat the program as a flat binary even if it looks like ELF\n"
            "  --stats           print instruction count and speed to stderr at exit\n"
            "  --version         print version and build configuration\n"
            "exit status: the program's exit code (CSR 0x802), 64+mcause for an unhandled trap\n"
            "             (mtvec == 0), 124 when the instruction limit is hit, 2 on load errors.\n",
            RAM_MB_DEFAULT, RAM_BASE_DEFAULT);
}

static uint32_t parse_u32(const char *s, const char *what) {
    char *end;
    unsigned long long v = strtoull(s, &end, 0);
    if (*end || v > 0xFFFFFFFFull) {
        fprintf(stderr, "psp-lite: bad %s '%s'\n", what, s);
        exit(EXIT_LOAD_ERROR);
    }
    return (uint32_t)v;
}

static uint32_t random_u32(void) {
    uint32_t v = 0;
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f || fread(&v, sizeof v, 1, f) != 1) v = (uint32_t)time(NULL) ^ ((uint32_t)getpid() << 16);
    if (f) fclose(f);
    return v;
}

int main(int argc, char **argv) {
    static psp_cpu cpu; /* large (shadow stack) - keep it off the host stack */
    psp_cpu *c = &cpu;
    int golden = 0, dump = 0, stats = 0, force_flat = 0, aslr = 0, have_aslr_seed = 0, ss = 0;
    uint32_t aslr_seed = 0, base = 0xFFFFFFFFu, ram_mb = RAM_MB_DEFAULT;
    uint64_t max_steps = 5000000000ull;
    const char *trace_path = NULL, *input_path = NULL, *flag = "FLAG{psp-lite-default-flag}";
    int trace = 0;

    enum { O_GOLDEN = 256, O_TRACE, O_TRACEF, O_DUMP, O_MAX, O_INPUT, O_FLAG, O_ASLR, O_ASLRSEED, O_BASE, O_RAM,
           O_SS, O_FLAT, O_STATS, O_VERSION, O_HELP };
    static const struct option opts[] = {
        {"golden", no_argument, 0, O_GOLDEN},     {"trace", no_argument, 0, O_TRACE},
        {"trace-file", required_argument, 0, O_TRACEF}, {"dump-regs", no_argument, 0, O_DUMP},
        {"max-instr", required_argument, 0, O_MAX}, {"input", required_argument, 0, O_INPUT},
        {"flag", required_argument, 0, O_FLAG},   {"aslr", no_argument, 0, O_ASLR},
        {"aslr-seed", required_argument, 0, O_ASLRSEED}, {"base", required_argument, 0, O_BASE},
        {"ram-mb", required_argument, 0, O_RAM},  {"shadow-stack", no_argument, 0, O_SS},
        {"flat", no_argument, 0, O_FLAT},         {"stats", no_argument, 0, O_STATS},
        {"version", no_argument, 0, O_VERSION},   {"help", no_argument, 0, O_HELP},
        {0, 0, 0, 0}};
    int o;
    while ((o = getopt_long(argc, argv, "h", opts, NULL)) != -1) {
        switch (o) {
        case O_GOLDEN: golden = 1; break;
        case O_TRACE: trace = 1; break;
        case O_TRACEF: trace = 1; trace_path = optarg; break;
        case O_DUMP: dump = 1; break;
        case O_MAX: max_steps = strtoull(optarg, NULL, 0); break;
        case O_INPUT: input_path = optarg; break;
        case O_FLAG: flag = optarg; break;
        case O_ASLR: aslr = 1; break;
        case O_ASLRSEED: aslr = 1; have_aslr_seed = 1; aslr_seed = parse_u32(optarg, "aslr seed"); break;
        case O_BASE: base = parse_u32(optarg, "base address"); break;
        case O_RAM: ram_mb = parse_u32(optarg, "RAM size"); break;
        case O_SS: ss = 1; break;
        case O_FLAT: force_flat = 1; break;
        case O_STATS: stats = 1; break;
        case O_VERSION:
            printf("psp-lite %s (defects %s, shadow-stack hook from %s)\n", PSP_VERSION,
                   PSP_DEFECTS ? "compiled in" : "compiled out", PSP_SS_SRC_NAME);
            return 0;
        case 'h':
        case O_HELP: usage(stdout); return 0;
        default: usage(stderr); return EXIT_LOAD_ERROR;
        }
    }
    if (optind != argc - 1) {
        usage(stderr);
        return EXIT_LOAD_ERROR;
    }
    const char *prog = argv[optind];
    if (ram_mb < 1 || ram_mb > 1024) {
        fprintf(stderr, "psp-lite: --ram-mb must be 1..1024\n");
        return EXIT_LOAD_ERROR;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);

    defects_init();
    cpu_init(c);
    c->golden = golden;
    c->shadow_stack = ss;
    c->max_steps = max_steps;
    c->ram_base = RAM_BASE_DEFAULT;
    c->ram_size = ram_mb << 20;
    c->ram = calloc(c->ram_size, 1);
    if (!c->ram) {
        perror("calloc");
        return EXIT_LOAD_ERROR;
    }
    memset(c->flag, 0, sizeof c->flag);
    strncpy((char *)c->flag, flag, sizeof c->flag);

    uint32_t span;
    int elf;
    if (image_span(prog, force_flat, &span, &elf)) return EXIT_LOAD_ERROR;
    if (aslr) {
        if (base != 0xFFFFFFFFu) {
            fprintf(stderr, "psp-lite: --aslr and --base are mutually exclusive\n");
            return EXIT_LOAD_ERROR;
        }
        if (span + 0x10000u >= c->ram_size) {
            fprintf(stderr, "psp-lite: image too large for ASLR in %u MiB of RAM\n", ram_mb);
            return EXIT_LOAD_ERROR;
        }
        uint32_t pages = (c->ram_size - span - 0x10000u) >> 12;
        if (pages > ASLR_MAX_PAGES) pages = ASLR_MAX_PAGES;
        uint32_t r = have_aslr_seed ? aslr_seed * 0x9E3779B1u ^ (aslr_seed >> 15) : random_u32();
        r ^= r >> 16;
        r *= 0x7FEB352Du;
        r ^= r >> 15;
        base = c->ram_base + (r % pages) * 4096u;
    } else if (base != 0xFFFFFFFFu && (base & 0xFFFu)) {
        fprintf(stderr, "psp-lite: --base must be 4 KiB aligned\n");
        return EXIT_LOAD_ERROR;
    }
    uint32_t entry;
    if (load_image(c, prog, force_flat, base, &entry)) return EXIT_LOAD_ERROR;
    c->pc = entry;

    if (input_path) {
        c->in = fopen(input_path, "rb");
        if (!c->in) {
            perror(input_path);
            return EXIT_LOAD_ERROR;
        }
    } else {
        c->in = stdin;
    }
    if (trace) {
        c->trace = trace_path ? fopen(trace_path, "w") : stderr;
        if (!c->trace) {
            perror(trace_path);
            return EXIT_LOAD_ERROR;
        }
    }

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    cpu_run(c);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    fflush(stdout);
    if (c->trace && c->trace != stderr) fclose(c->trace);
    if (dump) cpu_dump_regs(c, stderr);
    if (stats) {
        double s = (double)(t1.tv_sec - t0.tv_sec) + 1e-9 * (double)(t1.tv_nsec - t0.tv_nsec);
        fprintf(stderr, "psp-lite: %" PRIu64 " instructions retired, %" PRIu64 " executed, %.3f s, %.1f MIPS%s\n",
                c->instret, c->cycle, s, s > 0 ? (double)c->cycle / s / 1e6 : 0.0, golden ? " (golden)" : "");
        if (ss)
            fprintf(stderr, "psp-lite: shadow stack: %" PRIu64 " pushes, %" PRIu64 " pops, depth %d\n", c->ss.pushes,
                    c->ss.pops, c->ss.top);
        fprintf(stderr, "psp-lite: load base %08x\n", c->load_base);
    }
    return c->exit_code;
}
