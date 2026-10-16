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
 *   1. The entry's shape decides first, the way amulet's loader
 *      branches: `states` present — or neither field — is a modern
 *      entry (`version` is never read), walked by the rules below.
 *      No `states` and a `val` is a legacy pre-1.9 entry: the loader
 *      spells it `block_data` and answers it through the last
 *      pre-1.13 spec (the world's version capped at 1.12.x). The
 *      rows between the GENERATED LEGACY MAP markers hold that
 *      spec's answers — one per (name, val), plus a default row for
 *      each of the five names that decode by formula rather than
 *      table, covering every val their rows do not claim
 *      (decisions.md D24); tools/ref_convert.py re-pins them on
 *      every battery run. An entry neither table holds keeps its
 *      name and no properties — the reference's own pass-through
 *      with `block_data` dropped by the unknown-block policy. A
 *      `val` that is not a number is refused; the caller reports
 *      and carries air.
 *
 *   2. The name half is a table. Java spells some of these blocks
 *      differently, and the spelling can hang off one state value
 *      (`minecraft:sand` with red sand is `minecraft:red_sand`, a
 *      log's `old_log_type`, a torch's wall direction) or off a
 *      pair of values (a dead coral's colour, a cauldron's liquid
 *      at a fill level). The rows between the GENERATED NAME MAP
 *      markers in translate.c were enumerated against
 *      PyMCTranslate for every block name a real world holds, and
 *      every combination the world contains plus every small state
 *      product was checked against the reference before the table
 *      was written (decisions.md D17); tools/ref_convert.py
 *      re-pins it on every battery run. A name the table does not
 *      answer keeps its own spelling — the reference's answer for
 *      a block it does not rename.
 *
 *   3. The states half is a table too (decisions.md D27): the rows
 *      between the GENERATED STATE MAP markers answer (block, state,
 *      value) — which Java property the state carries and with what
 *      value, measured through PyMCTranslate's own answers —
 *      tag-type strict the way the reference is (the value must
 *      carry the spec's own tag type). Every matching row applies,
 *      in table order, later rows overwriting earlier ones on a
 *      shared key. A row carries only values that differ from the
 *      default an absent property stands for — the reference's own
 *      empty-cell answer — so `axis` "y", a furnace's `lit` and a
 *      wire's sides all stay absent, which is the same answer.
 *      Values are the reference's raw ones: the wire's `power` at
 *      signal 1 is "1", never a canon "true". A two-condition row
 *      between the GENERATED STATE RESCUE markers answers the cells
 *      whose composition alone cannot (hanging signs, doors, stems,
 *      walls): first match overwrites and stops.
 *
 *   4. A state no row answers is DROPPED, and the block takes the
 *      default an absent property stands for — the reference's own
 *      empty-cell answer, what props_match judges absence by (a
 *      furnace's `facing_direction` is not its 1.20.80 state and
 *      stays absent; farmland's `moisture` answers the default at
 *      every value). This is what keeps mcme's output in agreement
 *      with the reference rather than inventing mappings the
 *      reference does not have. There is no block registry here on
 *      purpose: the map is exactly the cases the reference has been
 *      asked for, and tools/ref_convert.py pins the rows against
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
 * or carries a `val` that is not a number — mcme_last_error() says which.
 * A caller converting a whole layer reports the drop rather than
 * failing the run (decisions.md#D12).
 */
McmeTag *mcme_translate_block(McmeArena *arena, const McmeTag *entry);

/*
 * The Java spelling of a Bedrock biome id, from the table between the
 * GENERATED BIOME MAP markers: the reference's own answer for the id
 * at the versions above, enumerated from its tables, re-pinned row by
 * row by tools/ref_convert.py.
 *
 * `out` receives a string the table owns — a literal, no arena, valid
 * as long as the table does. Returns false for an id the table does
 * not name: Bedrock ids past what this bank registers. The caller
 * decides what an unnamed id becomes; the reference answers plains,
 * and the count of cells that took that fallback belongs in the
 * report (decisions.md#D19).
 */
bool mcme_translate_biome(int32_t id, McmeStr *out);

#endif /* MCME_TRANSLATE_H */
