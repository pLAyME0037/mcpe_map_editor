#!/usr/bin/env python3
"""
Writes Bedrock subchunk payload fixtures, encoded by Amulet's own writer.

The LevelDB store layer can say what a world holds; "chunk writing" needs the
bytes under a 0x2f / 0x2c key decoded into blocks and, next, encoded back.
This tool forges the payloads that pin the *decode* half: thirteen cases
covering every paletted-storage width the game writes, both wrappers that
carry a storage count, the legacy single-layer wrapper, multi-layer
(waterlogged) subchunks, and the bits==0 oddity.

The layers are produced by amulet-core 1.9.50's own encoder —
`LevelDB8Interface._save_palette_subchunk`, the code path that writes worlds
Minecraft loads — so the bytes are the reference's, not our reading of a
spec. The wrappers (version byte, storage count, Y index) are three
documented bytes assembled here by hand.

Two things cannot come out of amulet's encoder and are forged deliberately:

  * bits == 0. Amulet floors its encoder at one bit per index
    (min_bit_size=1), so it never *writes* a single-entry layer compactly.
    Real worlds do contain them (any fully submerged 16x16x16 volume is one
    block type), and both independent disk readers — amulet's own decoder
    and mcbe-leveldb — agree on the layout: header byte 0x00, then the one
    NBT palette entry, with NO palette-length field. The wiki text can be
    read either way; the two battle-tested readers cannot. This fixture is
    what pins our decoder to them.
  * The version-1 wrapper (`\\x01` + one layer, no count byte) with legacy
    `{name, val}` palette entries, the pre-1.13 block representation.

Before anything is dumped, every payload is decoded back through amulet's
own `_load_palette_blocks` and required to reproduce the designed indices
and palette entry-for-entry. If the forgery is wrong, the fixture is
worthless — so, as with ref_leveldb_table.py, nothing is written until the
reference agrees with itself.

Each case is dumped three ways:
  <case>.bin   the payload, byte for byte
  <case>.idx   every layer's 4096 indices as raw little-endian uint16 in
               XZY order (x*256 + z*16 + y), layers concatenated
  <case>.txt   the expectations: version, layer shape, stored width per
               layer, and every palette entry as the compact SNBT amulet_nbt
               itself emits — which is what mcme's compact SNBT must match
               character for character

Usage:
    venv/bin/python tools/ref_bedrock_subchunk.py \
        --out tests/golden/subchunks
"""

from __future__ import annotations

import argparse
import os
import struct
import sys

import numpy
from amulet_nbt import ByteTag, CompoundTag, IntTag, NamedTag, ShortTag, StringTag

from amulet.level.formats.leveldb_world.interface.chunk.leveldb_8 import (
    LevelDB8Interface,
)

BLOCKS = 4096


def _log(msg: str) -> None:
    print(f"ref_bedrock_subchunk: {msg}", file=sys.stderr)


# ---------------------------------------------------------------------------
# Palette entries. Root names are empty because that is what the game writes;
# NamedTag defaults to that, and the verifier asserts it.
# ---------------------------------------------------------------------------

def block(name: str, states: dict | None = None, version: int = 17694720) -> NamedTag:
    return NamedTag(
        CompoundTag(
            {
                "name": StringTag(name),
                "states": CompoundTag(states or {}),
                "version": IntTag(version),
            }
        )
    )


def legacy_block(name: str, val: int = 0) -> NamedTag:
    return NamedTag(CompoundTag({"name": StringTag(name), "val": ShortTag(val)}))


def synthetic(state_count: int) -> list[NamedTag]:
    """`state_count` distinct entries; real names where cheap, synthetic
    beyond, because the format does not care and distinctness does."""
    return [
        block(f"mcme:fixture_{i:03d}", {"variant": IntTag(i)})
        for i in range(state_count)
    ]


# ---------------------------------------------------------------------------
# Index design. Amulet's encoder takes its array indexed [x][y][z] and does
# swapaxes(1,2).ravel() internally to reach the disk's XZY order (y fastest),
# which is what the wiki, Tomcc's gist and mcbe-leveldb all document. The .idx
# files therefore store the swapaxes ravel, and the verifier compares slot for
# slot against amulet's decoder output, which shares the [x][y][z] convention.
#
# Getting this backwards is invisible to the small palettes: patterns modulo
# 2, 4 and 8 collapse x+z+y-style and survive a z/y transpose. Modulo 32 does
# not, which is exactly why the case list spans every width — the asymmetric
# cases are what caught the transposition.
# ---------------------------------------------------------------------------

def pattern(seed: int, palette_len: int) -> numpy.ndarray:
    axis = numpy.arange(16)
    gx, gy, gz = numpy.meshgrid(axis, axis, axis, indexing="ij")
    return ((gx * 7 + gy * 29 + gz * 13 + seed) % palette_len).astype(numpy.uint32)


def plane(seed: int, modulus: int) -> numpy.ndarray:
    """A scattered 0/1 field — water in the gaps — for the second layer."""
    axis = numpy.arange(16)
    gx, gy, gz = numpy.meshgrid(axis, axis, axis, indexing="ij")
    return (((gx * 3 + gy * 7 + gz * 5 + seed) % modulus) == 0).astype(numpy.uint32)


def disk_order(arr: numpy.ndarray) -> numpy.ndarray:
    """The array in disk order: XZY, y fastest — what .idx files hold."""
    return arr.swapaxes(1, 2).ravel()


def amulet_bits(arr: numpy.ndarray) -> int:
    """The width amulet's encoder picks, restated here so the expectations
    file states it: bit_length of the largest index, floored at 1, with the
    7 and 9..15 gaps snapped up to 8 and 16."""
    bits = max(int(numpy.amax(arr)).bit_length(), 1)
    if bits == 7:
        bits = 8
    elif 9 <= bits <= 15:
        bits = 16
    return bits


# ---------------------------------------------------------------------------
# The cases.
# ---------------------------------------------------------------------------

# (layer indices array, layer palette); None indices means a forged bits==0
# layer. A case is (name, payload, [(indices-or-None, palette), ...], cy).
def design_cases(iface: LevelDB8Interface) -> list[tuple]:
    cases = []

    def encode_layers(layers):
        out = b""
        for indices, palette in layers:
            if indices is None:
                # The forged compact form: header 0x00, one NBT entry, no
                # length field — the layout both disk readers agree on.
                assert len(palette) == 1
                out += b"\x00" + palette[0].save_to(
                    compressed=False, little_endian=True
                )
            else:
                out += iface._save_palette_subchunk(indices, palette)
        return out

    # --- Every width, one case each. Palette sizes are chosen so the largest
    #     index needs exactly that width: 2 entries -> 1 bit, 4 -> 2, ...,
    #     130 -> 8 (bit_length 8), 257 -> 16 (bit_length 9 snaps to 16). ---
    small = [
        block("minecraft:stone"),
        block("minecraft:dirt"),
        block("minecraft:grass_block", {"snowy": StringTag("false")}),
        block("minecraft:oak_log", {"pillar_axis": StringTag("x")}),
        block(
            "minecraft:redstone_wire",
            {
                "north": StringTag("side"),
                "south": StringTag("side"),
                "powered": ByteTag(1),
            },
        ),
        block("minecraft:furnace", {"facing_direction": IntTag(2),
                                    "lit": ByteTag(0)}),
        block("minecraft:water"),
        block("minecraft:sand", {"sand_type": StringTag("red")}),
        block("minecraft:gravel"),
        block("minecraft:cobblestone"),
        block("minecraft:oak_planks"),
        block("minecraft:birch_log", {"pillar_axis": StringTag("y")}),
        block("minecraft:spruce_planks"),
        block("minecraft:torch", {"torch_facing_direction": IntTag(1)}),
        block("minecraft:farmland", {"moisture": IntTag(7)}),
        block("minecraft:glowstone"),
    ]
    for bits, size, seed in ((1, 2, 1), (2, 4, 2), (3, 8, 3), (4, 16, 4),
                             (5, 32, 5), (6, 64, 6)):
        palette = small[:size] if size <= len(small) else (
            small + synthetic(size - len(small))
        )
        indices = pattern(seed, size)
        layers = [(indices, palette)]
        payload = b"\x08" + bytes([1]) + encode_layers(layers)
        cases.append((f"v8-bits{bits}", payload, layers, None))

    for bits, size, seed in ((8, 130, 8), (16, 257, 16)):
        palette = synthetic(size)
        indices = pattern(seed, size)
        layers = [(indices, palette)]
        payload = b"\x08" + bytes([1]) + encode_layers(layers)
        cases.append((f"v8-bits{bits}", payload, layers, None))

    # --- bits == 0: one block fills the subchunk; forged, see the docstring.
    one = [block("minecraft:stone")]
    layers = [(None, one)]
    payload = b"\x08" + bytes([1]) + encode_layers(layers)
    cases.append(("v8-bpb0", payload, layers, None))

    # --- Two layers: terrain below, water hanging in the gaps above — the
    #     waterlogging shape the storage count exists for. ---
    terrain = small[:3]
    fluid = [block("minecraft:water"), block("minecraft:air")]
    layers = [(pattern(11, 3), terrain), (plane(13, 5), fluid)]
    payload = b"\x08" + bytes([2]) + encode_layers(layers)
    cases.append(("v8-waterlogged", payload, layers, None))

    # --- Version 9: the wrapper carries the subchunk's own Y index, which
    #     1.18.2+ worlds rely on because the overworld gained negative Y. ---
    layers = [(pattern(40, 4), small[:4])]
    payload = b"\x09" + bytes([1, 4]) + encode_layers(layers)
    cases.append(("v9-cy4", payload, layers, 4))

    layers = [(pattern(41, 8), small[:8])]
    payload = b"\x09" + bytes([1, (-4) & 0xFF]) + encode_layers(layers)
    cases.append(("v9-cyneg4", payload, layers, -4))

    # --- Version 1: no count byte, one layer, and the pre-1.13 {name, val}
    #     palette shape that outlived the wrapper that wrote it. ---
    legacy = [legacy_block("minecraft:stone"), legacy_block("minecraft:dirt"),
              legacy_block("minecraft:log", 4), legacy_block("minecraft:grass", 1)]
    layers = [(pattern(100, 4), legacy)]
    payload = b"\x01" + encode_layers(layers)
    cases.append(("v1-legacy", payload, layers, None))

    return cases


# ---------------------------------------------------------------------------
# Verification: read every payload back through amulet's decoder.
# ---------------------------------------------------------------------------

def verify(iface: LevelDB8Interface, name: str, payload: bytes, layers, cy) -> None:
    if payload[0] == 1:
        count, rest, got_cy = 1, payload[1:], None
    elif payload[0] == 8:
        count, rest, got_cy = payload[1], payload[2:], None
    elif payload[0] == 9:
        count, rest = payload[1], payload[3:]
        got_cy = struct.unpack("b", payload[2:3])[0]
    else:
        raise AssertionError(f"{name}: wrapper version {payload[0]}")

    assert count == len(layers), f"{name}: {count} layers, designed {len(layers)}"
    if cy is not None:
        assert got_cy == cy, f"{name}: Y index {got_cy}, designed {cy}"

    data = rest
    for i, (designed_indices, designed_palette) in enumerate(layers):
        blocks, palette, data = iface._load_palette_blocks(data)
        # _load_palette_blocks materialises a zeros array for a bits-0 layer
        # (the None lives only inside _decode_packed_array), so the two cases
        # collapse to one comparison against the designed XZY ravel.
        want = (numpy.zeros(BLOCKS, dtype=numpy.uint32)
                if designed_indices is None else designed_indices.ravel())
        assert numpy.array_equal(blocks.ravel(), want), \
            f"{name} layer {i}: indices differ"
        got = [t.save_to(compressed=False, little_endian=True) for t in palette]
        want = [t.save_to(compressed=False, little_endian=True)
                for t in designed_palette]
        assert got == want, f"{name} layer {i}: palette differs"
        for t in palette:
            assert t.name == "", f"{name}: palette root name {t.name!r}"

    assert data == b"", f"{name}: {len(data)} trailing bytes"


# ---------------------------------------------------------------------------
# Dumping.
# ---------------------------------------------------------------------------

def dump(out_dir: str, name: str, payload: bytes, layers, cy) -> None:
    base = os.path.join(out_dir, name)
    with open(base + ".bin", "wb") as fh:
        fh.write(payload)

    idx = bytearray()
    lines = [f"version {payload[0]}", f"layers {len(layers)}"]
    if payload[0] == 9:
        lines.append(f"index {cy}")
    for i, (indices, palette) in enumerate(layers):
        if indices is None:
            flat = numpy.zeros(BLOCKS, dtype=numpy.uint32)
            bits = 0
        else:
            flat = disk_order(indices)   # XZY, y fastest — the disk's order
            bits = amulet_bits(indices)
        for v in flat:
            idx += struct.pack("<H", int(v))
        lines.append(f"layer {i} bits {bits} palette {len(palette)} "
                     f"offset {i * BLOCKS * 2}")
        for j, tag in enumerate(palette):
            lines.append(f"snbt {j} {tag.to_snbt()}")
    with open(base + ".idx", "wb") as fh:
        fh.write(bytes(idx))
    with open(base + ".txt", "w", encoding="utf-8") as fh:
        fh.write("# generated by tools/ref_bedrock_subchunk.py "
                 "(amulet 0.10.64 encoder, read back by its decoder)\n")
        fh.write("\n".join(lines) + "\n")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", default="tests/golden/subchunks")
    args = ap.parse_args()

    iface = LevelDB8Interface()
    cases = design_cases(iface)

    # Verify everything before writing anything: a fixture that does not
    # survive the reference's own decoder is not a fixture.
    for name, payload, layers, cy in cases:
        verify(iface, name, payload, layers, cy)
        _log(f"{name}: {len(payload)} bytes, verified against the decoder")

    os.makedirs(args.out, exist_ok=True)
    for name, payload, layers, cy in cases:
        dump(args.out, name, payload, layers, cy)

    manifest = os.path.join(args.out, "MANIFEST.txt")
    with open(manifest, "w", encoding="ascii") as fh:
        fh.write("# Bedrock subchunk payload cases, one name per line; each\n"
                 "# has <name>.bin (payload), <name>.idx (XZY uint16 LE\n"
                 "# indices per layer) and <name>.txt (expectations)\n")
        fh.write("# generated by tools/ref_bedrock_subchunk.py\n")
        for name, _payload, _layers, _cy in cases:
            fh.write(f"{name}\n")
    _log(f"{len(cases)} cases -> {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
