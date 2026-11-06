/* selftest: does this machine show a usable cached vs flushed timing gap? */
#include "shd.h"
int main(void) {
    shd_timer_init();
    uint8_t *buf = shd_alloc(1 << 22, 0);
    enum { N = 2000 };
    static uint64_t hit[N], miss[N];
    for (int i = 0; i < N; i++) {
        uint8_t *p = buf + (size_t)(i % 512) * 4096 + 128;
        shd_access(p); hit[i] = shd_time_access(p);
        shd_flush(p); shd_mfence(); miss[i] = shd_time_access(p);
    }
    uint64_t h = shd_median(hit, N), m = shd_median(miss, N);
    printf("arch=%s timer=%s line=%ldB  median hit=%llu  median flushed=%llu  -> %s\n",
#if defined(__x86_64__)
           "x86_64",
#else
           "aarch64",
#endif
           shd_timer_name(), shd_line_size(), (unsigned long long)h, (unsigned long long)m,
           m > h + (h / 2 + 1) ? "OK: gap visible" : "WEAK: try SHD_TIMER=thread / bare metal");
    return 0;
}
