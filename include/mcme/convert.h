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

/* What one conversion did and what it left behind. Every field is a
   count; zero is the honest "nothing of this kind was in the way". */
typedef struct {
    size_t chunks;            /* overworld chunks written                */
    size_t subchunks;         /* payloads read and decoded                */
    size_t blocks;            /* non-air positions the output holds       */
    size_t block_entities;    /* block-entity keys, not carried           */
    size_t chunk_data;        /* chunk keys that are not subchunks:
                                 biome records, chunk versions, entity
                                 lists — counted, not carried             */
    size_t entities;          /* entity-list keys, not carried            */
    size_t legacy_subchunks;  /* subchunks dropped: payloads this build
                                 does not decode, or palettes carrying
                                 legacy (pre-1.9) entries                 */
    size_t other;             /* keys that are neither: level metadata,
                                 other dimensions, anything unparseable   */
    size_t water_layers;      /* second layers dropped — the waterlogging
                                 layer Bedrock keeps beside block zero    */
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
