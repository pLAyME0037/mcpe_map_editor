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
    size_t chunks;            /* overworld chunks written                */
    size_t subchunks;         /* payloads read and decoded                */
    size_t blocks;            /* non-air positions the output holds       */
    size_t block_entities_carried;
                              /* block-entity documents written to the
                                 output's block_entities list             */
    size_t block_entities;    /* block-entity documents not carried: an
                                 id with no Java spelling mcme trusts, a
                                 document with no coordinates, or a
                                 record that will not decode (counted
                                 once — half a record is not a count)     */
    size_t items;             /* item stacks not carried: a stack whose
                                 fields the mapping cannot translate —
                                 no half-translated item leaves           */
    size_t chunk_data;        /* chunk keys that are not subchunks:
                                 chunk versions, entity lists, the older
                                 0x2d biome records — counted, not
                                 carried                                 */
    size_t entities;          /* entity-list keys, not carried            */
    size_t legacy_subchunks;  /* subchunks dropped: payloads this build
                                 does not decode, or palettes carrying
                                 legacy (pre-1.9) entries                 */
    size_t other;             /* keys that are neither: level metadata,
                                 other dimensions, anything unparseable   */
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
