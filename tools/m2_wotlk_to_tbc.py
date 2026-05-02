#!/usr/bin/env python3
"""WotLK (M2 v264) -> TBC (M2 v260) structural converter.

This is **v2** of the converter. Where v1 only patched the file header and
embedded external `.skin` / `.anim` data without changing data layout, v2
performs a full structural rewrite:

    *  expands every animated container to its TBC layout
       (M2Track 20 -> 28 bytes; M2CompBone 88 -> 110; M2Color 40 -> 56;
        M2TextureTransform 60 -> 84; M2Light 156 -> 212; M2Camera 100 -> 124;
        M2Attachment 40 -> 48; M2Event 36 -> 44),
    *  converts every per-sequence M2Array<M2Array<T>> track into the TBC
       form: a single flat M2Array<T> with an accompanying interpolation
       ranges table that stores the (min,max) keyframe-index window for
       each animation,
    *  computes a global timeline (every WotLK `duration` becomes a
       (start_timestamp, end_timestamp) pair on a cumulative timeline) and
       shifts every per-sequence keyframe timestamp into that timeline,
    *  rewrites M2Sequence (64 -> 68 bytes) using start/end_timestamp,
    *  embeds the highest-LOD `.skin` payload as the inline TBC
       `M2SkinProfile` array reachable via `ofsViews`, restructuring
       `M2SkinSubmesh` (32 -> 48 bytes), and
    *  consolidates every external `.anim` keyframe stream into the new
       file so the result is a self-contained TBC `.m2`.

Authoritative size references: pywowlib (https://github.com/wowdev/pywowlib)
file_formats/m2_format.py and skin_format.py.
"""
from __future__ import annotations

import os
import struct
import sys
from dataclasses import dataclass, field


# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

WOTLK_VERSION = 264
# TBC retail used version 263 (patch 2.4.3). Earlier patches used 260,
# but every TBC v2.4 client we have a reference for embeds 263 in the
# header, including the user's private server build. Some clients
# explicitly compare the version number when deciding whether to load
# the inline skin profile, so we always emit 263.
TBC_VERSION = 263

# Header
WOTLK_HEADER_SIZE = 304
TBC_HEADER_SIZE = 324

# Animated container sizes (bytes)
WOTLK_BONE_SIZE = 88
TBC_BONE_SIZE = 112

WOTLK_COLOR_SIZE = 40
TBC_COLOR_SIZE = 56

WOTLK_TEXTURE_TRANSFORM_SIZE = 60
TBC_TEXTURE_TRANSFORM_SIZE = 84

WOTLK_LIGHT_SIZE = 156
TBC_LIGHT_SIZE = 212

WOTLK_CAMERA_SIZE = 100
TBC_CAMERA_SIZE = 124

WOTLK_ATTACHMENT_SIZE = 40
TBC_ATTACHMENT_SIZE = 48

WOTLK_EVENT_SIZE = 36
TBC_EVENT_SIZE = 44

# Sequence (M2Sequence)
WOTLK_SEQ_SIZE = 64
TBC_SEQ_SIZE = 68

# M2Track
WOTLK_TRACK_SIZE = 20  # M2Track w/ values
TBC_TRACK_SIZE = 28
WOTLK_TRACKBASE_SIZE = 12  # M2Track w/o values (M2Event)
TBC_TRACKBASE_SIZE = 20

# Sequence flags
SEQ_FLAG_PRIMARY = 0x20  # set => keyframe data is in the .m2 (internal)
SEQ_FLAG_ALIAS = 0x40    # this entry is an alias of `alias_next`

# Skin file
SKIN_MAGIC = b"SKIN"
SKIN_PROFILE_SIZE_TBC = 44       # 5 M2Arrays + bone_count_max
WOTLK_SUBMESH_SIZE = 32
TBC_SUBMESH_SIZE = 48
SKIN_TEXTURE_UNIT_SIZE = 24
SKIN_BONE_INFLUENCE_SIZE = 4

# Gap inserted between consecutive sequences on the global TBC timeline.
# Animations that share boundary timestamps would otherwise be ambiguous.
GLOBAL_TIMELINE_GAP_MS = 1


# ---------------------------------------------------------------------------
# Bytes helpers
# ---------------------------------------------------------------------------

def u16(buf: bytes, off: int) -> int:
    return struct.unpack_from("<H", buf, off)[0]


def i16(buf: bytes, off: int) -> int:
    return struct.unpack_from("<h", buf, off)[0]


def u32(buf: bytes, off: int) -> int:
    return struct.unpack_from("<I", buf, off)[0]


def i32(buf: bytes, off: int) -> int:
    return struct.unpack_from("<i", buf, off)[0]


def f32(buf: bytes, off: int) -> float:
    return struct.unpack_from("<f", buf, off)[0]


def m2arr(buf: bytes, off: int) -> tuple[int, int]:
    """Read M2Array (count, offset) at `off`."""
    return struct.unpack_from("<II", buf, off)


def pack_m2arr(count: int, offset: int) -> bytes:
    return struct.pack("<II", count, offset)


def align(n: int, alignment: int = 4) -> int:
    """Round `n` up to a multiple of `alignment`."""
    return (n + alignment - 1) & ~(alignment - 1)


# ---------------------------------------------------------------------------
# Track value sizes
# ---------------------------------------------------------------------------

# Each named track type knows its value-size in bytes (or None for events).
# This is what's stored in the M2Array<T> inside the M2Track.
TRACK_KIND_VALUE_SIZE: dict[str, int | None] = {
    # Bones
    "bone_translation": 12,    # vec3D
    "bone_rotation": 8,        # M2CompQuaternion (4 * int16)
    "bone_scale": 12,          # vec3D
    # Colors / transparency
    "color_rgb": 12,           # vec3D
    "color_alpha": 2,          # fixed16
    "transparency": 2,         # fixed16
    # Texture transforms
    "texture_translation": 12,
    "texture_rotation": 16,    # quat (4 * float32) -- *not* compressed
    "texture_scaling": 12,
    # Lights
    "light_ambient_color": 12,
    "light_ambient_intensity": 4,
    "light_diffuse_color": 12,
    "light_diffuse_intensity": 4,
    "light_attenuation_start": 4,
    "light_attenuation_end": 4,
    "light_visibility": 1,
    # Cameras
    "camera_positions": 36,        # M2SplineKey<vec3D> = 3 * 12
    "camera_target_position": 36,
    "camera_roll": 12,             # M2SplineKey<float32> = 3 * 4
    # Attachments
    "attachment_animate": 1,       # boolean
    # Events have no values
    "event_enabled": None,
}


# ---------------------------------------------------------------------------
# Parsed WotLK structures
# ---------------------------------------------------------------------------


@dataclass
class WotLKSequence:
    """A WotLK M2Sequence as read from disk plus computed timeline info."""

    index: int
    id: int
    sub_id: int
    duration: int
    movespeed: float
    flags: int
    frequency: int
    padding: int
    replay_min: int
    replay_max: int
    blend_time: int
    bbox_min: tuple[float, float, float]
    bbox_max: tuple[float, float, float]
    bound_radius: float
    variation_next: int
    alias_next: int
    # Computed:
    is_external: bool = False
    start_timestamp: int = 0
    end_timestamp: int = 0
    anim_data: bytes = b""

    @classmethod
    def parse(cls, buf: bytes, off: int, index: int) -> "WotLKSequence":
        return cls(
            index=index,
            id=u16(buf, off + 0x00),
            sub_id=u16(buf, off + 0x02),
            duration=u32(buf, off + 0x04),
            movespeed=f32(buf, off + 0x08),
            flags=u32(buf, off + 0x0C),
            frequency=i16(buf, off + 0x10),
            padding=u16(buf, off + 0x12),
            replay_min=u32(buf, off + 0x14),
            replay_max=u32(buf, off + 0x18),
            blend_time=u32(buf, off + 0x1C),
            bbox_min=(f32(buf, off + 0x20), f32(buf, off + 0x24), f32(buf, off + 0x28)),
            bbox_max=(f32(buf, off + 0x2C), f32(buf, off + 0x30), f32(buf, off + 0x34)),
            bound_radius=f32(buf, off + 0x38),
            variation_next=i16(buf, off + 0x3C),
            alias_next=u16(buf, off + 0x3E),
        )

    def serialize_tbc(self) -> bytes:
        out = bytearray(TBC_SEQ_SIZE)
        struct.pack_into("<HH", out, 0x00, self.id, self.sub_id)
        struct.pack_into("<II", out, 0x04, self.start_timestamp, self.end_timestamp)
        struct.pack_into("<f", out, 0x0C, self.movespeed)
        # Real TBC v260/v263 models (Eredar, Ogre, etc.) only ever set the
        # low flag bits 0x01..0x08 plus the alias bit 0x40. The WotLK
        # "primary bone sequence / data is in .m2" bit (0x20) and the
        # WotLK-only bookkeeping bit 0x10 are NOT used in TBC -- in TBC
        # all keyframe data is always inline in the .m2, so 0x20 has no
        # meaning and setting it confuses the client. We strip every bit
        # except the small set that genuinely exists in TBC files.
        TBC_VALID_FLAGS = 0x004F  # 0x01 | 0x02 | 0x04 | 0x08 | 0x40
        cleaned = self.flags & TBC_VALID_FLAGS
        struct.pack_into("<I", out, 0x10, cleaned & 0xFFFFFFFF)
        struct.pack_into("<hH", out, 0x14, self.frequency, self.padding)
        struct.pack_into("<II", out, 0x18, self.replay_min, self.replay_max)
        struct.pack_into("<I", out, 0x20, self.blend_time)
        struct.pack_into("<fff", out, 0x24, *self.bbox_min)
        struct.pack_into("<fff", out, 0x30, *self.bbox_max)
        struct.pack_into("<f", out, 0x3C, self.bound_radius)
        struct.pack_into("<hH", out, 0x40, self.variation_next, self.alias_next)
        return bytes(out)


@dataclass
class WotLKTrack:
    """A parsed WotLK M2Track or M2TrackBase (event)."""

    interp_type: int
    global_sequence: int
    # Per-sequence raw bytes (before TBC restructuring).
    # `ts_per_seq[i]` is the WotLK timestamp bytes for sequence `i`.
    # `vs_per_seq[i]` is the WotLK value bytes for sequence `i` (or None when
    # this is an M2Event track that stores no values).
    ts_per_seq: list[bytes] = field(default_factory=list)
    vs_per_seq: list[bytes | None] = field(default_factory=list)


@dataclass
class WotLKBone:
    key_bone_id: int
    flags: int
    parent_bone: int
    submesh_id: int
    bone_name_crc: int
    translation: WotLKTrack
    rotation: WotLKTrack
    scale: WotLKTrack
    pivot: tuple[float, float, float]


@dataclass
class WotLKColor:
    color: WotLKTrack
    alpha: WotLKTrack


@dataclass
class WotLKTextureTransform:
    translation: WotLKTrack
    rotation: WotLKTrack
    scaling: WotLKTrack


@dataclass
class WotLKLight:
    type_: int
    bone: int
    position: tuple[float, float, float]
    ambient_color: WotLKTrack
    ambient_intensity: WotLKTrack
    diffuse_color: WotLKTrack
    diffuse_intensity: WotLKTrack
    attenuation_start: WotLKTrack
    attenuation_end: WotLKTrack
    visibility: WotLKTrack


@dataclass
class WotLKCamera:
    type_: int
    fov: float
    far_clip: float
    near_clip: float
    positions: WotLKTrack
    position_base: tuple[float, float, float]
    target_position: WotLKTrack
    target_position_base: tuple[float, float, float]
    roll: WotLKTrack


@dataclass
class WotLKAttachment:
    id: int
    bone: int
    unknown: int
    position: tuple[float, float, float]
    animate_attached: WotLKTrack


@dataclass
class WotLKEvent:
    identifier: bytes  # 4 ASCII bytes
    data: int
    bone: int
    position: tuple[float, float, float]
    enabled: WotLKTrack  # M2Event uses M2TrackBase (no values)


# ---------------------------------------------------------------------------
# WotLK parser
# ---------------------------------------------------------------------------


class WotLKReader:
    """Parse a WotLK .m2 file (and adjacent .skin / .anim files) into objects."""

    def __init__(self, m2_path: str):
        self.m2_path = m2_path
        self.dir = os.path.dirname(os.path.abspath(m2_path))
        self.stem = os.path.splitext(os.path.basename(m2_path))[0]
        self.m2 = open(m2_path, "rb").read()
        if self.m2[:4] != b"MD20":
            raise ValueError(f"{m2_path}: not an M2 file (magic != MD20)")
        version = u32(self.m2, 4)
        if version != WOTLK_VERSION:
            raise ValueError(
                f"{m2_path}: expected WotLK version {WOTLK_VERSION}, got {version}"
            )
        # Header
        self.global_flags = u32(self.m2, 0x10)
        self.n_global_seq, self.ofs_global_seq = m2arr(self.m2, 0x14)
        self.n_anims, self.ofs_anims = m2arr(self.m2, 0x1C)
        self.n_anim_lookup, self.ofs_anim_lookup = m2arr(self.m2, 0x24)
        self.n_bones, self.ofs_bones = m2arr(self.m2, 0x2C)
        self.n_kbone_lookup, self.ofs_kbone_lookup = m2arr(self.m2, 0x34)
        self.n_vertices, self.ofs_vertices = m2arr(self.m2, 0x3C)
        self.n_views = u32(self.m2, 0x44)
        self.n_colors, self.ofs_colors = m2arr(self.m2, 0x48)
        self.n_textures, self.ofs_textures = m2arr(self.m2, 0x50)
        self.n_transparency, self.ofs_transparency = m2arr(self.m2, 0x58)
        self.n_tex_anims, self.ofs_tex_anims = m2arr(self.m2, 0x60)
        self.n_tex_replace, self.ofs_tex_replace = m2arr(self.m2, 0x68)
        self.n_materials, self.ofs_materials = m2arr(self.m2, 0x70)
        self.n_bone_lookup, self.ofs_bone_lookup = m2arr(self.m2, 0x78)
        self.n_tex_lookup, self.ofs_tex_lookup = m2arr(self.m2, 0x80)
        self.n_tex_unit_lookup, self.ofs_tex_unit_lookup = m2arr(self.m2, 0x88)
        self.n_trans_lookup, self.ofs_trans_lookup = m2arr(self.m2, 0x90)
        self.n_tex_anim_lookup, self.ofs_tex_anim_lookup = m2arr(self.m2, 0x98)
        # bbox + collision + ...
        self.bounding_box = self.m2[0xA0:0xB8]
        self.bounding_radius = f32(self.m2, 0xB8)
        self.collision_box = self.m2[0xBC:0xD4]
        self.collision_radius = f32(self.m2, 0xD4)
        self.n_coll_idx, self.ofs_coll_idx = m2arr(self.m2, 0xD8)
        self.n_coll_vert, self.ofs_coll_vert = m2arr(self.m2, 0xE0)
        self.n_coll_norm, self.ofs_coll_norm = m2arr(self.m2, 0xE8)
        self.n_attach, self.ofs_attach = m2arr(self.m2, 0xF0)
        self.n_attach_lookup, self.ofs_attach_lookup = m2arr(self.m2, 0xF8)
        self.n_events, self.ofs_events = m2arr(self.m2, 0x100)
        self.n_lights, self.ofs_lights = m2arr(self.m2, 0x108)
        self.n_cameras, self.ofs_cameras = m2arr(self.m2, 0x110)
        self.n_camera_lookup, self.ofs_camera_lookup = m2arr(self.m2, 0x118)
        self.n_ribbon, self.ofs_ribbon = m2arr(self.m2, 0x120)
        self.n_particle, self.ofs_particle = m2arr(self.m2, 0x128)

        # Name (small string, copy verbatim)
        n_name = u32(self.m2, 0x08)
        ofs_name = u32(self.m2, 0x0C)
        self.name_bytes = self.m2[ofs_name:ofs_name + n_name] if n_name else b""

        # Lookups - copy as bytes (raw uint16 / int16 arrays).
        self.global_sequences = self.m2[self.ofs_global_seq:self.ofs_global_seq + self.n_global_seq * 4]
        self.anim_lookup = self.m2[self.ofs_anim_lookup:self.ofs_anim_lookup + self.n_anim_lookup * 2]
        self.kbone_lookup = self.m2[self.ofs_kbone_lookup:self.ofs_kbone_lookup + self.n_kbone_lookup * 2]
        self.vertex_data = self.m2[self.ofs_vertices:self.ofs_vertices + self.n_vertices * 36]
        self.bone_lookup = self.m2[self.ofs_bone_lookup:self.ofs_bone_lookup + self.n_bone_lookup * 2]
        self.tex_lookup = self.m2[self.ofs_tex_lookup:self.ofs_tex_lookup + self.n_tex_lookup * 2]
        self.tex_unit_lookup = self.m2[self.ofs_tex_unit_lookup:self.ofs_tex_unit_lookup + self.n_tex_unit_lookup * 2]
        self.trans_lookup = self.m2[self.ofs_trans_lookup:self.ofs_trans_lookup + self.n_trans_lookup * 2]
        self.tex_anim_lookup = self.m2[self.ofs_tex_anim_lookup:self.ofs_tex_anim_lookup + self.n_tex_anim_lookup * 2]
        self.attach_lookup = self.m2[self.ofs_attach_lookup:self.ofs_attach_lookup + self.n_attach_lookup * 2]
        self.camera_lookup = self.m2[self.ofs_camera_lookup:self.ofs_camera_lookup + self.n_camera_lookup * 2]
        self.coll_idx = self.m2[self.ofs_coll_idx:self.ofs_coll_idx + self.n_coll_idx * 2]
        self.coll_vert = self.m2[self.ofs_coll_vert:self.ofs_coll_vert + self.n_coll_vert * 12]
        self.coll_norm = self.m2[self.ofs_coll_norm:self.ofs_coll_norm + self.n_coll_norm * 12]
        # Materials (4 bytes each)
        self.materials = self.m2[self.ofs_materials:self.ofs_materials + self.n_materials * 4]
        # Replacable texture lookup (uint16 array)
        self.replacable_texture_lookup = self.m2[self.ofs_tex_replace:self.ofs_tex_replace + self.n_tex_replace * 2]

        # Texture array (16 bytes each, references external strings).
        self.textures: list[tuple[int, int, bytes]] = []
        for i in range(self.n_textures):
            base = self.ofs_textures + i * 16
            ttype = u32(self.m2, base + 0x00)
            tflags = u32(self.m2, base + 0x04)
            n_tname, o_tname = m2arr(self.m2, base + 0x08)
            tname = self.m2[o_tname:o_tname + n_tname]
            self.textures.append((ttype, tflags, tname))

        # Sequences
        self.sequences: list[WotLKSequence] = [
            WotLKSequence.parse(self.m2, self.ofs_anims + i * WOTLK_SEQ_SIZE, i)
            for i in range(self.n_anims)
        ]

        # Discover external .anim files on disk.
        # Filename convention: <stem><id:04>-<sub_id:02>.anim
        self.anim_blobs: dict[tuple[int, int], bytes] = {}
        for s in self.sequences:
            if s.flags & SEQ_FLAG_PRIMARY:
                continue
            if s.flags & SEQ_FLAG_ALIAS:
                # Aliases redirect to alias_next; their data lives in
                # the target sequence (which is itself either internal or
                # external -- we'll resolve later).
                continue
            anim_name = f"{self.stem}{s.id:04d}-{s.sub_id:02d}.anim"
            anim_path = os.path.join(self.dir, anim_name)
            if os.path.exists(anim_path):
                s.is_external = True
                s.anim_data = open(anim_path, "rb").read()
                self.anim_blobs[(s.id, s.sub_id)] = s.anim_data
            else:
                # No file found: treat as internal even though the flag says external.
                # The track outer-array entries will read from m2 bytes (likely zero).
                s.is_external = False

        # Animated structures
        self.bones: list[WotLKBone] = self._parse_bones()
        self.colors: list[WotLKColor] = self._parse_colors()
        self.transparencies: list[WotLKTrack] = self._parse_transparencies()
        self.tex_transforms: list[WotLKTextureTransform] = self._parse_tex_transforms()
        self.lights: list[WotLKLight] = self._parse_lights()
        self.cameras: list[WotLKCamera] = self._parse_cameras()
        self.attachments: list[WotLKAttachment] = self._parse_attachments()
        self.events: list[WotLKEvent] = self._parse_events()

        # Skins
        self.skins: list[bytes] = self._discover_skins()

    # -------------- track parsers --------------

    def _parse_track(self, off: int, has_values: bool, value_size: int | None) -> WotLKTrack:
        """Parse a WotLK M2Track (or M2TrackBase) stored inside the .m2."""
        interp = u16(self.m2, off + 0x00)
        gseq = i16(self.m2, off + 0x02)
        n_outer_ts, o_outer_ts = m2arr(self.m2, off + 0x04)
        if has_values:
            n_outer_vs, o_outer_vs = m2arr(self.m2, off + 0x0C)
        track = WotLKTrack(interp_type=interp, global_sequence=gseq)

        # When this track points at a global sequence the outer array still
        # holds 1 inner array (the global-loop data). We treat that as
        # "single sequence" by storing it under index 0 and emitting one
        # interpolation_range entry on the TBC side.
        is_global = (gseq >= 0)

        # Outer count is normally either 0, 1 (global), or n_anims.
        outer_n = n_outer_ts
        if has_values:
            # Use the larger of the two so we don't lose data on broken files.
            outer_n = max(outer_n, n_outer_vs)

        for s_idx in range(len(self.sequences)):
            ts_bytes = b""
            vs_bytes: bytes | None = b"" if has_values else None
            if is_global:
                # Only first iteration has data; the rest are empty.
                if s_idx == 0 and n_outer_ts > 0:
                    inner_n, inner_o = m2arr(self.m2, o_outer_ts)
                    ts_bytes = self.m2[inner_o:inner_o + inner_n * 4]
                if has_values and s_idx == 0 and n_outer_vs > 0:
                    inner_n, inner_o = m2arr(self.m2, o_outer_vs)
                    if value_size is None:
                        vs_bytes = b""
                    else:
                        vs_bytes = self.m2[inner_o:inner_o + inner_n * value_size]
            else:
                if s_idx < n_outer_ts:
                    inner_n, inner_o = m2arr(self.m2, o_outer_ts + s_idx * 8)
                    seq = self.sequences[s_idx]
                    src = seq.anim_data if seq.is_external else self.m2
                    ts_bytes = src[inner_o:inner_o + inner_n * 4] if inner_n else b""
                if has_values and s_idx < n_outer_vs:
                    inner_n, inner_o = m2arr(self.m2, o_outer_vs + s_idx * 8)
                    if value_size is None:
                        vs_bytes = b""
                    else:
                        seq = self.sequences[s_idx]
                        src = seq.anim_data if seq.is_external else self.m2
                        vs_bytes = src[inner_o:inner_o + inner_n * value_size] if inner_n else b""
            track.ts_per_seq.append(ts_bytes)
            track.vs_per_seq.append(vs_bytes)
        return track

    # -------------- struct parsers --------------

    def _parse_bones(self) -> list[WotLKBone]:
        bones: list[WotLKBone] = []
        for i in range(self.n_bones):
            off = self.ofs_bones + i * WOTLK_BONE_SIZE
            translation = self._parse_track(off + 0x10, True, TRACK_KIND_VALUE_SIZE["bone_translation"])
            rotation = self._parse_track(off + 0x24, True, TRACK_KIND_VALUE_SIZE["bone_rotation"])
            scale = self._parse_track(off + 0x38, True, TRACK_KIND_VALUE_SIZE["bone_scale"])
            bones.append(WotLKBone(
                key_bone_id=i32(self.m2, off + 0x00),
                flags=u32(self.m2, off + 0x04),
                parent_bone=i16(self.m2, off + 0x08),
                submesh_id=u16(self.m2, off + 0x0A),
                bone_name_crc=u32(self.m2, off + 0x0C),
                translation=translation,
                rotation=rotation,
                scale=scale,
                pivot=(f32(self.m2, off + 0x4C), f32(self.m2, off + 0x50), f32(self.m2, off + 0x54)),
            ))
        return bones

    def _parse_colors(self) -> list[WotLKColor]:
        out: list[WotLKColor] = []
        for i in range(self.n_colors):
            off = self.ofs_colors + i * WOTLK_COLOR_SIZE
            col = self._parse_track(off + 0x00, True, TRACK_KIND_VALUE_SIZE["color_rgb"])
            alpha = self._parse_track(off + 0x14, True, TRACK_KIND_VALUE_SIZE["color_alpha"])
            out.append(WotLKColor(color=col, alpha=alpha))
        return out

    def _parse_transparencies(self) -> list[WotLKTrack]:
        out: list[WotLKTrack] = []
        for i in range(self.n_transparency):
            off = self.ofs_transparency + i * WOTLK_TRACK_SIZE
            out.append(self._parse_track(off, True, TRACK_KIND_VALUE_SIZE["transparency"]))
        return out

    def _parse_tex_transforms(self) -> list[WotLKTextureTransform]:
        out: list[WotLKTextureTransform] = []
        for i in range(self.n_tex_anims):
            off = self.ofs_tex_anims + i * WOTLK_TEXTURE_TRANSFORM_SIZE
            t = self._parse_track(off + 0x00, True, TRACK_KIND_VALUE_SIZE["texture_translation"])
            r = self._parse_track(off + 0x14, True, TRACK_KIND_VALUE_SIZE["texture_rotation"])
            s = self._parse_track(off + 0x28, True, TRACK_KIND_VALUE_SIZE["texture_scaling"])
            out.append(WotLKTextureTransform(translation=t, rotation=r, scaling=s))
        return out

    def _parse_lights(self) -> list[WotLKLight]:
        out: list[WotLKLight] = []
        for i in range(self.n_lights):
            off = self.ofs_lights + i * WOTLK_LIGHT_SIZE
            type_ = u16(self.m2, off + 0x00)
            bone = i16(self.m2, off + 0x02)
            position = (f32(self.m2, off + 0x04), f32(self.m2, off + 0x08), f32(self.m2, off + 0x0C))
            ambient_color = self._parse_track(off + 0x10, True, TRACK_KIND_VALUE_SIZE["light_ambient_color"])
            ambient_intensity = self._parse_track(off + 0x24, True, TRACK_KIND_VALUE_SIZE["light_ambient_intensity"])
            diffuse_color = self._parse_track(off + 0x38, True, TRACK_KIND_VALUE_SIZE["light_diffuse_color"])
            diffuse_intensity = self._parse_track(off + 0x4C, True, TRACK_KIND_VALUE_SIZE["light_diffuse_intensity"])
            attenuation_start = self._parse_track(off + 0x60, True, TRACK_KIND_VALUE_SIZE["light_attenuation_start"])
            attenuation_end = self._parse_track(off + 0x74, True, TRACK_KIND_VALUE_SIZE["light_attenuation_end"])
            visibility = self._parse_track(off + 0x88, True, TRACK_KIND_VALUE_SIZE["light_visibility"])
            out.append(WotLKLight(
                type_=type_, bone=bone, position=position,
                ambient_color=ambient_color, ambient_intensity=ambient_intensity,
                diffuse_color=diffuse_color, diffuse_intensity=diffuse_intensity,
                attenuation_start=attenuation_start, attenuation_end=attenuation_end,
                visibility=visibility,
            ))
        return out

    def _parse_cameras(self) -> list[WotLKCamera]:
        out: list[WotLKCamera] = []
        for i in range(self.n_cameras):
            off = self.ofs_cameras + i * WOTLK_CAMERA_SIZE
            type_ = i32(self.m2, off + 0x00)
            fov = f32(self.m2, off + 0x04)
            far_clip = f32(self.m2, off + 0x08)
            near_clip = f32(self.m2, off + 0x0C)
            positions = self._parse_track(off + 0x10, True, TRACK_KIND_VALUE_SIZE["camera_positions"])
            position_base = (f32(self.m2, off + 0x24), f32(self.m2, off + 0x28), f32(self.m2, off + 0x2C))
            target_position = self._parse_track(off + 0x30, True, TRACK_KIND_VALUE_SIZE["camera_target_position"])
            target_position_base = (f32(self.m2, off + 0x44), f32(self.m2, off + 0x48), f32(self.m2, off + 0x4C))
            roll = self._parse_track(off + 0x50, True, TRACK_KIND_VALUE_SIZE["camera_roll"])
            out.append(WotLKCamera(
                type_=type_, fov=fov, far_clip=far_clip, near_clip=near_clip,
                positions=positions, position_base=position_base,
                target_position=target_position, target_position_base=target_position_base,
                roll=roll,
            ))
        return out

    def _parse_attachments(self) -> list[WotLKAttachment]:
        out: list[WotLKAttachment] = []
        for i in range(self.n_attach):
            off = self.ofs_attach + i * WOTLK_ATTACHMENT_SIZE
            id_ = u32(self.m2, off + 0x00)
            bone = u16(self.m2, off + 0x04)
            unknown = u16(self.m2, off + 0x06)
            position = (f32(self.m2, off + 0x08), f32(self.m2, off + 0x0C), f32(self.m2, off + 0x10))
            animate = self._parse_track(off + 0x14, True, TRACK_KIND_VALUE_SIZE["attachment_animate"])
            out.append(WotLKAttachment(id=id_, bone=bone, unknown=unknown, position=position, animate_attached=animate))
        return out

    def _parse_events(self) -> list[WotLKEvent]:
        out: list[WotLKEvent] = []
        for i in range(self.n_events):
            off = self.ofs_events + i * WOTLK_EVENT_SIZE
            ident = self.m2[off + 0x00:off + 0x04]
            data = u32(self.m2, off + 0x04)
            bone = u32(self.m2, off + 0x08)
            position = (f32(self.m2, off + 0x0C), f32(self.m2, off + 0x10), f32(self.m2, off + 0x14))
            # M2Event uses M2TrackBase (no values), so value_size=None / has_values=False.
            enabled = self._parse_track(off + 0x18, False, None)
            out.append(WotLKEvent(identifier=ident, data=data, bone=bone, position=position, enabled=enabled))
        return out

    # -------------- skin discovery --------------

    def _discover_skins(self) -> list[bytes]:
        """Return raw bytes of every `<stem>NN.skin` LOD that exists on disk.

        TBC always embeds 4 views (LOD0..LOD3), so we try each numbered
        skin from 00 up to 03. Some WotLK exports only ship a single LOD
        even when `nViews > 1`; in that case we replicate LOD0 later when
        embedding. The 00.skin file is mandatory -- a TBC client cannot
        find the data otherwise.
        """
        skins: list[bytes] = []
        for i in range(4):
            skin_name = f"{self.stem}{i:02d}.skin"
            skin_path = os.path.join(self.dir, skin_name)
            if not os.path.exists(skin_path):
                if i == 0:
                    raise FileNotFoundError(f"required skin file not found: {skin_path}")
                break
            skins.append(open(skin_path, "rb").read())
        return skins


# ---------------------------------------------------------------------------
# Skin restructuring (WotLK -> TBC)
# ---------------------------------------------------------------------------


@dataclass
class TBCSkinProfile:
    """A WotLK .skin file rewritten to TBC inline form.

    All offsets in `vertex_indices`, `triangle_indices`, `bone_indices`,
    `submeshes`, `texture_units` are relative to the start of the .m2 once
    placed (filled in by the writer).
    """

    vertex_indices_count: int
    vertex_indices_data: bytes  # uint16 array
    triangle_indices_count: int
    triangle_indices_data: bytes
    bone_indices_count: int
    bone_indices_data: bytes
    submeshes_count: int
    submeshes_data: bytes  # already restructured to TBC layout
    texture_units_count: int
    texture_units_data: bytes
    bone_count_max: int


def restructure_skin_for_tbc(skin: bytes) -> TBCSkinProfile:
    """Parse a WotLK .skin and return its TBC equivalent.

    Both WotLK and TBC skin files use the **same** 48-byte
    ``M2SkinSubmesh`` layout (uint16 x10, center_position vec3D,
    sort_center_position vec3D, sort_radius float32) -- the
    ``sort_center_position`` / ``sort_radius`` fields were added all the
    way back in TBC (= BC), not in WotLK. So the only restructuring we do
    here is recompute ``bone_count_max`` if the source forgot to set it
    (some WotLK files leave it at zero, which makes the TBC client refuse
    to allocate any skinning palette).
    """
    if skin[:4] == SKIN_MAGIC:
        prof_off = 4
    else:
        prof_off = 0

    n_verts, ofs_verts = m2arr(skin, prof_off + 0x00)
    n_tris, ofs_tris = m2arr(skin, prof_off + 0x08)
    n_bones, ofs_bones = m2arr(skin, prof_off + 0x10)
    n_subs, ofs_subs = m2arr(skin, prof_off + 0x18)
    n_units, ofs_units = m2arr(skin, prof_off + 0x20)
    bone_count_max = u32(skin, prof_off + 0x28)

    vertex_indices_data = skin[ofs_verts:ofs_verts + n_verts * 2]
    triangle_indices_data = skin[ofs_tris:ofs_tris + n_tris * 2]
    bone_indices_data = skin[ofs_bones:ofs_bones + n_bones * SKIN_BONE_INFLUENCE_SIZE]
    texture_units_raw = skin[ofs_units:ofs_units + n_units * SKIN_TEXTURE_UNIT_SIZE]

    # Texture units: every reference TBC v263 model we have (Eredar, Ogre,
    # Chimera, CryptLord, Horisath) consistently uses ``Flags |= 0x10`` on
    # *every* batch. This bit is set by the retail TBC content tools as a
    # "this batch is part of the static rendering pass" marker; some 2.4.3
    # client builds skip rendering when it isn't set, which causes the
    # body submesh to silently disappear while the rig itself still
    # animates. The 0x10 bit doesn't change WotLK behaviour either way
    # (both eras OR it in implicitly), so we always set it here.
    texture_units_buf = bytearray(texture_units_raw)
    for i in range(n_units):
        flags_off = i * SKIN_TEXTURE_UNIT_SIZE  # uint16 at offset 0
        flags = struct.unpack_from("<H", texture_units_buf, flags_off)[0]
        flags |= 0x10
        struct.pack_into("<H", texture_units_buf, flags_off, flags)
    texture_units_data = bytes(texture_units_buf)

    # Submeshes are 48 bytes in *both* WotLK and TBC: copy verbatim.
    submeshes_data = skin[ofs_subs:ofs_subs + n_subs * TBC_SUBMESH_SIZE]

    # Recompute bone_count_max from the actual submesh data when the
    # source skin file left it at 0. We use the largest single submesh
    # bone_count, which is conservative but always correct: any submesh
    # references at most that many distinct bones.
    if bone_count_max == 0 and n_subs > 0:
        max_bc = 0
        for i in range(n_subs):
            bc = u16(submeshes_data, i * TBC_SUBMESH_SIZE + 0x0C)
            if bc > max_bc:
                max_bc = bc
        bone_count_max = max_bc

    return TBCSkinProfile(
        vertex_indices_count=n_verts,
        vertex_indices_data=vertex_indices_data,
        triangle_indices_count=n_tris,
        triangle_indices_data=triangle_indices_data,
        bone_indices_count=n_bones,
        bone_indices_data=bone_indices_data,
        submeshes_count=n_subs,
        submeshes_data=submeshes_data,
        texture_units_count=n_units,
        texture_units_data=texture_units_data,
        bone_count_max=bone_count_max,
    )


# ---------------------------------------------------------------------------
# Track conversion (WotLK -> TBC)
# ---------------------------------------------------------------------------


@dataclass
class TBCTrackPayload:
    """All bytes required to materialize a single TBC M2Track in the file.

    The `serialize_into` method writes the M2Track header at the requested
    offset using the offsets in `*_offset` fields (which the writer fills
    in once it knows where it placed the trailing data blocks).
    """

    interp_type: int
    global_sequence: int
    interp_ranges_count: int
    interp_ranges_data: bytes  # M2Range[] = uint32 min, uint32 max
    timestamps_count: int
    timestamps_data: bytes  # uint32[]
    has_values: bool
    values_count: int
    values_data: bytes  # T[] (raw bytes)
    interp_ranges_offset: int = 0
    timestamps_offset: int = 0
    values_offset: int = 0


def convert_track(
    track: WotLKTrack,
    sequences: list[WotLKSequence],
    n_global_seq: int,
    identity_value: bytes | None = None,
) -> TBCTrackPayload:
    """Concat per-sequence WotLK keyframes into the TBC flat-array form.

    Real TBC v260/v263 models store interpolation_ranges with exactly
    ``nAnimations + nGlobalSequences`` entries: the first ``nAnimations``
    describe the slice of the flat timestamps array that belongs to each
    animation sequence, and the trailing ``nGlobalSequences`` entries
    describe global-loop slots (filled with ``(0, 0)`` when this track
    isn't tied to a global sequence).

    For each sequence we emit one M2Range ``(min, max)`` of indices into
    the flat timestamps array. Empty sequences end up pointing at index
    ``0`` -- TBC clients dereference ``min`` unconditionally so we must
    keep it in-bounds.

    When the source had no keyframe data at all we collapse the track to
    a fully-empty M2Track (zero interp_ranges, zero timestamps, zero
    values) which matches what real TBC models do for unanimated bones.
    """
    has_any_data = any(ts for ts in track.ts_per_seq)
    track_has_values = any(v is not None for v in track.vs_per_seq)

    if not has_any_data:
        return TBCTrackPayload(
            interp_type=track.interp_type,
            global_sequence=track.global_sequence,
            interp_ranges_count=0,
            interp_ranges_data=b"",
            timestamps_count=0,
            timestamps_data=b"",
            has_values=track_has_values,
            values_count=0,
            values_data=b"",
        )

    flat_ts = bytearray()
    flat_vs = bytearray()
    # Collect (min, max) tuples first so we can patch up empty-sequence
    # entries to point at a real keyframe (TBC clients dereference the
    # `min` index even for "single keyframe" ranges where min == max,
    # so an out-of-bounds index would read garbage values).
    range_pairs: list[tuple[int, int]] = []
    cum_idx = 0

    if track.global_sequence >= 0:
        # Global-sequence track: WotLK stored exactly one inner array
        # (placed under index 0 by the parser). The TBC layout still
        # carries (nAnimations + nGlobalSequences) range entries, but
        # only the slot at offset (nAnimations + global_sequence) is
        # populated; the rest stay at (0, 0). The timestamps live in
        # global-loop time so we don't shift them.
        ts_bytes = track.ts_per_seq[0] if track.ts_per_seq else b""
        n_keys = len(ts_bytes) // 4
        if n_keys:
            flat_ts.extend(ts_bytes)
        for _ in range(len(sequences)):
            range_pairs.append((-1, -1))  # placeholder -> (0, 0)
        for g_idx in range(n_global_seq):
            if g_idx == track.global_sequence and n_keys:
                range_pairs.append((0, n_keys - 1))
            else:
                range_pairs.append((-1, -1))
        if track_has_values and track.vs_per_seq and track.vs_per_seq[0] is not None:
            flat_vs.extend(track.vs_per_seq[0])
    else:
        # Per-sequence track. Append every sequence's keys back-to-back
        # and emit (cum_start, cum_end) per sequence, plus n_global_seq
        # trailing (0, 0) entries.
        #
        # When this animation has *no* keyframes for a given sequence and
        # the caller has supplied an `identity_value`, we emit two
        # placeholder keyframes at the sequence's (start, end) timestamps
        # carrying the identity value. This matches the pattern used by
        # every reference TBC v263 model we have (``Eredar.M2``,
        # ``Ogre.M2``, etc.), which never have completely-empty per-anim
        # slots inside an otherwise-populated bone track. Without this
        # padding, the TBC client computes a degenerate transform when
        # playing those animations, which collapses the body submesh to
        # the origin and makes it visually disappear while the rig
        # continues to animate (this is the well-known "body invisible
        # but skeleton works" symptom).
        for s_idx, seq in enumerate(sequences):
            ts_bytes = track.ts_per_seq[s_idx] if s_idx < len(track.ts_per_seq) else b""
            n_keys = len(ts_bytes) // 4
            if n_keys:
                ts_arr = struct.unpack_from(f"<{n_keys}I", ts_bytes, 0)
                shifted = [(t + seq.start_timestamp) & 0xFFFFFFFF for t in ts_arr]
                flat_ts.extend(struct.pack(f"<{n_keys}I", *shifted))
                range_pairs.append((cum_idx, cum_idx + n_keys - 1))
                cum_idx += n_keys
                vs_bytes = track.vs_per_seq[s_idx] if s_idx < len(track.vs_per_seq) else None
                if vs_bytes is not None:
                    flat_vs.extend(vs_bytes)
            elif identity_value is not None:
                pad_ts = (seq.start_timestamp & 0xFFFFFFFF, seq.end_timestamp & 0xFFFFFFFF)
                flat_ts.extend(struct.pack("<II", *pad_ts))
                range_pairs.append((cum_idx, cum_idx + 1))
                cum_idx += 2
                flat_vs.extend(identity_value + identity_value)
            else:
                range_pairs.append((-1, -1))
                vs_bytes = track.vs_per_seq[s_idx] if s_idx < len(track.vs_per_seq) else None
                if vs_bytes is not None:
                    flat_vs.extend(vs_bytes)
        # Trailing global-sequence slots (unused for non-global tracks).
        for _ in range(n_global_seq):
            range_pairs.append((-1, -1))

    interp_ranges = bytearray()
    for rmin, rmax in range_pairs:
        if rmin < 0:
            interp_ranges.extend(struct.pack("<II", 0, 0))
        else:
            interp_ranges.extend(struct.pack("<II", rmin, rmax))

    return TBCTrackPayload(
        interp_type=track.interp_type,
        global_sequence=track.global_sequence,
        interp_ranges_count=(len(interp_ranges) // 8),
        interp_ranges_data=bytes(interp_ranges),
        timestamps_count=(len(flat_ts) // 4),
        timestamps_data=bytes(flat_ts),
        has_values=track_has_values,
        values_count=(len(flat_vs) // max(1, _values_size_from_track(track))) if track_has_values else 0,
        values_data=bytes(flat_vs) if track_has_values else b"",
    )


def _values_size_from_track(track: WotLKTrack) -> int:
    """Best-effort element size from per-sequence value bytes.

    All per-sequence value byte blobs in a single track must come from the
    same value-type, so any non-empty one tells us the element size given
    the matching timestamp count.
    """
    for s_idx, vs in enumerate(track.vs_per_seq):
        if vs is None or not vs:
            continue
        n = len(track.ts_per_seq[s_idx]) // 4
        if n > 0 and len(vs) % n == 0:
            return len(vs) // n
    return 1


# ---------------------------------------------------------------------------
# Serializer (writes TBC v260)
# ---------------------------------------------------------------------------


class TBCWriter:
    def __init__(self, src: WotLKReader):
        self.src = src
        self.out = bytearray()
        # Filled in as we lay things out.
        self.tbc_header = bytearray(TBC_HEADER_SIZE)

        # Compute global timeline.
        cum = 0
        for s in self.src.sequences:
            s.start_timestamp = cum
            s.end_timestamp = cum + s.duration
            cum = s.end_timestamp + GLOBAL_TIMELINE_GAP_MS

    # ----- low-level append helpers -----

    def _reserve_header(self) -> None:
        # Header is the first TBC_HEADER_SIZE bytes; everything else is appended.
        self.out.extend(self.tbc_header)

    def _set(self, offset: int, data: bytes) -> None:
        self.tbc_header[offset:offset + len(data)] = data

    def _pad4(self) -> None:
        while len(self.out) & 3:
            self.out.append(0)

    def _append(self, data: bytes) -> int:
        pos = len(self.out)
        self.out.extend(data)
        return pos

    def _array_field(self, offset: int, count: int, data_offset: int) -> None:
        self._set(offset, pack_m2arr(count, data_offset))

    # ----- track packing -----

    def _serialize_track(
        self,
        off_in_struct: int,
        track: WotLKTrack,
        struct_base: int,
        has_values: bool,
        identity_value: bytes | None = None,
    ) -> None:
        """Convert + write a TBC M2Track at `struct_base + off_in_struct`.

        Track payload (interp_ranges, timestamps, values bytes) is appended
        to `self.out` first; the struct's local M2Track header is then
        written into the in-place struct buffer.

        ``identity_value`` is the byte representation of the type-specific
        identity (e.g. ``struct.pack("<3f", 0, 0, 0)`` for translation).
        When supplied, empty per-animation slots are filled with two
        placeholder keyframes carrying this value -- see ``convert_track``
        for the rationale.
        """
        payload = convert_track(
            track, self.src.sequences, self.src.n_global_seq, identity_value=identity_value
        )
        self._pad4()
        ranges_off = self._append(payload.interp_ranges_data) if payload.interp_ranges_count else 0
        self._pad4()
        ts_off = self._append(payload.timestamps_data) if payload.timestamps_count else 0
        if has_values and payload.values_count:
            self._pad4()
            vs_off = self._append(payload.values_data)
        else:
            vs_off = 0
        track_size = TBC_TRACK_SIZE if has_values else TBC_TRACKBASE_SIZE
        # Header layout (TBC):
        #   uint16 interp_type
        #   int16  global_sequence
        #   M2Array<M2Range>  interpolation_ranges
        #   M2Array<uint32>   timestamps
        #   [M2Array<T>       values]   (omitted for events)
        struct.pack_into("<Hh", self.out, struct_base + off_in_struct + 0x00,
                         payload.interp_type, payload.global_sequence)
        struct.pack_into("<II", self.out, struct_base + off_in_struct + 0x04,
                         payload.interp_ranges_count, ranges_off)
        struct.pack_into("<II", self.out, struct_base + off_in_struct + 0x0C,
                         payload.timestamps_count, ts_off)
        if has_values:
            struct.pack_into("<II", self.out, struct_base + off_in_struct + 0x14,
                             payload.values_count, vs_off)
        return

    # ----- master serialization -----

    def serialize(self) -> bytes:
        s = self.src
        self._reserve_header()

        # Magic + version
        self._set(0x00, b"MD20")
        self._set(0x04, struct.pack("<I", TBC_VERSION))

        # Name (lname, ofsName) -- write name if present
        self._pad4()
        if s.name_bytes:
            ofs_name = self._append(s.name_bytes)
            self._array_field(0x08, len(s.name_bytes), ofs_name)
        else:
            self._array_field(0x08, 0, 0)

        # Global flags. Keep only the bits TBC understands (0x01 = TiltX,
        # 0x02 = TiltY, 0x04 = unknown, 0x08 = TextureCombiner, 0x10 = unk).
        # Anything above 0x1F is from MoP/WoD/Legion/etc. and would either
        # confuse a TBC client (e.g. the 0x20 PhysData flag asks for Cata
        # data we don't have) or simply not be applicable.
        flags = s.global_flags & 0x1F
        self._set(0x10, struct.pack("<I", flags))

        # Global sequences (uint32[])
        self._pad4()
        gs_off = self._append(s.global_sequences) if s.n_global_seq else 0
        self._array_field(0x14, s.n_global_seq, gs_off)

        # Sequences (M2Sequence[]) -- TBC layout, 68 bytes each
        seq_blob = b"".join(seq.serialize_tbc() for seq in s.sequences)
        self._pad4()
        seq_off = self._append(seq_blob) if s.n_anims else 0
        self._array_field(0x1C, s.n_anims, seq_off)

        # Sequence lookup
        self._pad4()
        sl_off = self._append(s.anim_lookup) if s.n_anim_lookup else 0
        self._array_field(0x24, s.n_anim_lookup, sl_off)

        # Playable animation lookup (TBC ONLY): leave empty for now. The
        # game uses this to map AnimationData.dbc entries to sequence slots
        # but our converted file already has the same sequence ordering as
        # the WotLK source so the engine fallback path works.
        self._array_field(0x2C, 0, 0)

        # Bones
        self._pad4()
        bones_buf = bytearray(s.n_bones * TBC_BONE_SIZE)
        bones_off = self._append(bytes(bones_buf))
        # Now overwrite each bone's bytes inside `self.out`.
        for i, bone in enumerate(s.bones):
            base = bones_off + i * TBC_BONE_SIZE
                # Header layout (TBC, total 16 bytes before tracks):
            #   int32  key_bone_id
            #   uint32 flags
            #   int16  parent_bone
            #   uint16 submesh_id
            #   uint32 bone_name_crc
            # Then 3x M2Track at 0x10, 0x2C, 0x48 and vec3D pivot at 0x64.
            struct.pack_into("<i", self.out, base + 0x00, bone.key_bone_id)
            struct.pack_into("<I", self.out, base + 0x04, bone.flags)
            struct.pack_into("<h", self.out, base + 0x08, bone.parent_bone)
            struct.pack_into("<H", self.out, base + 0x0A, bone.submesh_id)
            struct.pack_into("<I", self.out, base + 0x0C, bone.bone_name_crc & 0xFFFFFFFF)
            # Bone tracks: pad empty per-anim slots with the bind-pose
            # identity so the body submesh stays anchored to the rig
            # during animations that don't keyframe this bone (without
            # this padding the TBC client computes a degenerate transform
            # and the body collapses to the origin / disappears).
            self._serialize_track(
                0x10, bone.translation, base, has_values=True,
                identity_value=struct.pack("<fff", 0.0, 0.0, 0.0),
            )
            self._serialize_track(
                0x2C, bone.rotation, base, has_values=True,
                identity_value=struct.pack("<hhhh", 0, 0, 0, 32767),
            )
            self._serialize_track(
                0x48, bone.scale, base, has_values=True,
                identity_value=struct.pack("<fff", 1.0, 1.0, 1.0),
            )
            struct.pack_into("<fff", self.out, base + 0x64, *bone.pivot)
        self._array_field(0x34, s.n_bones, bones_off)

        # Key bone lookup
        self._pad4()
        kbone_off = self._append(s.kbone_lookup) if s.n_kbone_lookup else 0
        self._array_field(0x3C, s.n_kbone_lookup, kbone_off)

        # Vertices (raw 36-byte vertex data, copy verbatim)
        self._pad4()
        verts_off = self._append(s.vertex_data) if s.n_vertices else 0
        self._array_field(0x44, s.n_vertices, verts_off)

        # Skin profiles (TBC ONLY: inline). Real TBC models always embed
        # exactly 4 views (LOD0..LOD3); the client picks one based on
        # camera distance. WotLK is the same. If the user only supplied
        # fewer .skin files we replicate the highest-detail one to fill
        # the remaining slots -- this matches LKBC_Converter's behaviour
        # and is what the client expects when it indexes into views[k]
        # for k up to ``nViews-1``.
        n_views_to_embed = 4
        self._pad4()
        # Reserve space for `n_views_to_embed` profiles back-to-back.
        skin_array_off = len(self.out)
        self.out.extend(b"\x00" * (n_views_to_embed * SKIN_PROFILE_SIZE_TBC))

        for view_idx in range(n_views_to_embed):
            # Use the matching LOD if we have one, else fall back to LOD0.
            skin_idx = view_idx if view_idx < len(s.skins) else 0
            prof = restructure_skin_for_tbc(s.skins[skin_idx])
            self._pad4()
            v_off = self._append(prof.vertex_indices_data)
            self._pad4()
            t_off = self._append(prof.triangle_indices_data)
            self._pad4()
            b_off = self._append(prof.bone_indices_data)
            self._pad4()
            sub_off = self._append(prof.submeshes_data)
            self._pad4()
            tu_off = self._append(prof.texture_units_data)

            base = skin_array_off + view_idx * SKIN_PROFILE_SIZE_TBC
            struct.pack_into("<II", self.out, base + 0x00, prof.vertex_indices_count, v_off)
            struct.pack_into("<II", self.out, base + 0x08, prof.triangle_indices_count, t_off)
            struct.pack_into("<II", self.out, base + 0x10, prof.bone_indices_count, b_off)
            struct.pack_into("<II", self.out, base + 0x18, prof.submeshes_count, sub_off)
            struct.pack_into("<II", self.out, base + 0x20, prof.texture_units_count, tu_off)
            struct.pack_into("<I",  self.out, base + 0x28, prof.bone_count_max)

        self._array_field(0x4C, n_views_to_embed, skin_array_off if n_views_to_embed else 0)

        # Colors (M2Color[] -- 56 bytes each in TBC)
        self._pad4()
        colors_buf = bytearray(s.n_colors * TBC_COLOR_SIZE)
        colors_off = self._append(bytes(colors_buf))
        # Color tracks: pad empty per-anim slots so every animation has
        # a valid (rgb, alpha) keyframe even when the source did not
        # keyframe this color for that animation. Without this, the TBC
        # client computes NaN/Inf alpha during the empty animations and
        # the entire batch using this color goes invisible.
        rgb_identity = struct.pack("<fff", 1.0, 1.0, 1.0)
        alpha_identity = struct.pack("<H", 32767)  # fixed16 = 1.0
        for i, c in enumerate(s.colors):
            base = colors_off + i * TBC_COLOR_SIZE
            self._serialize_track(
                0x00, c.color, base, has_values=True, identity_value=rgb_identity
            )
            self._serialize_track(
                0x1C, c.alpha, base, has_values=True, identity_value=alpha_identity
            )
        self._array_field(0x54, s.n_colors, colors_off if s.n_colors else 0)

        # Textures (M2Texture[] -- 16 bytes each, name strings appended)
        self._pad4()
        tex_array_off = len(self.out)
        self.out.extend(b"\x00" * (s.n_textures * 16))
        for i, (ttype, tflags, tname) in enumerate(s.textures):
            self._pad4()
            n_off = self._append(tname) if tname else 0
            base = tex_array_off + i * 16
            struct.pack_into("<II", self.out, base + 0x00, ttype, tflags)
            struct.pack_into("<II", self.out, base + 0x08, len(tname), n_off)
        self._array_field(0x5C, s.n_textures, tex_array_off if s.n_textures else 0)

        # Texture weights (transparency) -- M2Track<fixed16>[]. Pad empty
        # per-anim slots with the fully-opaque identity (32767 = 1.0)
        # for the same reason we pad color tracks.
        self._pad4()
        tw_off = len(self.out)
        self.out.extend(b"\x00" * (s.n_transparency * TBC_TRACK_SIZE))
        for i, t in enumerate(s.transparencies):
            self._serialize_track(
                0x00, t, tw_off + i * TBC_TRACK_SIZE, has_values=True,
                identity_value=struct.pack("<H", 32767),
            )
        self._array_field(0x64, s.n_transparency, tw_off if s.n_transparency else 0)

        # Texture transforms (M2TextureTransform[] -- 84 bytes each in TBC)
        # NOTE: TBC v260/v263 has tex_anims at 0x6C (one slot earlier than
        # the converter previously placed it). Empirically verified against
        # ``Eredar.M2``, ``Ogre.M2``, etc. -- their model bbox starts at
        # 0xB4, which is only consistent with ``tex_anims`` at 0x6C and
        # exactly six lookup tables (materials, bone_lookup, tex_lookup,
        # tex_unit_lookup, transparency_lookup, uvanim_lookup) plus one
        # NEW M2Array (texture_combiner_combos) at 0xAC.
        self._pad4()
        tt_off = len(self.out)
        self.out.extend(b"\x00" * (s.n_tex_anims * TBC_TEXTURE_TRANSFORM_SIZE))
        # Texture transform tracks: pad empty per-anim slots with the
        # identity transform (vec3 zero translation, identity quaternion
        # rotation, vec3 one scaling).
        tex_trans_identity = struct.pack("<fff", 0.0, 0.0, 0.0)
        tex_rot_identity = struct.pack("<ffff", 0.0, 0.0, 0.0, 1.0)
        tex_scale_identity = struct.pack("<fff", 1.0, 1.0, 1.0)
        for i, t in enumerate(s.tex_transforms):
            base = tt_off + i * TBC_TEXTURE_TRANSFORM_SIZE
            self._serialize_track(
                0x00, t.translation, base, has_values=True,
                identity_value=tex_trans_identity,
            )
            self._serialize_track(
                0x1C, t.rotation, base, has_values=True,
                identity_value=tex_rot_identity,
            )
            self._serialize_track(
                0x38, t.scaling, base, has_values=True,
                identity_value=tex_scale_identity,
            )
        self._array_field(0x6C, s.n_tex_anims, tt_off if s.n_tex_anims else 0)

        # Replacable texture lookup (int16[])
        self._pad4()
        rt_off = self._append(s.replacable_texture_lookup) if s.n_tex_replace else 0
        self._array_field(0x74, s.n_tex_replace, rt_off)

        # Materials (M2Material[] -- 4 bytes each).
        self._pad4()
        mat_off = self._append(s.materials) if s.n_materials else 0
        self._array_field(0x7C, s.n_materials, mat_off)

        # NEW IN TBC v260/v263: an extra ``M2Array<uint16>`` between
        # Materials and BoneLookup. Looking at ``Ogre.M2`` (count=2 zeros)
        # and ``Eredar.M2`` (count=5, mostly zeros) it appears to be
        # tex_combiner_combos / bone_combos -- a small set of indices
        # used by the TBC content tools but ignored by the runtime when
        # the count is 0. WotLK has no source data here so we emit an
        # empty array. Crucially this slot MUST be present, otherwise
        # ``BoneLookup`` ends up at the wrong header offset and the body
        # submesh skins to garbage -> invisible body.
        self._array_field(0x84, 0, 0)

        # Bone lookup table -- moved from 0x84 (where the converter
        # previously placed it) to 0x8C to match real TBC v263 layout.
        self._pad4()
        bl_off = self._append(s.bone_lookup) if s.n_bone_lookup else 0
        self._array_field(0x8C, s.n_bone_lookup, bl_off)

        # Texture lookup
        self._pad4()
        tl_off = self._append(s.tex_lookup) if s.n_tex_lookup else 0
        self._array_field(0x94, s.n_tex_lookup, tl_off)

        # Tex unit lookup
        self._pad4()
        tul_off = self._append(s.tex_unit_lookup) if s.n_tex_unit_lookup else 0
        self._array_field(0x9C, s.n_tex_unit_lookup, tul_off)

        # Transparency lookup
        self._pad4()
        trl_off = self._append(s.trans_lookup) if s.n_trans_lookup else 0
        self._array_field(0xA4, s.n_trans_lookup, trl_off)

        # Texture transforms lookup (uvanim_lookup)
        self._pad4()
        ttl_off = self._append(s.tex_anim_lookup) if s.n_tex_anim_lookup else 0
        self._array_field(0xAC, s.n_tex_anim_lookup, ttl_off)

        # Bounding box / collision
        self._set(0xB4, s.bounding_box)
        self._set(0xCC, struct.pack("<f", s.bounding_radius))
        self._set(0xD0, s.collision_box)
        self._set(0xE8, struct.pack("<f", s.collision_radius))

        # Collision triangles
        self._pad4()
        ci_off = self._append(s.coll_idx) if s.n_coll_idx else 0
        self._array_field(0xEC, s.n_coll_idx, ci_off)

        # Collision vertices
        self._pad4()
        cv_off = self._append(s.coll_vert) if s.n_coll_vert else 0
        self._array_field(0xF4, s.n_coll_vert, cv_off)

        # Collision normals
        self._pad4()
        cn_off = self._append(s.coll_norm) if s.n_coll_norm else 0
        self._array_field(0xFC, s.n_coll_norm, cn_off)

        # Attachments (TBC: 48 bytes each)
        self._pad4()
        at_off = len(self.out)
        self.out.extend(b"\x00" * (s.n_attach * TBC_ATTACHMENT_SIZE))
        for i, a in enumerate(s.attachments):
            base = at_off + i * TBC_ATTACHMENT_SIZE
            struct.pack_into("<I", self.out, base + 0x00, a.id)
            struct.pack_into("<HH", self.out, base + 0x04, a.bone, a.unknown)
            struct.pack_into("<fff", self.out, base + 0x08, *a.position)
            self._serialize_track(0x14, a.animate_attached, base, has_values=True)
        self._array_field(0x104, s.n_attach, at_off if s.n_attach else 0)

        # Attachment lookup
        self._pad4()
        al_off = self._append(s.attach_lookup) if s.n_attach_lookup else 0
        self._array_field(0x10C, s.n_attach_lookup, al_off)

        # Events (TBC: 44 bytes each)
        self._pad4()
        ev_off = len(self.out)
        self.out.extend(b"\x00" * (s.n_events * TBC_EVENT_SIZE))
        for i, e in enumerate(s.events):
            base = ev_off + i * TBC_EVENT_SIZE
            self.out[base + 0x00:base + 0x04] = e.identifier
            struct.pack_into("<I", self.out, base + 0x04, e.data)
            struct.pack_into("<I", self.out, base + 0x08, e.bone)
            struct.pack_into("<fff", self.out, base + 0x0C, *e.position)
            self._serialize_track(0x18, e.enabled, base, has_values=False)
        self._array_field(0x114, s.n_events, ev_off if s.n_events else 0)

        # Lights (TBC: 212 bytes each)
        self._pad4()
        lt_off = len(self.out)
        self.out.extend(b"\x00" * (s.n_lights * TBC_LIGHT_SIZE))
        for i, l in enumerate(s.lights):
            base = lt_off + i * TBC_LIGHT_SIZE
            struct.pack_into("<Hh", self.out, base + 0x00, l.type_, l.bone)
            struct.pack_into("<fff", self.out, base + 0x04, *l.position)
            self._serialize_track(0x10, l.ambient_color, base, has_values=True)
            self._serialize_track(0x2C, l.ambient_intensity, base, has_values=True)
            self._serialize_track(0x48, l.diffuse_color, base, has_values=True)
            self._serialize_track(0x64, l.diffuse_intensity, base, has_values=True)
            self._serialize_track(0x80, l.attenuation_start, base, has_values=True)
            self._serialize_track(0x9C, l.attenuation_end, base, has_values=True)
            self._serialize_track(0xB8, l.visibility, base, has_values=True)
        self._array_field(0x11C, s.n_lights, lt_off if s.n_lights else 0)

        # Cameras (TBC: 124 bytes each)
        self._pad4()
        cm_off = len(self.out)
        self.out.extend(b"\x00" * (s.n_cameras * TBC_CAMERA_SIZE))
        for i, c in enumerate(s.cameras):
            base = cm_off + i * TBC_CAMERA_SIZE
            struct.pack_into("<i", self.out, base + 0x00, c.type_)
            struct.pack_into("<fff", self.out, base + 0x04, c.fov, c.far_clip, c.near_clip)
            self._serialize_track(0x10, c.positions, base, has_values=True)
            struct.pack_into("<fff", self.out, base + 0x2C, *c.position_base)
            self._serialize_track(0x38, c.target_position, base, has_values=True)
            struct.pack_into("<fff", self.out, base + 0x54, *c.target_position_base)
            self._serialize_track(0x60, c.roll, base, has_values=True)
        self._array_field(0x124, s.n_cameras, cm_off if s.n_cameras else 0)

        # Camera lookup
        self._pad4()
        cl_off = self._append(s.camera_lookup) if s.n_camera_lookup else 0
        self._array_field(0x12C, s.n_camera_lookup, cl_off)

        # Ribbon emitters (not converted -- emitted empty)
        self._array_field(0x134, 0, 0)

        # Particle emitters (not converted -- emitted empty)
        self._array_field(0x13C, 0, 0)

        # Finally, splice the header back in.
        self.out[0:TBC_HEADER_SIZE] = self.tbc_header
        return bytes(self.out)


# ---------------------------------------------------------------------------
# Public CLI
# ---------------------------------------------------------------------------


def convert(in_m2_path: str, out_m2_path: str, *, verbose: bool = True) -> None:
    src = WotLKReader(in_m2_path)
    if verbose:
        n_ext = sum(1 for s in src.sequences if s.is_external)
        n_alias = sum(1 for s in src.sequences if s.flags & SEQ_FLAG_ALIAS)
        print(f"[parse] {os.path.basename(in_m2_path)}: "
              f"{src.n_anims} anims ({n_ext} external, {n_alias} alias), "
              f"{src.n_bones} bones, {src.n_colors} colors, "
              f"{src.n_transparency} transparency tracks, "
              f"{src.n_tex_anims} texture transforms, "
              f"{src.n_lights} lights, {src.n_cameras} cameras, "
              f"{src.n_attach} attachments, {src.n_events} events, "
              f"{src.n_views} views (skin files found: {len(src.skins)})")
        if src.n_ribbon or src.n_particle:
            print(f"[warn] model has {src.n_ribbon} ribbons / {src.n_particle} "
                  f"particles -- these are not yet converted and will be emitted empty.")
    writer = TBCWriter(src)
    out_bytes = writer.serialize()
    with open(out_m2_path, "wb") as f:
        f.write(out_bytes)
    if verbose:
        print(f"[write] {out_m2_path}: {len(out_bytes)} bytes ({len(out_bytes) / 1024:.1f} KiB)")


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        print(f"\nusage: {sys.argv[0]} <input.m2> <output.m2>")
        return 2
    in_path, out_path = sys.argv[1], sys.argv[2]
    convert(in_path, out_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
