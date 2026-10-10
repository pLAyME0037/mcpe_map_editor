/*
 * Unit test entry point.
 *
 * The tests themselves live in src/<subsystem>/selftest.c so they are compiled
 * into the binary and reachable via `mcme selftest`. This file is what
 * `./nob test` runs: a normal host-side invocation with no CLI in the way.
 *
 * Adding a subsystem: write `int mcme_test_<name>(void)` in its selftest.c,
 * declare it in src/app/main.c's suite table, and add it to the array below.
 */

#include <stdio.h>

int mcme_test_core(void);
int mcme_test_nbt(void);
int mcme_test_compress(void);
int mcme_test_world(void);

int main(void)
{
    struct {
        const char *name;
        int (*fn)(void);
    } suites[] = {
        { "core", mcme_test_core },
        { "nbt", mcme_test_nbt },
        { "compress", mcme_test_compress },
        { "world", mcme_test_world },
    };

    const int n = (int)(sizeof(suites) / sizeof(suites[0]));
    int total_failed = 0;

    for (int i = 0; i < n; ++i) {
        int failed = suites[i].fn();
        total_failed += failed;
        printf("%-12s %s\n", suites[i].name, failed == 0 ? "ok" : "FAILED");
    }

    printf("\n%s — %d suite%s, %d failure%s\n",
           total_failed == 0 ? "PASS" : "FAIL",
           n, n == 1 ? "" : "s",
           total_failed, total_failed == 1 ? "" : "s");

    return total_failed == 0 ? 0 : 1;
}
