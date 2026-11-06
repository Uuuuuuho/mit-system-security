/* defects_dump.c - GRADER ONLY: print the seeded defect parameters of this build as JSON.
 * Built by `make defects-dump` with the same SEED as the simulator under test. */
#include "iss.h"

void defects_dump(FILE *f);

int main(void) {
    defects_init();
    defects_dump(stdout);
    return 0;
}
