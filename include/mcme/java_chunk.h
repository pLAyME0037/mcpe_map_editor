/*
 * Java (Anvil) chunk palettes: the block layer of a Java chunk.
 *
 * The layout is the one the golden world carries — 1.18+, the version
 * mcme reads: `sections` at the document's root (no Level wrapper),
 * each section an absolute Y byte and a `block_states` compound — a
 * palette list of {Name, Properties} compounds plus a long array
 * packing one palette index per block, 4096 to a section, in the
 * X-then-Z-then-Y order. Bits per entry are the palette's need with
 * 4 as the floor, and a packed entry never spans two longs. A
 * section stored without `block_states` is the format's own all-air
 * section: one implicit entry, every index zero.
 *
 * The verbs answer for one block, so this layer answers for one
 * block: which entry the palette says is there, and how to point a
 * position at a different one. Nothing here opens a region file —
 * the chunk document is the unit, and mcme_region_set_chunk is the
 * write that carries an edited tree back out.
 */
#ifndef MCME_JAVA_CHUNK_H
#define MCME_JAVA_CHUNK_H

#include <stdbool.h>
#include <stdint.h>

#include "mcme/core.h"
#include "mcme/nbt.h"
#include "mcme/world.h"          /* McmeWorldBlockStatus: the doc's
                                     answer vocabulary              */
#include "mcme/bedrock_write.h"   /* McmeWriteRefusal: the CLI's shared
                                     write vocabulary, as in region.h —
                                     and McmeBox, the corners fill speaks */

/*
 * The block at world block coordinates (x, y, z) inside the chunk
 * document `root`, as the spec `chunk set` accepts: the palette
 * entry's Name, with its Properties in braces when it has any — the
 * same text mcme_bedrock_block_spec writes for Bedrock entries, so
 * the two worlds print one dialect.
 *
 * A position no stored section covers, and a section stored without
 * block_states, are `minecraft:air`: what the format says they
 * hold. A y no section byte could name (past the byte that keeps Y)
 * is air too — nothing could list it.
 *
 * Returns false where the stored tree is not the format mcme reads,
 * with mcme_last_error() saying which part: sections that are not a
 * list, a section whose Y is not a byte, a palette that is not a
 * list of compounds with string Names, a data array too short for
 * the palette and bits it carries, an index outside its palette.
 */
bool mcme_java_block_spec(const McmeTag *root, int x, int y, int z,
                          McmeBuffer *out);

/*
 * The chunk document under block (x, y, z), opened and parsed — the
 * read every Java block answer stands on, moved from the CLI into
 * the core whole (G9): the region file located through the world's
 * dimension layout, read, decompressed, parsed into `*arena`/`doc`.
 * `world_dir` is the world's own root; `out_file` (may be NULL)
 * receives the region file's path — the write path's own need, kept
 * in the same walk. The caller frees `*arena`. The statuses and the
 * "chunk: ..." messages are the CLI's own, moved verbatim.
 */
McmeWorldBlockStatus mcme_java_chunk_doc(const char *world_dir,
                                         int32_t dimension,
                                         long x, long y, long z,
                                         McmeArena **arena,
                                         McmeNbtDoc *doc,
                                         char *out_file,
                                         size_t out_file_cap);

/* ========================================================================
 * A whole section at once (D40)
 *
 * One section's block layer read for EVERY position it covers — what
 * a sub-chunk's table wants: one pass over the long array instead of
 * one per position. The palette's specs in slot order (the same text
 * mcme_java_block_spec answers) and the 4096 indices pointing at
 * them in X-then-Z-then-Y, each checked against its palette.
 *
 * `stored` says the section stands in the chunk with block_states at
 * all. A section the chunk lists without block_states, and one it
 * lists no entry for, are the format's own all-air answer: stored
 * false, no specs, no indices — every position holds air, what
 * mcme_java_block_spec answers for them, and a caller that keeps an
 * air palette of its own beside the grid has read the world right.
 *
 * Everything returned lives in the arena: the specs' text and the
 * index array both. A malformed section fails the call naming what
 * was found — the per-position read's strictness, at section
 * scale.
 */
#define MCME_JAVA_SECTION_BLOCKS 4096u

typedef struct {
    bool         stored;      /* the section stands with block_states */
    size_t       spec_count;  /* the palette's entries; 0 when !stored */
    const char **specs;       /* spec_count specs, slot order, arena   */
    uint16_t    *indices;     /* 4096, X-then-Z-then-Y, arena — NULL
                                 when the section is the air answer     */
} McmeJavaSectionGrid;

bool mcme_java_section_grid(McmeJavaSectionGrid *out, McmeArena *arena,
                            const McmeTag *root, int y_sec);

/*
 * Replaces the block at (x, y, z) in `root`, in place — the tree the
 * caller parsed, edited to say something else. The position's entry
 * is reused when the new block is one the section's palette already
 * holds, and a new entry is appended when it is not — never
 * replacing a palette entry in place, which would change what every
 * other position holding the old entry means. The section's long
 * array is rebuilt at the bits the palette now needs. A section the
 * chunk does not list is created and kept in Y order — except for
 * air, which a position no section covers already holds, so the
 * edit invents nothing — and a section stored without block_states
 * starts as the all-air palette the format says it is, so the
 * first edit into an untouched section makes a palette of air plus
 * that block and every other position keeps its air.
 *
 * `changed` (when not NULL) reports whether the tree says anything
 * different afterwards: a no-op — the block already stored there —
 * edits nothing, and the caller skips the write, the way Bedrock's
 * chunk set keeps a no-op byte-identical.
 *
 * `refusal` (when not NULL): USAGE for a null argument or states
 * that are not a compound; NOT_READABLE where the stored tree is not
 * the format mcme reads; INTERNAL for arithmetic past the format —
 * a y past the byte that keeps a section's Y, a palette past the
 * 15 bits a packed entry allows.
 *
 * The tree is edited; writing the chunk back is the caller's job.
 */
bool mcme_java_set_block(McmeArena *arena, McmeTag *root,
                         int x, int y, int z,
                         const char *name, const McmeTag *states,
                         bool *changed,
                         McmeWriteRefusal *refusal);

/*
 * Every position of `box` inside the chunk document `root` becomes
 * the block — the whole box in one pass: each section the box
 * touches is decoded once, the box's positions pointed at the
 * target entry, and the section repacked once, rather than a
 * palette edit per block. The target is reused from the palette
 * when it holds the block and appended when it does not, never
 * replacing an entry in place — see mcme_java_set_block. A section
 * the box covers end to end lands as the format's own shorthand
 * for one block: a single-entry palette and no data array. Sections
 * the chunk does not list are created and kept in Y order; air on a
 * position no section covers edits nothing, the column already
 * says it.
 *
 * The tree is one chunk's, and `cx`, `cz` say which: a packing
 * slot holds one position of that chunk, so the box is tested
 * against the chunk's own world columns — x = cx*16 + (slot), z =
 * cz*16 + (slot) — not against the residue class. A box reaching
 * another chunk's column with the same slot leaves this one alone.
 *
 * `changed` (when not NULL) reports whether the tree says anything
 * different afterwards: a box already standing in this block edits
 * nothing, and the caller writes nothing.
 *
 * `refusal` (when not NULL): USAGE for a null argument or states
 * that are not a compound; NOT_READABLE where the stored tree is
 * not the format mcme reads; INTERNAL for arithmetic past the
 * format — a box whose Y spans past the byte that keeps a
 * section's Y, a palette past the 15 bits a packed entry allows.
 *
 * The tree is edited; writing each chunk back is the caller's job.
 */
bool mcme_java_fill_box(McmeArena *arena, McmeTag *root,
                        int cx, int cz,
                        const McmeBox *box,
                        const char *name, const McmeTag *states,
                        bool *changed,
                        McmeWriteRefusal *refusal);

/*
 * Every chunk of `cx1..cx2, cz1..cz2` in the region file `file`,
 * put through the same edit mcme_java_fill_box makes and never
 * written: the preflight the fill runs over every file the box
 * touches BEFORE writing any of them, so one bad chunk refuses the
 * whole fill with nothing written anywhere. A chunk the region file
 * does not have refuses with NOT_THERE — a chunk document carries
 * bookkeeping mcme will not invent, which is where Java fill parts
 * from Bedrock's, which creates the subchunks it needs. A chunk
 * that will not read, or whose tree the edit cannot make, refuses
 * with NOT_READABLE.
 */
bool mcme_java_fill_check(const char *file,
                          int cx1, int cx2, int cz1, int cz2,
                          const McmeBox *box,
                          const char *name, const McmeTag *states,
                          McmeWriteRefusal *refusal);

/*
 * The same chunks, edited and written: every section the box
 * touches repacked once, each changed chunk re-encoded, compressed
 * and appended through mcme_region_set_chunk at timestamp `ts`, a
 * chunk whose sections the box does not change not written at all.
 * `changed` (when not NULL) reports whether any chunk changed — a
 * fill that was already this block rewrites nothing.
 *
 * Same refusals as mcme_java_fill_check. Run that first over every
 * file the box touches: this one alone promises no transaction
 * across chunks, and a chunk gone between check and fill refuses
 * with NOT_THERE with the earlier writes already landed.
 */
bool mcme_java_fill(const char *file,
                    int cx1, int cx2, int cz1, int cz2,
                    const McmeBox *box,
                    const char *name, const McmeTag *states,
                    uint32_t ts, bool *changed,
                    McmeWriteRefusal *refusal);

#endif /* MCME_JAVA_CHUNK_H */
