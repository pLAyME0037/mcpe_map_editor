#include "mcme/core.h"

#include <stdlib.h>
#include <string.h>

/*
 * Bump allocator.
 *
 * Scratch memory with a clear lifetime — one operation, one chunk build, one
 * frame. Allocate in any order, free all at once. This is the tool that keeps
 * the voxel mesher and the NBT reader from drowning in malloc/free pairs.
 *
 * Blocks are a linked list, newest first, so the common case (alloc, alloc,
 * alloc) is a pointer bump with no branching into old blocks.
 */

#define ARENA_DEFAULT_BLOCK (64u * 1024u)

typedef struct McmeArenaBlock {
    struct McmeArenaBlock *next;
    size_t used;
    size_t cap;
    /* Flexible array member: the block memory follows the header. */
    unsigned char data[];
} McmeArenaBlock;

struct McmeArena {
    McmeArenaBlock *head;
    size_t total;       /* total bytes reserved across all blocks */
    size_t large_limit; /* blocks bigger than this are allocated standalone */
};

static McmeArenaBlock *block_new(size_t cap)
{
    McmeArenaBlock *b = malloc(sizeof(McmeArenaBlock) + cap);
    if (!b) return NULL;
    b->next = NULL;
    b->used = 0;
    b->cap = cap;
    return b;
}

McmeArena *mcme_arena_new(void)
{
    McmeArena *a = calloc(1, sizeof(McmeArena));
    if (!a) return NULL;
    /* Anything over half a block gets its own allocation so a single huge
       request does not waste the rest of a block. */
    a->large_limit = ARENA_DEFAULT_BLOCK / 2;
    return a;
}

void mcme_arena_free(McmeArena *arena)
{
    if (!arena) return;
    McmeArenaBlock *b = arena->head;
    while (b) {
        McmeArenaBlock *next = b->next;
        free(b);
        b = next;
    }
    free(arena);
}

void *mcme_arena_alloc(McmeArena *arena, size_t size)
{
    if (!arena) return NULL;

    /* Round the request up to the alignment we guarantee. */
    size = (size + 15u) & ~(size_t)15u;
    if (size == 0) size = 16;

    /* Align the POINTER, not just the size. The block header is a fixed number
       of bytes but not necessarily a multiple of 16, so data[] does not start
       aligned. Padding up to the next boundary here is correct no matter how
       the header is laid out. */
    McmeArenaBlock *head = arena->head;
    if (head && size <= head->cap) {
        uintptr_t raw = (uintptr_t)(head->data + head->used);
        uintptr_t aligned = (raw + 15u) & ~(uintptr_t)15u;
        size_t pad = (size_t)(aligned - raw);
        if (head->used + pad + size <= head->cap) {
            head->used += pad + size;
            arena->total += pad + size;
            return (void *)aligned;
        }
    }

    /* Large requests get a standalone block, chained into the list so
       mcme_arena_free still cleans everything up. */
    if (size > arena->large_limit) {
        McmeArenaBlock *b = block_new(size + 16);
        if (!b) {
            mcme_set_error("arena: out of memory (%zu bytes)", size);
            return NULL;
        }
        uintptr_t raw = (uintptr_t)b->data;
        uintptr_t aligned = (raw + 15u) & ~(uintptr_t)15u;
        b->used = (size_t)(aligned - raw) + size;
        b->next = arena->head;
        arena->head = b;
        arena->total += size;
        return (void *)aligned;
    }

    /* Grow: at least double, so repeated small allocs stay amortised O(1). */
    size_t cap = ARENA_DEFAULT_BLOCK;
    if (head) {
        size_t want = head->cap * 2;
        if (want > cap) cap = want;
    }

    McmeArenaBlock *b = block_new(cap + 16);   /* slack for alignment */
    if (!b) {
        mcme_set_error("arena: out of memory (%zu bytes)", size);
        return NULL;
    }
    uintptr_t raw = (uintptr_t)b->data;
    uintptr_t aligned = (raw + 15u) & ~(uintptr_t)15u;
    b->used = (size_t)(aligned - raw) + size;
    b->next = arena->head;
    arena->head = b;
    arena->total += size;
    return (void *)aligned;
}

char *mcme_arena_strdup(McmeArena *arena, const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *p = mcme_arena_alloc(arena, n);
    if (!p) return NULL;
    memcpy(p, s, n);
    return p;
}

size_t mcme_arena_bytes(const McmeArena *arena)
{
    return arena ? arena->total : 0;
}
