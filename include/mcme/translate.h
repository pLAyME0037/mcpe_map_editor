/*
 * Bedrock block palette entries as Java 1.20 ones.
 *
 * A Bedrock subchunk palette holds one little-endian NBT compound per
 * distinct block — `name`, `states`, `version` — and a Java section
 * palette holds one compound per distinct block too: `Name`, and
 * `Properties` when any state differs from the default. This module is
 * the map between the two, for one version pair: Bedrock 1.20 to Java
 * 1.20.
 *
 * The rules, in the order they apply:
 *
 *   1. An entry without a `version` field is a legacy palette entry —
 *      a pre-1.9 block, often `{name, val}` — and has no Java 1.20
 *      spelling mcme knows. It is refused; the caller reports and
 *      carries air.
 *
 *   2. A handful of names change wholesale: `minecraft:sand` with red
 *      sand is `minecraft:red_sand`, states and all.
 *
 *   3. States pass through under Java's names where the two editions
 *      agree: `pillar_axis` is `axis` on every log, wood, stem and
 *      hyphae; `snowy`, `lit` and the rest are spelled the same and
 *      keep their tag types (Bedrock and Java store both editions of
 *      boolean and enum states the same way).
 *
 *   4. A state the map does not list is DROPPED, and the block takes
 *      its Java default — the choice PyMCTranslate makes with the same
 *      inputs (a furnace's `facing_direction`, a torch's
 *      `torch_facing_direction`, farmland's `moisture` all become the
 *      default state on both sides). This is what keeps mcme's output
 *      in agreement with the reference rather than inventing mappings
 *      the reference does not have. There is no block registry here on
 *      purpose: the map is exactly the cases real worlds have needed,
 *      and tools/ref_convert.py pins every one of them against
 *      PyMCTranslate itself.
 *
 * The output entry is new memory in `arena`; the input is untouched.
 */

#ifndef MCME_TRANSLATE_H
#define MCME_TRANSLATE_H

#include "mcme/nbt.h"

/*
 * One Bedrock palette entry as the Java 1.20 palette entry.
 *
 * Returns NULL when the entry is not a compound, has no string `name`,
 * or is a legacy entry without `version` — mcme_last_error() says which.
 * A caller converting a whole layer reports the drop rather than
 * failing the run (decisions.md#D12).
 */
McmeTag *mcme_translate_block(McmeArena *arena, const McmeTag *entry);

#endif /* MCME_TRANSLATE_H */
