#!/usr/bin/env python3
"""
Cross-check: mcme and amulet read and write the same block entity
record.

The C selftest pins `mcme_bedrock_block_entity` and the write pair
against the fixture this tree generates. This tool proves them
against the REFERENCE: it copies the golden entity store, then

  READ  — asks mcme for the entity at a block and reads the same
          chunk record with amulet-core 1.9.50's own loader,
          `_unpack_nbt_list`, the code path that reads worlds
          Minecraft loads, requiring the two to agree on the WHOLE
          document, field for field;

  WRITE — asks mcme to store or remove a document and reads the
          record back with the reference, byte for byte: the
          record after the edit must be the record before it with
          only the edited document's extent exchanged, because
          "the documents beside the edit are untouched" is a claim
          about bytes, not about fields.

Covered:

  * a chest with two item stacks, a sign's text, a furnace's
    burn state, and a chest in another dimension
  * several documents in one record: the read is a search by the
    coordinates each document carries, and the answer is the one
    that matches
  * a document with no coordinates is not matched to a position
  * the record belongs to the chunk the coordinates fall in: the
    same local block one chunk over is not that entity
  * no record at that block is exit 2, a record the game deleted
    is exit 2, and a record that will not decode is exit 3
  * setting a document whose own coordinates are absent: the
    position argument is what lands in the stored document
  * a document whose id names no namespace is refused before the
    store changes
  * setting into a chunk with no record creates one, and removing
    its last document removes the record
  * a record that will not decode is one mcme will not rewrite,
    and the refusals write no bytes

The comparison is the whole tree rather than a few named fields:
each side is reduced to plain Python (dicts, lists, strings,
numbers) and compared for equality, so a reader that renames a
field, drops one, or reads a byte as something else fails here.

Usage:
    venv/bin/python tools/ref_bedrock_entity.py --mcme ./mcme
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile

FIXTURE_WORLD = "tests/golden/worlds/bedrock-entities"


def _log(msg: str) -> None:
    print(f"ref_bedrock_entity: {msg}", file=sys.stderr)


def record_key(x: int, z: int, dimension: int = 0) -> bytes:
    k = struct.pack("<ii", x, z)
    if dimension != 0:
        k += struct.pack("<i", dimension)
    return k + bytes([0x31])


def plain(value):
    """Anything amulet's own decode gives, as plain Python: NBT
    tags unwrap through py_data, dicts and lists recurse, byte
    arrays become lists of numbers the way JSON has to hold
    them."""
    py_data = getattr(value, "py_data", None)
    if py_data is not None and not isinstance(value, (dict, list)):
        return plain(py_data)
    if isinstance(value, dict):
        return {k: plain(v) for k, v in value.items()}
    if isinstance(value, (list, tuple)):
        return [plain(v) for v in value]
    if isinstance(value, (bytes, bytearray)):
        return list(value)
    return value


def field(comp, key):
    """A compound's entry, or None: amulet_nbt compounds answer
    `in` and `[]` like a dict."""
    return comp[key] if key in comp else None


def coords(comp) -> tuple[int, int, int]:
    """A document's block, or the far-off sentinel: coordinates
    are ints or they are not coordinates, and a document without
    them matches no position — the reference's own behaviour."""
    far = -1 << 30
    return (
        int(getattr(field(comp, "x"), "py_int", far)),
        int(getattr(field(comp, "y"), "py_int", far)),
        int(getattr(field(comp, "z"), "py_int", far)),
    )


def documents(iface, payload: bytes):
    """The record's documents, decoded by the reference."""
    out = []
    for doc in iface._unpack_nbt_list(payload):
        comp = doc.tag
        cx, cy, cz = coords(comp)
        out.append(
            {
                "id": str(getattr(field(comp, "id"), "py_str", "")),
                "x": cx,
                "y": cy,
                "z": cz,
                "doc": plain(comp.py_data),
            }
        )
    return out


def stored_at(iface, payload: bytes, x: int, y: int, z: int):
    """The documents claiming that block — one, unless the record
    is stranger than a stored world should be."""
    return [
        d for d in documents(iface, payload)
        if (d["x"], d["y"], d["z"]) == (x, y, z)
    ]


def spans(payload: bytes):
    """Each document's byte extent in the record: where it starts
    and how many bytes it occupies, read the way the reference
    reads — load_nbt with a ReadContext, whose offset is where the
    next document starts. A splice is checked against these."""
    from amulet_nbt import ReadContext, load, utf8_escape_decoder

    out = []
    at = 0
    raw = payload
    while raw:
        ctx = ReadContext()
        tag = load(raw, little_endian=True, read_context=ctx,
                   string_decoder=utf8_escape_decoder)
        if ctx.offset <= 0:
            raise AssertionError("the reference made no progress")
        out.append((at, ctx.offset, tag))
        at += ctx.offset
        raw = raw[ctx.offset:]
    if at != len(payload):
        raise AssertionError(
            f"the reference consumed {at} of {len(payload)} bytes")
    return out


def extent_at(payload: bytes, x: int, y: int, z: int):
    """The bytes of the document at that block — the only extent
    an edit is allowed to exchange."""
    found = None
    for at, length, tag in spans(payload):
        if coords(tag.tag) == (x, y, z):
            if found is not None:
                raise AssertionError(
                    f"two documents claim block ({x}, {y}, {z})")
            found = (at, length)
    if found is None:
        raise AssertionError(
            f"no document at block ({x}, {y}, {z}) to edit")
    return found


def check_splice(name: str, before: bytes, after: bytes, span,
                 removed: bool = False) -> None:
    """The record after the edit is `before` with only the edited
    document's extent exchanged.

    For a removal the answer is exactly prefix + suffix. For a
    replacement the middle is whatever mcme wrote — its length is
    derived from the two records, never assumed — and everything
    either side of it must be the bytes it was, which is what
    makes "the sign beside the chest is untouched" true rather
    than merely plausible."""
    at, length = span
    if removed:
        if after != before[:at] + before[at + length:]:
            raise AssertionError(
                f"{name}: a deletion rewrote more than the "
                "document it removed")
        return
    kept = len(before) - length
    middle = len(after) - kept
    if middle < 0:
        raise AssertionError(
            f"{name}: the record shrank by more than the document "
            "it replaced")
    if after[:at] != before[:at] or after[at + middle:] != before[at + length:]:
        raise AssertionError(
            f"{name}: bytes outside the edited document changed")


def main() -> int:
    ap = argparse.ArgumentParser(
        description="mcme and amulet read and write one record"
    )
    ap.add_argument("--mcme", required=True, help="path to mcme")
    ap.add_argument(
        "--world",
        default=FIXTURE_WORLD,
        help="golden world whose store is the starting point",
    )
    args = ap.parse_args()

    from amulet.libs.leveldb import LevelDB
    from amulet.level.formats.leveldb_world.interface.chunk.leveldb_8 import (
        LevelDB8Interface,
    )

    iface = LevelDB8Interface()
    checks = 0

    tmp = tempfile.mkdtemp(prefix="mcme_entity_ref_")
    world = os.path.join(tmp, "world")
    shutil.copytree(args.world, world)
    db_path = os.path.join(world, "db")

    def run_entity(x: int, y: int, z: int, dimension: int = 0):
        cmd = [args.mcme, "--json", "chunk", "entity", world,
               str(x), str(y), str(z)]
        if dimension != 0:
            cmd += ["--dimension", str(dimension)]
        r = subprocess.run(cmd, capture_output=True, text=True)
        return r.returncode, r.stdout.strip(), r.stderr.strip()

    def run_set(x: int, y: int, z: int, snbt: str):
        cmd = [args.mcme, "chunk", "entity", "set", world,
               str(x), str(y), str(z), snbt]
        r = subprocess.run(cmd, capture_output=True, text=True)
        return r.returncode, r.stdout.strip(), r.stderr.strip()

    def run_del(x: int, y: int, z: int):
        cmd = [args.mcme, "chunk", "entity", "del", world,
               str(x), str(y), str(z)]
        r = subprocess.run(cmd, capture_output=True, text=True)
        return r.returncode, r.stdout.strip(), r.stderr.strip()

    def raw(cx: int, cz: int, dimension: int = 0):
        """The record's bytes as the store holds them, or None."""
        db = LevelDB(db_path)
        try:
            try:
                return db.get(record_key(cx, cz, dimension))
            except KeyError:
                return None
        finally:
            db.close()

    def expect_entity(name: str, x: int, y: int, z: int,
                      dimension: int = 0):
        nonlocal checks
        payload = raw(x // 16, z // 16, dimension)
        here = [] if payload is None else stored_at(
            iface, payload, x, y, z)

        rc, out, err = run_entity(x, y, z, dimension)
        if not here:
            # No record for the chunk, or none of its documents at
            # that block: either way there is no entity there.
            if rc != 2:
                raise AssertionError(
                    f"{name}: no document at that block, so exit "
                    f"{rc} not 2 ({err})")
            checks += 1
            _log(f"{name}: nothing at that block, exit 2")
            return
        if rc != 0:
            raise AssertionError(f"{name}: mcme exit {rc}: {err}")
        if len(here) != 1:
            raise AssertionError(
                f"{name}: the reference found {len(here)} "
                "documents at that block")
        if json.loads(out) != here[0]["doc"]:
            raise AssertionError(
                f"{name}: mcme read {out!r}, reference read "
                f"{here[0]['doc']!r}")
        checks += 1
        _log(f"{name}: whole document agrees "
             f"(id {here[0]['id']!r})")

    # --- The chest: the first of three documents in one record.
    expect_entity("chest", 12, 64, 3)
    # --- The sign: the second document of the same record.
    expect_entity("sign", 0, 65, 0)
    # --- The coordinateless document is matched to no position.
    expect_entity("no document at that block", 12, 64, 4)
    # --- A furnace in another chunk, at a negative chunk
    #     coordinate.
    expect_entity("furnace", 16, 8, -32)
    # --- Same local block, one chunk over: not that entity.
    expect_entity("same local block, other chunk", 12, 64, 19)
    # --- Another dimension's own record.
    expect_entity("nether chest", 0, 5, 0, dimension=1)

    # = The write half: mcme writes, the reference reads — the
    #   direction the read half ran the other way round.

    # --- set: replace the chest. The document carries no
    #     coordinates, so the position argument is the only thing
    #     that can land in it.
    before = raw(0, 0)
    span = extent_at(before, 12, 64, 3)
    rc, out, err = run_set(
        12, 64, 3,
        '{id: "minecraft:chest", CustomName: "stores", '
        'Items: [{Slot: 0b, id: "minecraft:diamond", '
        'count: 3b, damage: 0s}]}')
    if rc != 0 or out:
        raise AssertionError(
            f"set the chest: exit {rc}, stdout {out!r} ({err})")
    checks += 1
    _log("set: exit 0, no output")

    after = raw(0, 0)
    check_splice("set", before, after, span)
    checks += 1
    _log("set: the record is prefix + this document + suffix")

    match = stored_at(iface, after, 12, 64, 3)
    if len(match) != 1:
        raise AssertionError(
            f"set: the reference finds {len(match)} documents at "
            "the block mcme wrote, want 1")
    doc = match[0]["doc"]
    if (doc.get("id"), doc.get("CustomName")) != (
            "minecraft:chest", "stores"):
        raise AssertionError(f"set: the reference read {doc!r}")
    checks += 1
    _log("set: the position's coordinates are what mcme stored")
    expect_entity("chest after an edit", 12, 64, 3)
    # --- The sign behind it reads as it did before the edit.
    expect_entity("sign after an edit", 0, 65, 0)

    # --- del: only the sign's bytes leave the record.
    before = raw(0, 0)
    span = extent_at(before, 0, 65, 0)
    rc, out, err = run_del(0, 65, 0)
    if rc != 0 or out:
        raise AssertionError(
            f"del the sign: exit {rc}, stdout {out!r} ({err})")
    checks += 1
    _log("del: exit 0, no output")

    after = raw(0, 0)
    check_splice("del", before, after, span, removed=True)
    checks += 1
    _log("del: the record is prefix + suffix, byte for byte")
    if len(documents(iface, after)) != 2:
        raise AssertionError(
            "del: the reference reads other than 2 documents "
            "after removing one")
    checks += 1
    expect_entity("chest after a deletion", 12, 64, 3)
    expect_entity("the deleted sign", 0, 65, 0)

    # --- A document mcme will not write: the reference drops one
    #     whose id names no namespace, so mcme refuses it and the
    #     record does not move.
    before = raw(0, 0)
    rc, out, err = run_set(12, 64, 3, '{id: "chest"}')
    if rc != 1:
        raise AssertionError(
            f"an id with no namespace: exit {rc}, want 1 ({err})")
    if raw(0, 0) != before:
        raise AssertionError("a refused write changed the record")
    checks += 1
    _log("an id with no namespace: exit 1, record untouched")

    # --- A chunk that holds no record at all gets one.
    if raw(2, 2) is not None:
        raise AssertionError(
            "the fixture already has a record in chunk (2, 2)")
    rc, out, err = run_set(
        40, 10, 40, '{id: "minecraft:sign", Text: "written"}')
    if rc != 0 or out:
        raise AssertionError(
            f"set into an empty chunk: exit {rc} ({err})")
    created = raw(2, 2)
    if created is None:
        raise AssertionError("no record was created for chunk (2, 2)")
    match = stored_at(iface, created, 40, 10, 40)
    if len(match) != 1 or match[0]["doc"].get("Text") != "written":
        raise AssertionError(
            f"the reference read {match!r} from the new record")
    checks += 1
    _log("set: a chunk with no record gets one")
    expect_entity("an entity in a chunk that had none", 40, 10, 40)

    # --- Its last document's record goes with it.
    rc, out, err = run_del(40, 10, 40)
    if rc != 0 or out:
        raise AssertionError(
            f"del the last document: exit {rc} ({err})")
    if raw(2, 2) is not None:
        raise AssertionError(
            "the record survived its last document")
    checks += 1
    _log("del: the last document's record is removed, not emptied")
    expect_entity("after the record's last document", 40, 10, 40)
    rc, out, err = run_del(40, 10, 40)
    if rc != 2:
        raise AssertionError(
            f"del twice: exit {rc}, want 2 ({err})")
    checks += 1
    _log("del: a second time is exit 2")

    # --- A record the game deleted is an absence, not a fault.
    db = LevelDB(db_path)
    try:
        db.delete(record_key(0, 0))
    finally:
        db.close()
    expect_entity("after a deletion", 12, 64, 3)

    # --- A record that will not decode is exit 3 for a read —
    #     and one mcme will not rewrite either.
    db = LevelDB(db_path)
    try:
        db.put(record_key(0, 0), b"\x0a\x00\x03id\x08\x00\x04xx")
    finally:
        db.close()
    rc, out, err = run_entity(12, 64, 3)
    if rc != 3:
        raise AssertionError(
            f"junk record: exit {rc}, want 3 ({err})")
    checks += 1
    _log("junk record: exit 3")

    before = raw(0, 0)
    rc, out, err = run_set(12, 64, 3, '{id: "minecraft:chest"}')
    if rc != 3:
        raise AssertionError(
            f"set into a junk record: exit {rc}, want 3 ({err})")
    rc, out, err = run_del(12, 64, 3)
    if rc != 3:
        raise AssertionError(
            f"del into a junk record: exit {rc}, want 3 ({err})")
    if raw(0, 0) != before:
        raise AssertionError("refused writes changed the record")
    checks += 1
    _log("junk record: set and del exit 3, record untouched")

    shutil.rmtree(tmp, ignore_errors=True)
    _log(f"ok — {checks} checks")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as exc:
        _log(f"FAIL: {exc}")
        sys.exit(1)
