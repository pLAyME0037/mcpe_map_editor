#include "mcme/core.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

bool mcme_fs_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

bool mcme_fs_read(const char *path, void **out_data, size_t *out_size)
{
    if (!path || !out_data || !out_size) {
        mcme_set_error("mcme_fs_read: null argument");
        return false;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        mcme_set_error("cannot open '%s': %s", path, strerror(errno));
        return false;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        mcme_set_error("cannot seek '%s': %s", path, strerror(errno));
        fclose(f);
        return false;
    }

    long size = ftell(f);
    if (size < 0) {
        mcme_set_error("cannot size '%s': %s", path, strerror(errno));
        fclose(f);
        return false;
    }
    rewind(f);

    /* One extra byte so the buffer is always NUL-terminated, which makes
       mcme_fs_read_text trivial and costs nothing. */
    char *buf = malloc((size_t)size + 1);
    if (!buf) {
        mcme_set_error("out of memory reading '%s' (%ld bytes)", path, size);
        fclose(f);
        return false;
    }

    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);

    if (got != (size_t)size) {
        mcme_set_error("short read on '%s' (%zu of %ld bytes)", path, got, size);
        free(buf);
        return false;
    }

    buf[size] = '\0';
    *out_data = buf;
    *out_size = (size_t)size;
    return true;
}

char *mcme_fs_read_text(const char *path)
{
    void *data;
    size_t size;
    if (!mcme_fs_read(path, &data, &size)) return NULL;
    return (char *)data;
}

bool mcme_fs_write(const char *path, const void *data, size_t size)
{
    if (!path || (!data && size > 0)) {
        mcme_set_error("mcme_fs_write: null argument");
        return false;
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        mcme_set_error("cannot create '%s': %s", path, strerror(errno));
        return false;
    }

    size_t put = fwrite(data, 1, size, f);
    if (put != size) {
        mcme_set_error("short write to '%s' (%zu of %zu bytes)", path, put, size);
        fclose(f);
        return false;
    }

    if (fclose(f) != 0) {
        mcme_set_error("cannot close '%s': %s", path, strerror(errno));
        return false;
    }
    return true;
}

bool mcme_fs_mkdir_p(const char *path)
{
    if (!path || !*path) return false;

    char buf[4096];
    size_t n = strlen(path);
    if (n >= sizeof(buf)) {
        mcme_set_error("path too long: %s", path);
        return false;
    }
    memcpy(buf, path, n + 1);

    /* Walk left to right, creating each component. */
    for (char *p = buf + 1; *p; ++p) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(buf, 0755) != 0 && errno != EEXIST) {
            mcme_set_error("cannot create directory '%s': %s", buf, strerror(errno));
            return false;
        }
        *p = '/';
    }

    if (mkdir(buf, 0755) != 0 && errno != EEXIST) {
        mcme_set_error("cannot create directory '%s': %s", buf, strerror(errno));
        return false;
    }
    return true;
}
