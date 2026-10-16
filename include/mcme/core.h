/*
 * mcme core primitives.
 *
 * Every subsystem includes this header first. See wiki/simplexity.md for the
 * rules this file is written under.
 *
 * Ownership convention, used throughout mcme:
 *   - A function returning `bool` failed; call mcme_last_error() for context.
 *   - A function returning a pointer or a buffer documents who frees it.
 *     If it does not say, the caller frees.
 */

#ifndef MCME_CORE_H
#define MCME_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

/* ========================================================================
 * Version
 * ======================================================================== */

#define MCME_VERSION_STRING "0.2.2"

/* ========================================================================
 * Error reporting
 *
 * One process-wide error string for human-readable context. It is a courtesy
 * for logs and CLI messages, NOT a control-flow mechanism. Code still checks
 * return values.
 * ======================================================================== */

void mcme_set_error(const char *fmt, ...);
const char *mcme_last_error(void);
void mcme_clear_error(void);

/* ========================================================================
 * Logging
 *
 * All log output goes to stderr so stdout stays clean for --json.
 * ======================================================================== */

typedef enum {
    MCME_LOG_DEBUG = 0,
    MCME_LOG_INFO  = 1,
    MCME_LOG_WARN  = 2,
    MCME_LOG_ERROR = 3,
} McmeLevel;

void mcme_log_set_level(McmeLevel level);
void mcme_log_set_quiet(bool quiet);
McmeLevel mcme_log_get_level(void);

void mcme_log(McmeLevel level, const char *fmt, ...);

#define MCME_DEBUG(...) mcme_log(MCME_LOG_DEBUG, __VA_ARGS__)
#define MCME_INFO(...)  mcme_log(MCME_LOG_INFO,  __VA_ARGS__)
#define MCME_WARN(...)  mcme_log(MCME_LOG_WARN,  __VA_ARGS__)
#define MCME_ERROR(...) mcme_log(MCME_LOG_ERROR, __VA_ARGS__)

/* ========================================================================
 * Filesystem
 *
 * Buffers returned here are malloc()'d. The caller frees().
 * ======================================================================== */

/* Read a whole file. On success *out_data is malloc'd and *out_size is set. */
bool mcme_fs_read(const char *path, void **out_data, size_t *out_size);

/* Read a whole file as a NUL-terminated string. Caller frees(). */
char *mcme_fs_read_text(const char *path);

bool mcme_fs_write(const char *path, const void *data, size_t size);
bool mcme_fs_exists(const char *path);
bool mcme_fs_mkdir_p(const char *path);

/* ========================================================================
 * Arena allocator
 *
 * For scratch memory with a clear lifetime (one operation, one frame, one
 * chunk build). Not for anything that outlives the arena.
 * ======================================================================== */

typedef struct McmeArena McmeArena;

McmeArena *mcme_arena_new(void);
void       mcme_arena_free(McmeArena *arena);   /* frees everything allocated from it */
void      *mcme_arena_alloc(McmeArena *arena, size_t size);
char      *mcme_arena_strdup(McmeArena *arena, const char *s);
size_t     mcme_arena_bytes(const McmeArena *arena);   /* total bytes reserved */

/* ========================================================================
 * Byte buffer
 *
 * A growable byte array, malloc-backed. This is the output target for every
 * serializer (NBT, SNBT, JSON) and the scratch space for encoding conversions.
 *
 * Unlike the arena, a buffer is meant to be handed off: mcme_buffer_take()
 * transfers ownership of the bytes to the caller, who frees them.
 * ======================================================================== */

typedef struct {
    uint8_t *data;
    size_t   len;
    size_t   cap;
} McmeBuffer;

void mcme_buffer_init(McmeBuffer *b);  /* data is NULL until first append */
void mcme_buffer_free(McmeBuffer *b);  /* releases the bytes; safe on empty */
bool mcme_buffer_reserve(McmeBuffer *b, size_t extra);

bool mcme_buffer_append(McmeBuffer *b, const void *data, size_t len);
bool mcme_buffer_append_u8(McmeBuffer *b, uint8_t v);
bool mcme_buffer_append_cstr(McmeBuffer *b, const char *s); /* bytes, no NUL */
bool mcme_buffer_append_char(McmeBuffer *b, char c);

/* Endian-specific appends. NBT stores numbers big-endian for Java and
   little-endian for Bedrock, so both are needed. */
bool mcme_buffer_append_u16be(McmeBuffer *b, uint16_t v);
bool mcme_buffer_append_u16le(McmeBuffer *b, uint16_t v);
bool mcme_buffer_append_i32be(McmeBuffer *b, int32_t v);
bool mcme_buffer_append_i32le(McmeBuffer *b, int32_t v);
bool mcme_buffer_append_i64be(McmeBuffer *b, int64_t v);
bool mcme_buffer_append_i64le(McmeBuffer *b, int64_t v);
bool mcme_buffer_append_f32be(McmeBuffer *b, float v);
bool mcme_buffer_append_f32le(McmeBuffer *b, float v);
bool mcme_buffer_append_f64be(McmeBuffer *b, double v);
bool mcme_buffer_append_f64le(McmeBuffer *b, double v);

/* Transfers ownership of the bytes to the caller (who must free() them) and
   resets `b` to empty. Returns NULL if the buffer is empty. */
uint8_t *mcme_buffer_take(McmeBuffer *b, size_t *out_len);

/* ========================================================================
 * Event queue
 *
 * The threading contract for mcme, mirroring the original app:
 *   - Any thread may mcme_eq_post().
 *   - Only the main thread (the GTK loop) may mcme_eq_drain().
 *   - Worker threads produce data; they never touch OpenGL.
 *
 * Drain snapshots the pending list, then runs it unlocked, so a handler may
 * safely post new events.
 * ======================================================================== */

typedef void (*McmeFn)(void *userdata);

typedef struct McmeEventQueue McmeEventQueue;

McmeEventQueue *mcme_eq_new(void);
bool   mcme_eq_post(McmeEventQueue *q, McmeFn fn, void *userdata);
size_t mcme_eq_drain(McmeEventQueue *q); /* runs pending, returns count run */
size_t mcme_eq_pending(const McmeEventQueue *q);
void   mcme_eq_free(McmeEventQueue *q);

/* ========================================================================
 * Thread pool
 *
 * Fixed set of workers. submit() is safe from any thread. wait() blocks until
 * every submitted job has finished.
 * ======================================================================== */

typedef struct McmeThreadPool McmeThreadPool;

McmeThreadPool *mcme_tp_new(int nthreads);        /* 0 => one per CPU */
bool   mcme_tp_submit(McmeThreadPool *tp, McmeFn fn, void *userdata);
void   mcme_tp_wait(McmeThreadPool *tp);
size_t mcme_tp_completed(const McmeThreadPool *tp);
void   mcme_tp_free(McmeThreadPool *tp);

/* ========================================================================
 * Language files
 *
 * Parses the original project's .lang format (key=value, # comments, \n
 * escapes, UTF-8) unmodified. See assets/lang/.
 * ======================================================================== */

typedef struct {
    const char *key;
    const char *value;
} McmeLangEntry;

typedef struct {
    char *id;                    /* e.g. "en_GB" */
    McmeLangEntry *entries;      /* owned; keys and values point into chunks */
    size_t entry_count;
    void  *string_chunks;        /* opaque head of the string block chain */
} McmeLang;

/* Load one .lang file. `id` is copied. Entries point into owned storage. */
bool mcme_lang_load_file(McmeLang *lang, const char *id, const char *path);

/* Returns the value for `key`, or NULL if absent. The pointer stays valid
   until mcme_lang_free(). */
const char *mcme_lang_get(const McmeLang *lang, const char *key);

void mcme_lang_free(McmeLang *lang);

/* ========================================================================
 * Config
 *
 * JSON on disk. The original used gzipped Python pickle, which cannot be read
 * by C and is unsafe to load from an untrusted source. See decisions.md#D4.
 *
 * The `struct cJSON` type is declared by third_party/cjson/cJSON.h. We only
 * forward-declare it here so this header does not depend on cJSON.
 * ======================================================================== */

struct cJSON;

struct cJSON *mcme_config_load(const char *path); /* NULL if missing/invalid */
bool          mcme_config_save(const char *path, const struct cJSON *cfg);
bool          mcme_config_pretty(const struct cJSON *cfg, char **out_text); /* caller frees */

/* Dotted-path accessors. get() returns a borrowed pointer owned by `cfg`. */
struct cJSON *mcme_config_get(struct cJSON *cfg, const char *dotted);
bool          mcme_config_set(struct cJSON *cfg, const char *dotted, const char *json_value);

/* ========================================================================
 * Utility
 * ======================================================================== */

int mcme_cpu_count(void);

#endif /* MCME_CORE_H */
