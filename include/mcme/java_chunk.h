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

#include "mcme/core.h"
#include "mcme/nbt.h"
#include "mcme/bedrock_write.h"   /* McmeWriteRefusal: the CLI's shared
                                     write vocabulary, as in region.h */

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

#endif /* MCME_JAVA_CHUNK_H */
