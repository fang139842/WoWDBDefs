#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
m2_wotlk_to_tbc.py
==================

Convert a WotLK M2 model file (version 264) into a TBC-compatible M2 file
(version 260).

The three major differences handled by this script:

1. **Header layout**.  The TBC v260 M2 header is 324 bytes long, while the
   WotLK v264 header is 304 bytes.  TBC adds three fields that WotLK
   removed:

     * ``M2Array<M2SequenceFallback> playable_animation_lookup``
       (inserted before ``bones`` at WotLK offset ``0x02C``)
     * ``uint32 ofsViews``
       (inserted right after ``nViews`` at WotLK offset ``0x048``;
       in WotLK the field at ``0x044`` is just ``num_skin_profiles``)
     * ``M2Array<M2TextureFlipbook> texture_flipbooks``
       (inserted between ``texture_weights`` and ``texture_transforms``
       at WotLK offset ``0x060``)

   Because the header grew by 20 bytes, **every** offset stored anywhere in
   the data section must be shifted by +20.

2. **Embedded skin profiles**.  WotLK keeps view (LOD) data in external
   ``{model}{i:02d}.skin`` files.  TBC requires the skin profiles to live
   inside the M2 itself (pointed at by ``ofsViews``).  The script reads
   each ``.skin`` file, embeds the whole thing at the end of the M2, and
   writes a contiguous ``M2SkinProfile`` array whose entries reference the
   embedded data.

3. **Embedded animation data**.  WotLK can store animation key-frame data
   in external ``{model}{animID:04d}-{subID:02d}.anim`` files.  Sequences
   whose ``flags`` bit ``0x20`` is **clear** keep their key-frame data in
   an external ``.anim`` file (per pywowlib / wowdev.wiki: ``0x20`` set =
   "primary bone sequence; data is in the m2", ``0x20`` clear = "data is
   in an .anim file").  The script appends each ``.anim`` file to the M2,
   patches the matching inner array offsets in every ``M2Track``, and
   sets bit ``0x20`` on the now-internal sequences.

The script only depends on the Python standard library and is invoked as::

    python tools/m2_wotlk_to_tbc.py input.m2 output.m2

All ``.skin`` and ``.anim`` files must live in the same directory as the
input ``.m2``.  Authoritative format references:
https://wowdev.wiki/M2 and https://wowdev.wiki/M2/.skin .
"""

from __future__ import annotations

import argparse
import os
import struct
import sys
from typing import Callable, Dict, List, Optional, Set, Tuple

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

WOTLK_VERSION = 264
TBC_VERSION = 260

WOTLK_HEADER_SIZE = 304
TBC_HEADER_SIZE = 324
HEADER_SHIFT = TBC_HEADER_SIZE - WOTLK_HEADER_SIZE  # 20

# 0x20 = `looped_animation` / "primary bone sequence" / "data stored in m2"
# (set: data is in the .m2 file; clear: data is in the external .anim file)
SEQ_FLAG_LOW_PRIORITY = 0x10
SEQ_FLAG_DATA_IN_M2 = 0x20  # primary bone sequence
SEQ_FLAG_IS_ALIAS = 0x40

# WotLK structure sizes (bytes)
SZ_M2_SEQUENCE = 64
SZ_M2_TRACK = 20  # WotLK and later (older versions are 28)
SZ_M2_COMPBONE = 88
SZ_M2_COLOR = 40
SZ_M2_TEXTURE = 16
SZ_M2_TEXTURE_WEIGHT = 20
SZ_M2_TEXTURE_TRANSFORM = 60
SZ_M2_ATTACHMENT = 40
SZ_M2_EVENT = 36
SZ_M2_LIGHT = 156
SZ_M2_CAMERA = 100  # WotLK pre-Cata
SZ_M2_RIBBON = 176
SZ_M2_PARTICLE = 476  # WotLK 264 (older = 492)

# Number of M2Tracks contained inside each parent structure (WotLK layout).
TRACKS_IN_BONE = 3       # translation, rotation, scaling
TRACKS_IN_COLOR = 2      # color, alpha
TRACKS_IN_TEX_TFM = 3    # translation, rotation, scaling
TRACKS_IN_LIGHT = 7
TRACKS_IN_CAMERA = 3

# WotLK .skin files begin with a 4-byte 'SKIN' magic, followed by the
# 44-byte M2SkinProfile header, followed by the data the profile's
# M2Arrays point at.  Offsets stored inside the M2SkinProfile are
# relative to the start of the .skin file (i.e. include the 4 magic
# bytes).  Older clients sometimes ship the header without the magic,
# so the loader auto-detects which form was provided.
SKIN_MAGIC = b"SKIN"
SKIN_PROFILE_SIZE = 44
SKIN_M2ARRAY_FIELD_OFFSETS = (0, 8, 16, 24, 32)  # count fields; offset = +4

# ---------------------------------------------------------------------------
# Header field offsets
# ---------------------------------------------------------------------------


class WotLK:
    """WotLK v264 M2 header field byte offsets (input layout)."""

    MAGIC = 0x000
    VERSION = 0x004
    NAME_COUNT = 0x008
    NAME_OFS = 0x00C
    GLOBAL_FLAGS = 0x010
    GLOBAL_LOOPS_COUNT = 0x014
    GLOBAL_LOOPS_OFS = 0x018
    SEQUENCES_COUNT = 0x01C
    SEQUENCES_OFS = 0x020
    SEQ_LOOKUP_COUNT = 0x024
    SEQ_LOOKUP_OFS = 0x028
    BONES_COUNT = 0x02C
    BONES_OFS = 0x030
    KEYBONE_LOOKUP_COUNT = 0x034
    KEYBONE_LOOKUP_OFS = 0x038
    VERTICES_COUNT = 0x03C
    VERTICES_OFS = 0x040
    NUM_VIEWS = 0x044  # uint32 (no ofsViews in WotLK)
    COLORS_COUNT = 0x048
    COLORS_OFS = 0x04C
    TEXTURES_COUNT = 0x050
    TEXTURES_OFS = 0x054
    TEX_WEIGHTS_COUNT = 0x058
    TEX_WEIGHTS_OFS = 0x05C
    TEX_TRANSFORMS_COUNT = 0x060
    TEX_TRANSFORMS_OFS = 0x064
    TEX_REPLACE_COUNT = 0x068
    TEX_REPLACE_OFS = 0x06C
    MATERIALS_COUNT = 0x070
    MATERIALS_OFS = 0x074
    BONE_LOOKUP_COUNT = 0x078
    BONE_LOOKUP_OFS = 0x07C
    TEX_LOOKUP_COUNT = 0x080
    TEX_LOOKUP_OFS = 0x084
    TEX_UNIT_LOOKUP_COUNT = 0x088
    TEX_UNIT_LOOKUP_OFS = 0x08C
    TRANSPARENCY_LOOKUP_COUNT = 0x090
    TRANSPARENCY_LOOKUP_OFS = 0x094
    TEX_TRANSFORMS_LOOKUP_COUNT = 0x098
    TEX_TRANSFORMS_LOOKUP_OFS = 0x09C
    BBOX = 0x0A0  # CAaBox (24) + bsphere_radius (4) = 28 bytes
    CBOX = 0x0BC  # CAaBox (24) + csphere_radius (4) = 28 bytes
    COLLISION_TRI_COUNT = 0x0D8
    COLLISION_TRI_OFS = 0x0DC
    COLLISION_VERT_COUNT = 0x0E0
    COLLISION_VERT_OFS = 0x0E4
    COLLISION_NORM_COUNT = 0x0E8
    COLLISION_NORM_OFS = 0x0EC
    ATTACHMENTS_COUNT = 0x0F0
    ATTACHMENTS_OFS = 0x0F4
    ATTACH_LOOKUP_COUNT = 0x0F8
    ATTACH_LOOKUP_OFS = 0x0FC
    EVENTS_COUNT = 0x100
    EVENTS_OFS = 0x104
    LIGHTS_COUNT = 0x108
    LIGHTS_OFS = 0x10C
    CAMERAS_COUNT = 0x110
    CAMERAS_OFS = 0x114
    CAMERA_LOOKUP_COUNT = 0x118
    CAMERA_LOOKUP_OFS = 0x11C
    RIBBONS_COUNT = 0x120
    RIBBONS_OFS = 0x124
    PARTICLES_COUNT = 0x128
    PARTICLES_OFS = 0x12C
    END = 0x130  # 304 bytes


def wotlk_to_tbc_field(wotlk_off: int) -> int:
    """Translate a WotLK header byte offset to its TBC equivalent.

    Three insertion points (cumulative offsets):
      * +8  bytes inserted at WotLK 0x02C (playable_animation_lookup)
      * +4  bytes inserted at WotLK 0x048 (ofsViews; sits right after nViews)
      * +8  bytes inserted at WotLK 0x060 (texture_flipbooks)
    """
    if wotlk_off < 0x02C:
        return wotlk_off
    if wotlk_off < 0x048:
        return wotlk_off + 8
    if wotlk_off < 0x060:
        return wotlk_off + 12
    return wotlk_off + 20


# ---------------------------------------------------------------------------
# Tiny binary helpers
# ---------------------------------------------------------------------------


def u32(buf: bytes, off: int) -> int:
    return struct.unpack_from("<I", buf, off)[0]


def u16(buf: bytes, off: int) -> int:
    return struct.unpack_from("<H", buf, off)[0]


def write_u32(buf: bytearray, off: int, value: int) -> None:
    struct.pack_into("<I", buf, off, value & 0xFFFFFFFF)


def read_array_pair(buf: bytes, count_off: int) -> Tuple[int, int]:
    """Return (count, offset) for an M2Array stored at ``count_off``."""
    count = u32(buf, count_off)
    offset = u32(buf, count_off + 4)
    return count, offset


# ---------------------------------------------------------------------------
# Walker — collects every offset position that points into the data section
# ---------------------------------------------------------------------------


class Walker:
    """Walks a WotLK M2 file structure tree and remembers every offset
    location that needs to be patched.

    All positions / offsets stored in :class:`Walker` are **WotLK file
    positions**.  After the walk finishes the caller can apply the
    +``HEADER_SHIFT`` adjustment by writing to the corresponding TBC buffer
    position (which is ``wotlk_pos + HEADER_SHIFT`` for everything in the
    data section).
    """

    def __init__(self, data: bytes, external_seqs: Set[int]) -> None:
        self._data = data
        self._external_seqs = external_seqs

        # All offset positions (in WotLK file coordinates) whose value
        # should be increased by HEADER_SHIFT (the +20 bytes header growth).
        self.shift_positions: List[int] = []

        # External M2Track inner-array offset positions, keyed by sequence
        # index.  After embedding the matching .anim file, the value at
        # each of these positions must be set to anim_base + original.
        self.external_inner: List[Tuple[int, int]] = []

    # ---- low-level scheduling helpers ------------------------------------

    def _schedule_shift(self, pos: int) -> None:
        """Mark a 4-byte offset at WotLK position ``pos`` for +SHIFT."""
        if pos < 0 or pos + 4 > len(self._data):
            raise ValueError(f"Offset position {pos:#x} is out of range")
        if u32(self._data, pos) != 0:
            self.shift_positions.append(pos)

    # ---- M2Track / M2Array walking --------------------------------------

    def _walk_m2track(self, track_pos: int, walk_values: bool = True) -> None:
        """Walk a single 20-byte WotLK ``M2Track``.

        Layout:
          +0  uint16 interpolation_type
          +2  int16  global_sequence
          +4  uint32 nTimestamps  (outer array count = nSequences)
          +8  uint32 ofsTimestamps -> array of {count, offset} entries
          +12 uint32 nValues
          +16 uint32 ofsValues
        """
        # Outer arrays
        self._schedule_shift(track_pos + 8)
        if walk_values:
            self._schedule_shift(track_pos + 16)

        ts_count, ts_offset = read_array_pair(self._data, track_pos + 4)
        if ts_count and ts_offset:
            self._walk_inner_array(ts_offset, ts_count)

        if walk_values:
            vs_count, vs_offset = read_array_pair(self._data, track_pos + 12)
            if vs_count and vs_offset:
                self._walk_inner_array(vs_offset, vs_count)

    def _walk_inner_array(self, base_off: int, count: int) -> None:
        """Walk the per-sequence inner ``M2Array`` array of an M2Track.

        Each entry is 8 bytes: ``{count, offset}``.  Offsets for non-external
        sequences point into the M2 data section and are simply +SHIFTed.
        Offsets for external sequences point into a ``.anim`` file and must
        be replaced with the absolute position once we embed that file.
        """
        for seq_idx in range(count):
            entry_off = base_off + seq_idx * 8
            entry_count = u32(self._data, entry_off)
            if entry_count == 0:
                continue
            inner_offset_pos = entry_off + 4
            inner_offset_val = u32(self._data, inner_offset_pos)
            if seq_idx in self._external_seqs:
                # External entries reference the .anim file; the value can
                # legitimately be 0 (= start of the .anim file).
                self.external_inner.append((inner_offset_pos, seq_idx))
            elif inner_offset_val != 0:
                self.shift_positions.append(inner_offset_pos)

    # ---- top-level structure walkers ------------------------------------

    def walk_compbone(self, off: int) -> None:
        # M2CompBone: 4(key_bone_id)+4(flags)+2(parent)+2(submesh)+
        #             4(name_crc) = 16, then 3 M2Track + 12 (pivot) = 88.
        self._walk_m2track(off + 16)
        self._walk_m2track(off + 36)
        self._walk_m2track(off + 56)

    def walk_color(self, off: int) -> None:
        self._walk_m2track(off + 0)
        self._walk_m2track(off + 20)

    def walk_texture(self, off: int) -> None:
        # type(4) + flags(4) + filename M2Array (8)
        self._schedule_shift(off + 8 + 4)

    def walk_texture_weight(self, off: int) -> None:
        self._walk_m2track(off + 0)

    def walk_texture_transform(self, off: int) -> None:
        self._walk_m2track(off + 0)
        self._walk_m2track(off + 20)
        self._walk_m2track(off + 40)

    def walk_attachment(self, off: int) -> None:
        # id(4)+bone(2)+unk(2)+pos(12) = 20, then M2Track (20) = 40.
        self._walk_m2track(off + 20)

    def walk_event(self, off: int) -> None:
        # ident(4)+data(4)+bone(4)+pos(12)=24, then M2TrackBase (no values)
        # = 12 (actually pywowlib reports M2Event = 36, so trackbase is 12).
        self._walk_m2track(off + 24, walk_values=False)

    def walk_light(self, off: int) -> None:
        # type(2)+bone(2)+pos(12) = 16, then 7 M2Tracks = 140 -> 156 total.
        for i in range(TRACKS_IN_LIGHT):
            self._walk_m2track(off + 16 + i * SZ_M2_TRACK)

    def walk_camera(self, off: int) -> None:
        # type(4)+fov(4)+far(4)+near(4)=16, M2Track(20)=36, base(12)=48,
        # M2Track(20)=68, base(12)=80, M2Track(20)=100.
        self._walk_m2track(off + 16)
        self._walk_m2track(off + 48)
        self._walk_m2track(off + 80)


# ---------------------------------------------------------------------------
# Header construction
# ---------------------------------------------------------------------------


def build_tbc_header(wotlk_data: bytes) -> bytearray:
    """Create a 324-byte TBC v260 header from the WotLK v264 input."""
    header = bytearray(TBC_HEADER_SIZE)

    # Copy each WotLK header byte to its TBC slot, applying +HEADER_SHIFT to
    # offset fields so they keep pointing at the (now-relocated) data.
    pairs: List[Tuple[int, int]] = [
        (WotLK.NAME_COUNT, WotLK.NAME_OFS),
        (WotLK.GLOBAL_LOOPS_COUNT, WotLK.GLOBAL_LOOPS_OFS),
        (WotLK.SEQUENCES_COUNT, WotLK.SEQUENCES_OFS),
        (WotLK.SEQ_LOOKUP_COUNT, WotLK.SEQ_LOOKUP_OFS),
        # playable_animation_lookup goes here in TBC (zero-filled)
        (WotLK.BONES_COUNT, WotLK.BONES_OFS),
        (WotLK.KEYBONE_LOOKUP_COUNT, WotLK.KEYBONE_LOOKUP_OFS),
        (WotLK.VERTICES_COUNT, WotLK.VERTICES_OFS),
        # nViews + ofsViews handled separately
        (WotLK.COLORS_COUNT, WotLK.COLORS_OFS),
        (WotLK.TEXTURES_COUNT, WotLK.TEXTURES_OFS),
        (WotLK.TEX_WEIGHTS_COUNT, WotLK.TEX_WEIGHTS_OFS),
        # texture_flipbooks goes here in TBC (zero-filled)
        (WotLK.TEX_TRANSFORMS_COUNT, WotLK.TEX_TRANSFORMS_OFS),
        (WotLK.TEX_REPLACE_COUNT, WotLK.TEX_REPLACE_OFS),
        (WotLK.MATERIALS_COUNT, WotLK.MATERIALS_OFS),
        (WotLK.BONE_LOOKUP_COUNT, WotLK.BONE_LOOKUP_OFS),
        (WotLK.TEX_LOOKUP_COUNT, WotLK.TEX_LOOKUP_OFS),
        (WotLK.TEX_UNIT_LOOKUP_COUNT, WotLK.TEX_UNIT_LOOKUP_OFS),
        (WotLK.TRANSPARENCY_LOOKUP_COUNT, WotLK.TRANSPARENCY_LOOKUP_OFS),
        (WotLK.TEX_TRANSFORMS_LOOKUP_COUNT, WotLK.TEX_TRANSFORMS_LOOKUP_OFS),
        (WotLK.COLLISION_TRI_COUNT, WotLK.COLLISION_TRI_OFS),
        (WotLK.COLLISION_VERT_COUNT, WotLK.COLLISION_VERT_OFS),
        (WotLK.COLLISION_NORM_COUNT, WotLK.COLLISION_NORM_OFS),
        (WotLK.ATTACHMENTS_COUNT, WotLK.ATTACHMENTS_OFS),
        (WotLK.ATTACH_LOOKUP_COUNT, WotLK.ATTACH_LOOKUP_OFS),
        (WotLK.EVENTS_COUNT, WotLK.EVENTS_OFS),
        (WotLK.LIGHTS_COUNT, WotLK.LIGHTS_OFS),
        (WotLK.CAMERAS_COUNT, WotLK.CAMERAS_OFS),
        (WotLK.CAMERA_LOOKUP_COUNT, WotLK.CAMERA_LOOKUP_OFS),
        (WotLK.RIBBONS_COUNT, WotLK.RIBBONS_OFS),
        (WotLK.PARTICLES_COUNT, WotLK.PARTICLES_OFS),
    ]

    # Magic, version, name length, flags, BBOX/CBOX blocks
    header[0:4] = wotlk_data[WotLK.MAGIC : WotLK.MAGIC + 4]
    write_u32(header, WotLK.VERSION, TBC_VERSION)
    write_u32(header, WotLK.GLOBAL_FLAGS, u32(wotlk_data, WotLK.GLOBAL_FLAGS))

    for count_off, ofs_off in pairs:
        tbc_count = wotlk_to_tbc_field(count_off)
        tbc_ofs = wotlk_to_tbc_field(ofs_off)
        write_u32(header, tbc_count, u32(wotlk_data, count_off))
        old_offset = u32(wotlk_data, ofs_off)
        new_offset = old_offset + HEADER_SHIFT if old_offset else 0
        write_u32(header, tbc_ofs, new_offset)

    # nViews (no offset shift; the value is just a count) + zeroed ofsViews
    n_views = u32(wotlk_data, WotLK.NUM_VIEWS)
    tbc_n_views = wotlk_to_tbc_field(WotLK.NUM_VIEWS)
    write_u32(header, tbc_n_views, n_views)
    write_u32(header, tbc_n_views + 4, 0)  # ofsViews — set later

    # 28-byte BBOX + bsphere_radius and 28-byte CBOX + csphere_radius
    tbc_bbox = wotlk_to_tbc_field(WotLK.BBOX)
    tbc_cbox = wotlk_to_tbc_field(WotLK.CBOX)
    header[tbc_bbox : tbc_bbox + 28] = wotlk_data[WotLK.BBOX : WotLK.BBOX + 28]
    header[tbc_cbox : tbc_cbox + 28] = wotlk_data[WotLK.CBOX : WotLK.CBOX + 28]

    return header


# ---------------------------------------------------------------------------
# Skin embedding
# ---------------------------------------------------------------------------


def _split_skin_blob(blob: bytes, idx: int) -> Tuple[bytes, bytes, int]:
    """Return (profile_bytes, payload_bytes, header_size_in_skin_file).

    ``header_size_in_skin_file`` is the byte offset, **inside the .skin
    file**, where the data section starts — i.e. the value that needs to
    be subtracted when relocating an offset out of the .skin file and into
    the M2.  WotLK skins start with a 4-byte ``'SKIN'`` magic; pre-WotLK
    development builds sometimes ship without it.
    """
    if blob[:4] == SKIN_MAGIC:
        header = 4 + SKIN_PROFILE_SIZE
        if len(blob) < header:
            raise ValueError(
                f"Skin file #{idx} too small ({len(blob)} bytes) for "
                f"SKIN magic + M2SkinProfile"
            )
        return blob[4:header], blob[header:], header
    if len(blob) < SKIN_PROFILE_SIZE:
        raise ValueError(
            f"Skin file #{idx} too small ({len(blob)} bytes) for M2SkinProfile"
        )
    return blob[:SKIN_PROFILE_SIZE], blob[SKIN_PROFILE_SIZE:], SKIN_PROFILE_SIZE


def embed_skin_files(
    out_buf: bytearray,
    skin_blobs: List[bytes],
    verbose: bool = True,
) -> int:
    """Embed all .skin files into ``out_buf`` and return the byte position
    of the contiguous M2SkinProfile array (= TBC ``ofsViews``).

    Algorithm per .skin file:

    1. Detect and strip the optional 4-byte ``'SKIN'`` magic.
    2. Append the data section (everything after the M2SkinProfile) to
       ``out_buf`` at ``data_pos``.
    3. Patch the M2SkinProfile's 5 M2Array offsets so they point at the
       embedded data: ``new = data_pos + (old - skin_header_size)``.
    4. Write the patched 44-byte M2SkinProfile into a contiguous array
       reserved at ``skin_array_pos``.
    """
    if not skin_blobs:
        return 0

    skin_array_pos = len(out_buf)
    out_buf.extend(b"\x00" * (len(skin_blobs) * SKIN_PROFILE_SIZE))

    for i, blob in enumerate(skin_blobs):
        profile_bytes, payload, skin_header_size = _split_skin_blob(blob, i)

        data_pos = len(out_buf)
        out_buf.extend(payload)
        delta = data_pos - skin_header_size

        profile = bytearray(profile_bytes)
        for field_count_off in SKIN_M2ARRAY_FIELD_OFFSETS:
            count = u32(profile, field_count_off)
            ofs = u32(profile, field_count_off + 4)
            if count > 0 and ofs > 0:
                write_u32(profile, field_count_off + 4, ofs + delta)
        slot = skin_array_pos + i * SKIN_PROFILE_SIZE
        out_buf[slot : slot + SKIN_PROFILE_SIZE] = profile

        if verbose:
            magic_note = "with 'SKIN' magic" if skin_header_size == 48 else "no magic"
            print(
                f"  embedded skin #{i} ({magic_note}): profile at "
                f"{slot:#x}, data at {data_pos:#x} ({len(payload)} bytes)"
            )

    return skin_array_pos


# ---------------------------------------------------------------------------
# Animation discovery + embedding
# ---------------------------------------------------------------------------


def parse_sequences(
    wotlk_data: bytes,
) -> List[Dict[str, int]]:
    """Return one dict per animation sequence with keys
    ``index``, ``anim_id``, ``sub_anim_id``, ``flags``, ``seq_offset``.
    """
    n = u32(wotlk_data, WotLK.SEQUENCES_COUNT)
    base = u32(wotlk_data, WotLK.SEQUENCES_OFS)
    if n == 0 or base == 0:
        return []

    out: List[Dict[str, int]] = []
    for i in range(n):
        seq_off = base + i * SZ_M2_SEQUENCE
        anim_id = u16(wotlk_data, seq_off + 0x00)
        sub_id = u16(wotlk_data, seq_off + 0x02)
        flags = u32(wotlk_data, seq_off + 0x0C)
        out.append(
            {
                "index": i,
                "anim_id": anim_id,
                "sub_anim_id": sub_id,
                "flags": flags,
                "seq_offset": seq_off,
            }
        )
    return out


def discover_external_anims(
    sequences: List[Dict[str, int]],
    model_dir: str,
    model_name: str,
    verbose: bool = True,
) -> Tuple[Set[int], Dict[int, bytes]]:
    """For every sequence whose .anim file exists on disk, return its
    index + the file contents.  The set is the source of truth for "this
    sequence is external"; we deliberately do not rely on the ``0x20``
    flag because some private-server toolchains write it inconsistently.
    """
    external: Set[int] = set()
    blobs: Dict[int, bytes] = {}
    for seq in sequences:
        fname = f"{model_name}{seq['anim_id']:04d}-{seq['sub_anim_id']:02d}.anim"
        path = os.path.join(model_dir, fname)
        if os.path.isfile(path):
            with open(path, "rb") as fh:
                blobs[seq["index"]] = fh.read()
            external.add(seq["index"])
            if verbose:
                in_m2 = bool(seq["flags"] & SEQ_FLAG_DATA_IN_M2)
                print(
                    f"  found external anim: {fname} "
                    f"(seq#{seq['index']}, flag DATA_IN_M2={in_m2})"
                )
        else:
            if not (seq["flags"] & SEQ_FLAG_DATA_IN_M2) and verbose:
                print(
                    f"  WARN: seq#{seq['index']} ({seq['anim_id']}-{seq['sub_anim_id']})"
                    f" has DATA_IN_M2 cleared but {fname} is missing"
                )
    return external, blobs


# ---------------------------------------------------------------------------
# Main conversion routine
# ---------------------------------------------------------------------------


def _walk_top_level_arrays(walker: Walker, wotlk_data: bytes) -> None:
    """Walk every top-level M2Array in the WotLK header and recurse into the
    structures that contain inner M2Arrays / M2Tracks.
    """

    def walk_array(
        count_off: int,
        entry_size: int,
        per_entry: Optional[Callable[[Walker, int], None]],
    ) -> None:
        # Top-level header offsets are rewritten by build_tbc_header() — we
        # only need to descend into each entry to discover offsets stored
        # *inside* the data section.
        count, offset = read_array_pair(wotlk_data, count_off)
        if per_entry is None or count == 0 or offset == 0:
            return
        for i in range(count):
            per_entry(walker, offset + i * entry_size)

    # name + globalLoops + sequences (no inner offsets) + sequence lookup
    walk_array(WotLK.NAME_COUNT, 1, None)
    walk_array(WotLK.GLOBAL_LOOPS_COUNT, 4, None)
    walk_array(WotLK.SEQUENCES_COUNT, SZ_M2_SEQUENCE, None)
    walk_array(WotLK.SEQ_LOOKUP_COUNT, 2, None)

    # bones, key bone lookup, vertices, colors, textures
    walk_array(WotLK.BONES_COUNT, SZ_M2_COMPBONE, Walker.walk_compbone)
    walk_array(WotLK.KEYBONE_LOOKUP_COUNT, 2, None)
    walk_array(WotLK.VERTICES_COUNT, 48, None)  # M2Vertex = 48 bytes
    walk_array(WotLK.COLORS_COUNT, SZ_M2_COLOR, Walker.walk_color)
    walk_array(WotLK.TEXTURES_COUNT, SZ_M2_TEXTURE, Walker.walk_texture)

    walk_array(WotLK.TEX_WEIGHTS_COUNT, SZ_M2_TEXTURE_WEIGHT, Walker.walk_texture_weight)
    walk_array(
        WotLK.TEX_TRANSFORMS_COUNT,
        SZ_M2_TEXTURE_TRANSFORM,
        Walker.walk_texture_transform,
    )

    # plain lookup tables — no inner pointers
    walk_array(WotLK.TEX_REPLACE_COUNT, 2, None)
    walk_array(WotLK.MATERIALS_COUNT, 4, None)  # M2Material = 4 bytes
    walk_array(WotLK.BONE_LOOKUP_COUNT, 2, None)
    walk_array(WotLK.TEX_LOOKUP_COUNT, 2, None)
    walk_array(WotLK.TEX_UNIT_LOOKUP_COUNT, 2, None)
    walk_array(WotLK.TRANSPARENCY_LOOKUP_COUNT, 2, None)
    walk_array(WotLK.TEX_TRANSFORMS_LOOKUP_COUNT, 2, None)

    walk_array(WotLK.COLLISION_TRI_COUNT, 2, None)
    walk_array(WotLK.COLLISION_VERT_COUNT, 12, None)
    walk_array(WotLK.COLLISION_NORM_COUNT, 12, None)

    walk_array(WotLK.ATTACHMENTS_COUNT, SZ_M2_ATTACHMENT, Walker.walk_attachment)
    walk_array(WotLK.ATTACH_LOOKUP_COUNT, 2, None)
    walk_array(WotLK.EVENTS_COUNT, SZ_M2_EVENT, Walker.walk_event)
    walk_array(WotLK.LIGHTS_COUNT, SZ_M2_LIGHT, Walker.walk_light)
    walk_array(WotLK.CAMERAS_COUNT, SZ_M2_CAMERA, Walker.walk_camera)
    walk_array(WotLK.CAMERA_LOOKUP_COUNT, 2, None)

    # Ribbons / particles: just shift the outer pointer.  Inner tracks are
    # not patched by this walker — see the WARN print in convert().
    walk_array(WotLK.RIBBONS_COUNT, SZ_M2_RIBBON, None)
    walk_array(WotLK.PARTICLES_COUNT, SZ_M2_PARTICLE, None)


def convert(
    input_path: str,
    output_path: str,
    verbose: bool = True,
) -> None:
    with open(input_path, "rb") as f:
        wotlk_data = f.read()

    # ---- validation ------------------------------------------------------
    if len(wotlk_data) < WOTLK_HEADER_SIZE:
        raise SystemExit(
            f"input is too short ({len(wotlk_data)} < {WOTLK_HEADER_SIZE})"
        )
    if wotlk_data[0:4] != b"MD20":
        raise SystemExit("input is not an M2 file (missing MD20 magic)")
    version = u32(wotlk_data, WotLK.VERSION)
    if version != WOTLK_VERSION:
        raise SystemExit(
            f"input is not WotLK M2 v264 (got version {version}). "
            f"This converter only supports v264 -> v260."
        )

    model_dir = os.path.dirname(os.path.abspath(input_path)) or "."
    model_name = os.path.splitext(os.path.basename(input_path))[0]

    if verbose:
        print(f"input:    {input_path}")
        print(f"output:   {output_path}")
        print(f"model:    {model_name}")
        print(f"version:  {version} (will become {TBC_VERSION})")
        print(f"data section: {WOTLK_HEADER_SIZE} .. {len(wotlk_data)} bytes")

    # ---- discover sequences and their .anim files ------------------------
    sequences = parse_sequences(wotlk_data)
    n_views = u32(wotlk_data, WotLK.NUM_VIEWS)
    if verbose:
        print(f"sequences: {len(sequences)}; nViews: {n_views}")
        for cnt, ofs, label in (
            (u32(wotlk_data, WotLK.BONES_COUNT), WotLK.BONES_OFS, "bones"),
            (u32(wotlk_data, WotLK.RIBBONS_COUNT), WotLK.RIBBONS_OFS, "ribbon emitters"),
            (u32(wotlk_data, WotLK.PARTICLES_COUNT), WotLK.PARTICLES_OFS, "particle emitters"),
        ):
            print(f"  {label}: {cnt}")

    if u32(wotlk_data, WotLK.RIBBONS_COUNT) or u32(wotlk_data, WotLK.PARTICLES_COUNT):
        print(
            "  WARN: model has ribbon or particle emitters; their inner "
            "M2Tracks are NOT walked. Manual review of converted output "
            "is recommended.",
            file=sys.stderr,
        )

    external_seqs, anim_blobs = discover_external_anims(
        sequences, model_dir, model_name, verbose
    )

    # ---- read .skin files ------------------------------------------------
    skin_blobs: List[bytes] = []
    for i in range(n_views):
        skin_name = f"{model_name}{i:02d}.skin"
        skin_path = os.path.join(model_dir, skin_name)
        if not os.path.isfile(skin_path):
            raise SystemExit(f"required skin file missing: {skin_path}")
        with open(skin_path, "rb") as fh:
            skin_blobs.append(fh.read())
        if verbose:
            print(f"  found skin: {skin_name} ({len(skin_blobs[-1])} bytes)")

    # ---- walk WotLK structures and collect offset positions --------------
    walker = Walker(wotlk_data, external_seqs)
    _walk_top_level_arrays(walker, wotlk_data)

    # ---- build TBC header ------------------------------------------------
    tbc_header = build_tbc_header(wotlk_data)

    # ---- compose buffer: TBC header + (shifted) WotLK data ---------------
    out = bytearray()
    out.extend(tbc_header)
    out.extend(wotlk_data[WOTLK_HEADER_SIZE:])

    # Apply +HEADER_SHIFT to every collected offset position.  Walker
    # positions are WotLK file positions; in the new buffer they live at
    # `wotlk_pos + HEADER_SHIFT` (data section was relocated by +20).
    for wotlk_pos in walker.shift_positions:
        old_value = u32(wotlk_data, wotlk_pos)
        if old_value == 0:
            continue
        new_pos = wotlk_pos + HEADER_SHIFT
        write_u32(out, new_pos, old_value + HEADER_SHIFT)

    # ---- embed .skin files ----------------------------------------------
    skin_array_pos = embed_skin_files(out, skin_blobs, verbose=verbose)
    # Set TBC ofsViews
    if skin_array_pos:
        write_u32(out, wotlk_to_tbc_field(WotLK.NUM_VIEWS) + 4, skin_array_pos)

    # ---- embed .anim files and rewrite external M2Track inner offsets ----
    anim_positions: Dict[int, int] = {}
    for seq_idx, blob in anim_blobs.items():
        anim_positions[seq_idx] = len(out)
        out.extend(blob)
        if verbose:
            print(
                f"  embedded {model_name}{sequences[seq_idx]['anim_id']:04d}-"
                f"{sequences[seq_idx]['sub_anim_id']:02d}.anim at "
                f"{anim_positions[seq_idx]:#x} ({len(blob)} bytes)"
            )

    for inner_offset_pos, seq_idx in walker.external_inner:
        if seq_idx not in anim_positions:
            print(
                f"  WARN: seq#{seq_idx} referenced by an M2Track but no "
                f".anim file was found; leaving offset alone",
                file=sys.stderr,
            )
            continue
        new_pos_in_out = inner_offset_pos + HEADER_SHIFT
        original_in_anim = u32(wotlk_data, inner_offset_pos)
        write_u32(out, new_pos_in_out, anim_positions[seq_idx] + original_in_anim)

    # ---- mark sequences as internal --------------------------------------
    # After embedding, every sequence's data is in the .m2 file, so set the
    # 0x20 flag (data_in_m2 / primary bone sequence).
    seq_base = u32(out, wotlk_to_tbc_field(WotLK.SEQUENCES_OFS))
    n_sequences = u32(out, wotlk_to_tbc_field(WotLK.SEQUENCES_COUNT))
    flipped = 0
    for i in range(n_sequences):
        seq_off = seq_base + i * SZ_M2_SEQUENCE
        flags = u32(out, seq_off + 0x0C)
        new_flags = flags | SEQ_FLAG_DATA_IN_M2
        if new_flags != flags:
            write_u32(out, seq_off + 0x0C, new_flags)
            flipped += 1
    if verbose and flipped:
        print(f"  set DATA_IN_M2 (0x20) on {flipped} sequence(s)")

    # ---- final validation ------------------------------------------------
    _final_validate(out, n_views, skin_array_pos)

    with open(output_path, "wb") as f:
        f.write(out)

    if verbose:
        print(f"wrote {len(out):,} bytes to {output_path}")


# ---------------------------------------------------------------------------
# Validation
# ---------------------------------------------------------------------------


def _final_validate(out: bytes, n_views: int, skin_array_pos: int) -> int:
    """Run cheap sanity checks on the output buffer."""
    issues = 0
    if out[:4] != b"MD20":
        print("VALIDATE: missing MD20 magic", file=sys.stderr)
        issues += 1
    version = u32(out, WotLK.VERSION)
    if version != TBC_VERSION:
        print(f"VALIDATE: version is {version}, expected {TBC_VERSION}", file=sys.stderr)
        issues += 1
    if n_views > 0:
        ofs_views = u32(out, wotlk_to_tbc_field(WotLK.NUM_VIEWS) + 4)
        if ofs_views == 0 or ofs_views >= len(out):
            print(
                f"VALIDATE: ofsViews={ofs_views:#x} (file size={len(out):#x})",
                file=sys.stderr,
            )
            issues += 1
        if ofs_views != skin_array_pos:
            print(
                f"VALIDATE: ofsViews ({ofs_views:#x}) != skin_array_pos "
                f"({skin_array_pos:#x})",
                file=sys.stderr,
            )
            issues += 1
    if issues == 0:
        print("validation: ok")
    else:
        print(f"validation: {issues} issue(s)", file=sys.stderr)
    return issues


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def _parse_args(argv: Optional[List[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Convert a WotLK M2 model file (v264) into a TBC-compatible "
            "M2 file (v260) by embedding external .skin and .anim files "
            "and rebuilding the header into the TBC layout."
        ),
    )
    parser.add_argument("input", help="input .m2 file (WotLK / version 264)")
    parser.add_argument(
        "output",
        help="output .m2 file (TBC / version 260)",
    )
    parser.add_argument(
        "-q",
        "--quiet",
        action="store_true",
        help="suppress informational diagnostics",
    )
    return parser.parse_args(argv)


def main(argv: Optional[List[str]] = None) -> int:
    args = _parse_args(argv)
    convert(args.input, args.output, verbose=not args.quiet)
    return 0


if __name__ == "__main__":
    sys.exit(main())
