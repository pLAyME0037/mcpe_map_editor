#include "mcme/core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ========================================================================
 * Error string
 * ======================================================================== */

static char g_error[1024];

void mcme_set_error(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(g_error, sizeof(g_error), fmt, args);
    va_end(args);
}

const char *mcme_last_error(void)
{
    return g_error;
}

void mcme_clear_error(void)
{
    g_error[0] = '\0';
}

/* ========================================================================
 * Logging
 *
 * Everything goes to stderr. This is deliberate: when --json is passed, stdout
 * must hold exactly one JSON value and nothing else (wiki/cli.md).
 * ======================================================================== */

static McmeLevel g_level = MCME_LOG_INFO;
static bool      g_quiet = false;

void mcme_log_set_level(McmeLevel level) { g_level = level; }
McmeLevel mcme_log_get_level(void)       { return g_level; }

void mcme_log_set_quiet(bool quiet) { g_quiet = quiet; }

void mcme_log(McmeLevel level, const char *fmt, ...)
{
    if (level < g_level) return;
    if (g_quiet && level < MCME_LOG_ERROR) return;

    static const char *tag[] = { "debug", "info ", "warn ", "error" };
    if (level > MCME_LOG_ERROR) level = MCME_LOG_ERROR;

    fprintf(stderr, "mcme [%s] ", tag[level]);

    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);

    fputc('\n', stderr);
}

/* ========================================================================
 * Utility
 * ======================================================================== */

int mcme_cpu_count(void)
{
#if defined(__linux__)
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (f) {
        int n = 0;
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "processor", 9) == 0) n++;
        }
        fclose(f);
        if (n > 0) return n;
    }
#endif
    return 4; /* a sane default; callers only use this for pool sizing */
}
