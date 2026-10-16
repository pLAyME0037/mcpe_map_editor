/*
 * World-to-world conversion: a Bedrock store read chunk by chunk, the
 * blocks through the translate map, a Java world written out. The
 * format facts are each module's own (bedrock_chunk, region, nbt);
 * this module is the pipeline and the policy around it.
 *
 * The policy is decisions.md#D12: what cannot be carried is dropped,
 * counted in the report, and never fails the run; `<out>` must not
 * exist and is removed whole if the run fails, so an output world is
 * either finished or absent. Every count in McmeConvertReport is a
 * named gap — the CLI prints them, the dev-log grows them into later
 * entries.
 */

#ifndef MCME_CONVERT_H
#define MCME_CONVERT_H

#include "mcme/bedrock_write.h"   /* McmeWriteRefusal: the shared why */
#include "mcme/core.h"

/* What one conversion did and what it left behind. Every dropped
   count is a named gap (D12); the carried counts exist so "zero
   dropped" can be told apart from "the input had none". */
typedef struct {
    size_t chunks;            /* chunks written, all three dimensions    */
    size_t subchunks;         /* payloads read and decoded                */
    size_t blocks;            /* non-air positions the output holds       */
    size_t block_entities_carried;
                              /* block-entity documents written to the
                                 output's block_entities list — id and
                                 position beside what each family's own
                                 Java spelling holds (D15, D22)           */
    size_t block_entities;    /* block-entity documents not carried: an
                                 id with no Java spelling mcme trusts, a
                                 document with no coordinates, a document
                                 the world has not filled (a LootTable),
                                 or a record that will not decode (counted
                                 once — half a record is not a count)     */
    size_t items;             /* item stacks not carried: a stack whose
                                 fields the mapping cannot translate —
                                 no half-translated item leaves           */
    size_t chunk_data;        /* chunk keys that are not subchunks:
                                 chunk versions, the older 0x2d biome
                                 records — counted, not carried       */
    size_t entities;          /* entity documents not carried: the
                                 per-chunk lists' documents (the 0x32
                                 records), walked like block-entity
                                 records — the reference answers
                                 nothing Java can load (D23), so every
                                 document counts, and a record that
                                 will not decode counts once         */
    size_t actors;            /* actor documents not carried: one per
                                 actorprefix record the store holds —
                                 the modern spelling of the same
                                 entities — whether the chunk's digp
                                 index still names the record or the
                                 chunk behind it was pruned (D13);
                                 the index keys themselves count
                                 nowhere, their documents count here
                                 (D23)                                 */
    size_t legacy_subchunks;  /* subchunks dropped: payloads this build
                                 does not decode (the pre-palette
                                 versions), or palettes holding an
                                 entry the map still refuses — no
                                 string `name`, or a `val` that is
                                 not a number (the `{name, val}`
                                 answers themselves carry, D24)     */
    size_t other;             /* keys that are neither: level metadata,
                                 a number beyond the game's three
                                 dimensions, anything unparseable         */
    size_t water_positions;   /* second-layer positions not carried —
                                 water beside a block the Java half has
                                 no waterlogged property for, or a layer
                                 cell that is not water at all           */
    size_t ticks_carried;     /* tick entries written to the output
                                 chunk's fluid_ticks / block_ticks lists */
    size_t ticks;             /* tick entries not carried: a record
                                 whose chunk holds no subchunks, a
                                 record that will not decode (counted
                                 once), an entry missing the fields the
                                 Java list spells                      */
    size_t biomes_carried;    /* biome cells (a 4x4x4 each) written to
                                 the output sections' biomes containers */
    size_t biomes;            /* biome cells not carried: an id with no
                                 Java spelling (the cell arrives as the
                                 reference's plains fallback), a cell
                                 whose section mcme does not write, or a
                                 record that will not decode (counted
                                 once — half a record is not a count)   */
    size_t heightmaps_carried;/* the chunk's heightmap — the Data3D
                                 record's 256 columns, already Java's
                                 own numbers — written as the chunk's
                                 MOTION_BLOCKING map (D20)              */
    size_t heightmaps;        /* maps not carried: a converted chunk
                                 whose store held no record, a record
                                 that will not decode (its map is lost
                                 with it), a column the world's height
                                 cannot spell, an orphan record's map  */
} McmeConvertReport;

/*
 * Converts a Bedrock world into `<out>` as a Java 1.20 world.
 *
 * `<in>` must probe as Bedrock; `<out>` must not exist. On failure
 * false is returned, mcme_last_error() says what stood in the way,
 * `refusal` (when not NULL) which class — USAGE for paths, NOT_READABLE
 * for a level.dat mcme cannot parse, STORE for read/write failures,
 * INTERNAL for encoding arithmetic. Whatever `<out>` received is
 * removed before returning false; nothing of a failed run survives.
 */
bool mcme_convert_bedrock_java(const char *in, const char *out,
                               McmeConvertReport *report,
                               McmeWriteRefusal *refusal);

#endif /* MCME_CONVERT_H */
