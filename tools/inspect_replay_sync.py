#!/usr/bin/env python3
"""Validate/compare TH08 replay-sync sidecars (schemas 2 through 8)."""

from __future__ import annotations

import argparse
import hashlib
import struct
import sys
from dataclasses import dataclass, fields
from pathlib import Path
from typing import Callable


MAGICS = {
    2: b"TH8RSA02",
    3: b"TH8RSA03",
    4: b"TH8RSA04",
    5: b"TH8RSA05",
    6: b"TH8RSA06",
    7: b"TH8RSA07",
    8: b"TH8RSA08",
}
SNAPSHOT_NAMES = {
    2: {1: "stage_begin", 2: "periodic", 3: "stage_terminal"},
    3: {1: "stage_begin", 2: "frame", 3: "stage_terminal"},
    4: {1: "stage_begin", 2: "frame", 3: "stage_terminal"},
    5: {1: "stage_begin", 2: "frame", 3: "stage_terminal"},
    6: {1: "stage_begin", 2: "frame", 3: "stage_terminal"},
    7: {1: "stage_begin", 2: "frame", 3: "stage_terminal"},
    8: {1: "stage_begin", 2: "frame", 3: "stage_terminal"},
}
DIGEST_NAMES = {
    2: ("replay", "game", "player", "spell", "population"),
    3: ("core", "player", "enemy", "projectile", "item_effect"),
    4: ("core", "player", "enemy", "projectile", "item_effect"),
    5: (),
    6: (),
    7: (),
    8: (),
}
LOCAL_LANE_NAMES = (
    "core_gameplay",
    "player_body",
    "player_options",
    "player_owned",
    "enemy_ecl",
    "projectile_items",
)
DIAGNOSTIC_LANE_NAMES = (
    "projectile_manager",
    "bullet_lifecycle",
    "bullet_kinematics",
    "bullet_transforms",
    "lasers",
    "item_lifecycle",
    "item_kinematics",
    "graze_effects",
    "bullet_counts",
    "object_counts",
)

RSA04_BUFFER_BYTES = 1024 * 1024
RSA04_LOCAL_LANE_COUNT = len(LOCAL_LANE_NAMES)
RSA04_LOCAL_LANE_ALGORITHM = 1  # FNV-1a 32
RSA04_LOCAL_LANE_RECORD_OFFSET = 72
RSA04_KNOWN_REQUIRED_FRAME_COUNT = 7020
RSA04_LIFECYCLE_RECORDS = 2  # stage begin and stage terminal
RSA05_DIAGNOSTIC_LANE_COUNT = len(DIAGNOSTIC_LANE_NAMES)
RSA05_DIAGNOSTIC_LANE_RECORD_OFFSET = 32
RSA06_RECORD_BYTES = 112
RSA06_ITEM_SPAWN_WITNESS_RECORD_OFFSET = 96
RSA06_ITEM_SPAWN_WITNESS_WORDS = 4
RSA06_KNOWN_DEMO2_FRAME_COUNT = 6983
RSA07_HEADER_BYTES = 8192
RSA07_ITEM_SPAWN_BURST_DETAIL_OFFSET = 1024
RSA07_ITEM_SPAWN_BURST_DETAIL_MAGIC = b"TH8I7D01"
RSA07_ITEM_SPAWN_BURST_DETAIL_HEADER_BYTES = 96
RSA07_ITEM_SPAWN_BURST_ENTRY_BYTES = 40
RSA07_ITEM_SPAWN_BURST_ENTRY_OFFSET = (
    RSA07_ITEM_SPAWN_BURST_DETAIL_OFFSET
    + RSA07_ITEM_SPAWN_BURST_DETAIL_HEADER_BYTES
)
RSA07_ITEM_SPAWN_BURST_CAPACITY = 128
RSA07_ITEM_SPAWN_BURST_THRESHOLD = 64

ITEM_SPAWN_OUTCOME_NAMES = {
    0: "none",
    1: "accepted",
    2: "reject_x",
    3: "reject_time_first_slot",
    4: "reject_pool_full",
}

ReplayIdentity = tuple[int, ...]
Emit = Callable[[str], None]


def u16(data: bytes, offset: int) -> int:
    return struct.unpack_from("<H", data, offset)[0]


def i16(data: bytes, offset: int) -> int:
    return struct.unpack_from("<h", data, offset)[0]


def u32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


@dataclass(frozen=True)
class Header:
    schema: int
    header_bytes: int
    record_bytes: int
    record_count: int
    run_count: int
    used_bytes: int
    buffer_bytes: int
    error_flags: int
    stage_active: int
    pending_active: int
    platform: int
    identity_count: int
    identity_capacity: int
    identity_bytes: int
    identity_offset: int
    record_capacity: int
    periodic_interval: int
    periodic_frame_capacity: int
    single_replay_frame_capacity: int
    lifecycle_reserve: int
    local_lane_count: int
    local_lane_algorithm: int
    local_lane_record_offset: int
    known_required_frame_count: int


@dataclass(frozen=True)
class Record:
    snapshot_type: int
    stage: int
    input_kind: int
    flags: int
    run_ordinal: int
    error_flags: int
    input_mask: int
    identity_index: int
    replay_frame: int
    completed_frames: int
    rng_seed_begin: int
    rng_seed_end: int
    rng_generation_begin: int
    rng_generation_end: int
    digests: tuple[tuple[int, int], ...]
    diagnostic_lanes: tuple[int, ...]
    local_lanes: tuple[int, ...]
    item_spawn_witness: tuple[int, ...]


@dataclass(frozen=True)
class ItemSpawnBurstEntry:
    x_bits: int
    y_bits: int
    z_bits: int
    requested_type: int
    requested_state: int
    requested_next_index: int
    outcome: int
    effective_type: int
    effective_state: int
    effective_next_index: int


@dataclass(frozen=True)
class ItemSpawnBurstDetail:
    flags: int
    stage: int
    run_ordinal: int
    identity_index: int
    replay_frame: int
    completed_frames: int
    total_requests: int
    stored_requests: int
    input_mask: int
    input_kind: int
    rng_seed_begin: int
    rng_seed_end: int
    rng_generation_begin: int
    rng_generation_end: int
    entries: tuple[ItemSpawnBurstEntry, ...]


@dataclass(frozen=True)
class Sidecar:
    path: Path
    data: bytes
    header: Header
    identities: tuple[ReplayIdentity, ...]
    records: tuple[Record, ...]
    item_spawn_burst: ItemSpawnBurstDetail | None


@dataclass(frozen=True)
class AlignmentKey:
    identity: ReplayIdentity
    stage: int
    stage_occurrence: int
    snapshot_type: int
    replay_frame: int


@dataclass(frozen=True)
class AlignedRecord:
    key: AlignmentKey
    source_index: int
    record: Record


def parse_bytes(path: Path, data: bytes) -> Sidecar:
    if len(data) < 160:
        raise ValueError(f"{path}: truncated header ({len(data)} bytes)")
    schema = u32(data, 8)
    if schema not in MAGICS or data[:8] != MAGICS[schema]:
        raise ValueError(
            f"{path}: unsupported schema/magic schema={schema} magic={data[:8]!r}"
        )
    if u32(data, 20) != 0x01020304:
        raise ValueError(f"{path}: unsupported schema or endian marker")

    header = Header(
        schema=schema,
        header_bytes=u32(data, 12),
        record_bytes=u32(data, 16),
        record_count=u32(data, 24),
        run_count=u32(data, 28),
        used_bytes=u32(data, 32),
        buffer_bytes=u32(data, 36),
        error_flags=u32(data, 40),
        stage_active=u32(data, 88),
        pending_active=u32(data, 92),
        platform=u32(data, 96),
        identity_count=u32(data, 100),
        identity_capacity=u32(data, 104),
        identity_bytes=u32(data, 108),
        identity_offset=u32(data, 112),
        record_capacity=u32(data, 124),
        periodic_interval=u32(data, 128),
        periodic_frame_capacity=u32(data, 132),
        single_replay_frame_capacity=u32(data, 136),
        lifecycle_reserve=u32(data, 140),
        local_lane_count=u32(data, 144) if schema in (4, 5, 6, 7, 8) else 0,
        local_lane_algorithm=u32(data, 148) if schema in (4, 5, 6, 7, 8) else 0,
        local_lane_record_offset=u32(data, 152) if schema in (4, 5, 6, 7, 8) else 0,
        known_required_frame_count=u32(data, 156) if schema in (4, 5, 6, 7, 8) else 0,
    )
    expected_record_bytes = {
        2: 72,
        3: 72,
        4: 96,
        5: 96,
        6: RSA06_RECORD_BYTES,
        7: RSA06_RECORD_BYTES,
        8: RSA06_RECORD_BYTES,
    }[schema]
    expected_header_bytes = RSA07_HEADER_BYTES if schema in (7, 8) else 1024
    if (
        header.header_bytes != expected_header_bytes
        or header.record_bytes != expected_record_bytes
    ):
        raise ValueError(f"{path}: unexpected header/record geometry")
    if header.used_bytes != len(data):
        raise ValueError(
            f"{path}: header used_bytes={header.used_bytes}, file={len(data)}"
        )
    expected_used = header.header_bytes + header.record_count * header.record_bytes
    if expected_used != header.used_bytes:
        raise ValueError(f"{path}: record count does not match used_bytes")
    if header.record_count > header.record_capacity:
        raise ValueError(f"{path}: record count exceeds fixed capacity")
    if header.used_bytes > header.buffer_bytes:
        raise ValueError(f"{path}: used bytes exceed fixed trace buffer")
    if header.identity_count > header.identity_capacity:
        raise ValueError(f"{path}: identity count exceeds table capacity")
    identity_end = (
        header.identity_offset + header.identity_count * header.identity_bytes
    )
    expected_identity_bytes = {
        2: 24,
        3: 32,
        4: 32,
        5: 32,
        6: 32,
        7: 32,
        8: 32,
    }[schema]
    if (
        header.identity_bytes != expected_identity_bytes
        or identity_end > header.header_bytes
    ):
        raise ValueError(f"{path}: invalid replay identity table")
    if schema == 3:
        if header.buffer_bytes < 1024 * 1024:
            raise ValueError(f"{path}: RSA03 correctness buffer is smaller than 1 MiB")
        if header.periodic_interval != 1:
            raise ValueError(f"{path}: RSA03 must contain every logical replay frame")
        if header.single_replay_frame_capacity < 6119:
            raise ValueError(f"{path}: RSA03 cannot hold the known stage-5 demo")
    elif schema in (4, 5, 6, 7, 8):
        if header.buffer_bytes != RSA04_BUFFER_BYTES:
            raise ValueError(f"{path}: RSA04 correctness buffer must be exactly 1 MiB")
        if header.periodic_interval != 1:
            raise ValueError(f"{path}: RSA04 must contain every logical replay frame")
        expected_lane_metadata = (
            RSA04_LOCAL_LANE_COUNT,
            RSA04_LOCAL_LANE_ALGORITHM,
            RSA04_LOCAL_LANE_RECORD_OFFSET,
            RSA04_KNOWN_REQUIRED_FRAME_COUNT,
        )
        actual_lane_metadata = (
            header.local_lane_count,
            header.local_lane_algorithm,
            header.local_lane_record_offset,
            header.known_required_frame_count,
        )
        if actual_lane_metadata != expected_lane_metadata:
            raise ValueError(
                f"{path}: invalid RSA0{schema} local-lane metadata "
                f"expected={expected_lane_metadata} actual={actual_lane_metadata}"
            )
        required_records = (
            header.known_required_frame_count + RSA04_LIFECYCLE_RECORDS
        )
        if header.record_capacity < required_records:
            raise ValueError(
                f"{path}: RSA0{schema} capacity cannot hold 7020 frames plus begin/end"
            )
        if header.single_replay_frame_capacity < header.known_required_frame_count:
            raise ValueError(
                f"{path}: RSA0{schema} cannot hold the known 7020-frame demo"
            )

        if schema in (5, 6, 7, 8):
            diagnostic_end = (
                RSA05_DIAGNOSTIC_LANE_RECORD_OFFSET
                + RSA05_DIAGNOSTIC_LANE_COUNT * 4
            )
            if diagnostic_end != header.local_lane_record_offset:
                raise ValueError(
                    f"{path}: invalid RSA0{schema} diagnostic-lane geometry"
                )
        if schema in (6, 7, 8):
            local_lane_end = (
                header.local_lane_record_offset + header.local_lane_count * 4
            )
            witness_end = (
                RSA06_ITEM_SPAWN_WITNESS_RECORD_OFFSET
                + RSA06_ITEM_SPAWN_WITNESS_WORDS * 4
            )
            if (
                local_lane_end != RSA06_ITEM_SPAWN_WITNESS_RECORD_OFFSET
                or witness_end != header.record_bytes
            ):
                raise ValueError(
                    f"{path}: invalid RSA0{schema} SpawnItem-witness geometry"
                )
            if header.single_replay_frame_capacity < RSA06_KNOWN_DEMO2_FRAME_COUNT:
                raise ValueError(
                    f"{path}: RSA0{schema} cannot hold the known 6983-frame demo2"
                )

    identities = []
    for index in range(header.identity_count):
        base = header.identity_offset + index * header.identity_bytes
        identity = (
            u32(data, base),
            u16(data, base + 4),
            data[base + 6],
            data[base + 7],
            u32(data, base + 8),
            u32(data, base + 12),
            u32(data, base + 16),
            u32(data, base + 20),
        )
        if schema in (3, 4, 5, 6, 7, 8):
            identity += (
                data[base + 24],
                data[base + 25],
                data[base + 26],
                data[base + 27],
                i16(data, base + 28),
                u16(data, base + 30),
            )
        identities.append(identity)

    records = []
    for index in range(header.record_count):
        base = header.header_bytes + index * header.record_bytes
        digests = (
            tuple(
                (
                    u32(data, base + 32 + domain * 8),
                    u32(data, base + 36 + domain * 8),
                )
                for domain in range(5)
            )
            if schema in (2, 3, 4)
            else ()
        )
        diagnostic_lanes = (
            tuple(
                u32(
                    data,
                    base + RSA05_DIAGNOSTIC_LANE_RECORD_OFFSET + lane * 4,
                )
                for lane in range(RSA05_DIAGNOSTIC_LANE_COUNT)
            )
            if schema in (5, 6, 7, 8)
            else ()
        )
        local_lanes = (
            tuple(
                u32(data, base + header.local_lane_record_offset + lane * 4)
                for lane in range(header.local_lane_count)
            )
            if schema in (4, 5, 6, 7, 8)
            else ()
        )
        item_spawn_witness = (
            tuple(
                u32(
                    data,
                    base + RSA06_ITEM_SPAWN_WITNESS_RECORD_OFFSET + word * 4,
                )
                for word in range(RSA06_ITEM_SPAWN_WITNESS_WORDS)
            )
            if schema in (6, 7, 8)
            else ()
        )
        record = Record(
            snapshot_type=data[base],
            stage=data[base + 1],
            input_kind=data[base + 2],
            flags=data[base + 3],
            run_ordinal=u16(data, base + 4),
            error_flags=u16(data, base + 6),
            input_mask=u16(data, base + 8),
            identity_index=u16(data, base + 10),
            replay_frame=u32(data, base + 12),
            completed_frames=u32(data, base + 16),
            rng_seed_begin=u16(data, base + 20),
            rng_seed_end=u16(data, base + 22),
            rng_generation_begin=u32(data, base + 24),
            rng_generation_end=u32(data, base + 28),
            digests=digests,
            diagnostic_lanes=diagnostic_lanes,
            local_lanes=local_lanes,
            item_spawn_witness=item_spawn_witness,
        )
        if record.snapshot_type not in SNAPSHOT_NAMES[schema]:
            raise ValueError(f"{path}: record {index} has invalid snapshot type")
        if record.identity_index >= header.identity_count:
            raise ValueError(f"{path}: record {index} has invalid identity index")
        records.append(record)

    item_spawn_burst = None
    if schema in (7, 8):
        detail_base = RSA07_ITEM_SPAWN_BURST_DETAIL_OFFSET
        if (
            data[detail_base : detail_base + 8]
            != RSA07_ITEM_SPAWN_BURST_DETAIL_MAGIC
            or u32(data, detail_base + 8) != 1
            or u32(data, detail_base + 12)
            != RSA07_ITEM_SPAWN_BURST_DETAIL_HEADER_BYTES
            or u32(data, detail_base + 16) != RSA07_ITEM_SPAWN_BURST_ENTRY_BYTES
            or u32(data, detail_base + 20) != RSA07_ITEM_SPAWN_BURST_CAPACITY
            or u32(data, detail_base + 24) != RSA07_ITEM_SPAWN_BURST_THRESHOLD
            or u32(data, detail_base + 84)
            != RSA07_ITEM_SPAWN_BURST_ENTRY_OFFSET
            or u32(data, detail_base + 88)
            != RSA07_ITEM_SPAWN_BURST_CAPACITY
            * RSA07_ITEM_SPAWN_BURST_ENTRY_BYTES
        ):
            raise ValueError(
                f"{path}: invalid RSA0{schema} SpawnItem-burst geometry"
            )
        detail_end = (
            RSA07_ITEM_SPAWN_BURST_ENTRY_OFFSET
            + RSA07_ITEM_SPAWN_BURST_CAPACITY
            * RSA07_ITEM_SPAWN_BURST_ENTRY_BYTES
        )
        if detail_end > header.header_bytes:
            raise ValueError(
                f"{path}: RSA0{schema} SpawnItem-burst exceeds header"
            )

        flags = u32(data, detail_base + 28)
        total_requests = u32(data, detail_base + 52)
        stored_requests = u32(data, detail_base + 56)
        if (
            stored_requests > RSA07_ITEM_SPAWN_BURST_CAPACITY
            or stored_requests > total_requests
        ):
            raise ValueError(
                f"{path}: invalid RSA0{schema} SpawnItem-burst counts"
            )
        captured = (flags & 1) != 0
        if captured:
            if total_requests < RSA07_ITEM_SPAWN_BURST_THRESHOLD:
                raise ValueError(
                    f"{path}: RSA0{schema} captured burst is below its threshold"
                )
            if u32(data, detail_base + 40) >= header.identity_count:
                raise ValueError(
                    f"{path}: RSA0{schema} burst has invalid replay identity index"
                )
        elif total_requests != 0 or stored_requests != 0:
            raise ValueError(
                f"{path}: RSA0{schema} uncaptured burst publishes request counts"
            )

        burst_entries = []
        for index in range(stored_requests):
            base = (
                RSA07_ITEM_SPAWN_BURST_ENTRY_OFFSET
                + index * RSA07_ITEM_SPAWN_BURST_ENTRY_BYTES
            )
            burst_entries.append(
                ItemSpawnBurstEntry(
                    x_bits=u32(data, base + 0),
                    y_bits=u32(data, base + 4),
                    z_bits=u32(data, base + 8),
                    requested_type=u32(data, base + 12),
                    requested_state=u32(data, base + 16),
                    requested_next_index=u32(data, base + 20),
                    outcome=u32(data, base + 24),
                    effective_type=u32(data, base + 28),
                    effective_state=u32(data, base + 32),
                    effective_next_index=u32(data, base + 36),
                )
            )
        item_spawn_burst = ItemSpawnBurstDetail(
            flags=flags,
            stage=u32(data, detail_base + 32),
            run_ordinal=u32(data, detail_base + 36),
            identity_index=u32(data, detail_base + 40),
            replay_frame=u32(data, detail_base + 44),
            completed_frames=u32(data, detail_base + 48),
            total_requests=total_requests,
            stored_requests=stored_requests,
            input_mask=u32(data, detail_base + 60),
            input_kind=u32(data, detail_base + 64),
            rng_seed_begin=u32(data, detail_base + 68),
            rng_seed_end=u32(data, detail_base + 72),
            rng_generation_begin=u32(data, detail_base + 76),
            rng_generation_end=u32(data, detail_base + 80),
            entries=tuple(burst_entries),
        )

    return Sidecar(
        path,
        data,
        header,
        tuple(identities),
        tuple(records),
        item_spawn_burst,
    )


def parse(path: Path) -> Sidecar:
    return parse_bytes(path, path.read_bytes())


def describe(sidecar: Sidecar) -> None:
    header = sidecar.header
    sha256 = hashlib.sha256(sidecar.data).hexdigest()
    snapshot_names = SNAPSHOT_NAMES[header.schema]
    type_counts = {name: 0 for name in snapshot_names.values()}
    for record in sidecar.records:
        type_counts[snapshot_names[record.snapshot_type]] += 1
    last = sidecar.records[-1] if sidecar.records else None
    print(f"path={sidecar.path}")
    print(f"sha256={sha256}")
    print(
        f"schema={header.schema} bytes={len(sidecar.data)} records={header.record_count}/"
        f"{header.record_capacity} runs={header.run_count} identities="
        f"{header.identity_count} platform={header.platform}"
    )
    print(
        f"errors=0x{header.error_flags:08x} active={header.stage_active} "
        f"pending={header.pending_active} snapshots={type_counts}"
    )
    print(
        f"period={header.periodic_interval}f capacity="
        f"{header.single_replay_frame_capacity}f lifecycle_reserve="
        f"{header.lifecycle_reserve}"
    )
    if header.schema in (4, 5, 6, 7, 8):
        print(
            f"local_lanes={header.local_lane_count} algorithm="
            f"fnv1a32({header.local_lane_algorithm}) record_offset="
            f"{header.local_lane_record_offset} known_required_frames="
            f"{header.known_required_frame_count}"
        )
    if header.schema in (5, 6, 7, 8):
        print(
            f"diagnostic_lanes={RSA05_DIAGNOSTIC_LANE_COUNT} "
            f"record_offset={RSA05_DIAGNOSTIC_LANE_RECORD_OFFSET}"
        )
    if header.schema in (6, 7, 8):
        print(
            "item_spawn_witness_words="
            f"{RSA06_ITEM_SPAWN_WITNESS_WORDS} record_offset="
            f"{RSA06_ITEM_SPAWN_WITNESS_RECORD_OFFSET}"
        )
    if header.schema in (7, 8) and sidecar.item_spawn_burst is not None:
        detail = sidecar.item_spawn_burst
        print(
            "item_spawn_burst="
            f"captured={detail.flags & 1} threshold="
            f"{RSA07_ITEM_SPAWN_BURST_THRESHOLD} capacity="
            f"{RSA07_ITEM_SPAWN_BURST_CAPACITY} entries="
            f"{detail.stored_requests}/{detail.total_requests} "
            f"stage={detail.stage} replay_frame={detail.replay_frame} "
            f"flags=0x{detail.flags:08x}"
        )
    for index, identity in enumerate(sidecar.identities):
        print(f"identity[{index}]=({format_identity(identity)})")
    if last is not None:
        print(
            f"last={snapshot_names[last.snapshot_type]} stage={last.stage} "
            f"replay_frame={last.replay_frame} completed={last.completed_frames} "
            f"record_errors=0x{last.error_flags:04x} flags=0x{last.flags:02x}"
        )


def compare_byte_exact(left: Sidecar, right: Sidecar, emit: Emit = print) -> bool:
    limit = min(len(left.data), len(right.data))
    first_offset = next(
        (offset for offset in range(limit) if left.data[offset] != right.data[offset]),
        limit,
    )
    emit(
        f"compare=DIFFERENT first_offset={first_offset} "
        f"left_bytes={len(left.data)} right_bytes={len(right.data)}"
    )
    header_bytes = min(left.header.header_bytes, right.header.header_bytes)
    if first_offset >= header_bytes:
        record_bytes = min(left.header.record_bytes, right.header.record_bytes)
        record_index = (first_offset - header_bytes) // record_bytes
        emit(f"first_different_record={record_index}")
        if record_index < len(left.records) and record_index < len(right.records):
            lrec = left.records[record_index]
            rrec = right.records[record_index]
            emit(f"left_record={lrec}")
            emit(f"right_record={rrec}")
            digest_names = DIGEST_NAMES[left.header.schema]
            for domain, (ldigest, rdigest) in enumerate(
                zip(lrec.digests, rrec.digests)
            ):
                if ldigest != rdigest:
                    emit(
                        f"first_digest_domain={digest_names[domain]} "
                        f"left={ldigest} right={rdigest}"
                    )
                    break
    if (
        left.header.schema in (7, 8)
        and right.header.schema == left.header.schema
    ):
        emit_item_spawn_burst_comparison(left, right, emit)
    return False


def format_identity(identity: ReplayIdentity) -> str:
    result = (
        f"magic=0x{identity[0]:08x},version={identity[1]},"
        f"extended={identity[2]},user_data={identity[3]},"
        f"file_size={identity[4]},checksum=0x{identity[5]:08x},"
        f"compressed={identity[6]},decompressed={identity[7]}"
    )
    if len(identity) >= 14:
        result += (
            f",minor_version={identity[8]},shot_type={identity[9]},"
            f"difficulty={identity[10]},practice={identity[11]},"
            f"spellcard={identity[12]},major_version=0x{identity[13]:04x}"
        )
    return result


def format_item_spawn_witness(witness: tuple[int, ...]) -> str:
    if len(witness) != RSA06_ITEM_SPAWN_WITNESS_WORDS:
        return "unavailable"
    counts, request_digest, first_reject_x_bits, result_meta = witness
    outcome = (result_meta >> 28) & 0x7
    return (
        f"requests={counts & 0xff},accepted={(counts >> 8) & 0xff},"
        f"reject_x={(counts >> 16) & 0xff},"
        f"reject_time_first_slot={(counts >> 24) & 0xff},"
        f"request_digest=0x{request_digest:08x},"
        f"first_reject_x_bits=0x{first_reject_x_bits:08x},"
        f"last_outcome={ITEM_SPAWN_OUTCOME_NAMES.get(outcome, f'unknown_{outcome}')},"
        f"last_type={(result_meta >> 12) & 0xff},"
        f"last_state={(result_meta >> 20) & 0xff},"
        f"last_next_index={result_meta & 0xfff},"
        f"counter_overflow={(result_meta >> 31) & 1}"
    )


def format_alignment_key(key: AlignmentKey, schema: int) -> str:
    return (
        f"identity=({format_identity(key.identity)}) stage={key.stage} "
        f"stage_occurrence={key.stage_occurrence} "
        f"snapshot={SNAPSHOT_NAMES[schema][key.snapshot_type]} "
        f"replay_frame={key.replay_frame}"
    )


def aligned_records(sidecar: Sidecar) -> tuple[AlignedRecord, ...]:
    """Replace wire-local run/identity indexes with stable trace identities."""

    # run_ordinal is only used to associate records inside one sidecar.  The
    # resulting occurrence number is derived independently on each side from
    # stage-begin order for the same stable replay identity and stage.
    run_occurrences: dict[tuple[ReplayIdentity, int, int], int] = {}
    next_occurrence: dict[tuple[ReplayIdentity, int], int] = {}
    closed_runs: set[tuple[ReplayIdentity, int, int]] = set()
    result: list[AlignedRecord] = []

    for source_index, record in enumerate(sidecar.records):
        identity = sidecar.identities[record.identity_index]
        raw_run = (identity, record.stage, record.run_ordinal)
        stable_stage = (identity, record.stage)

        if record.snapshot_type == 1:
            if raw_run in run_occurrences:
                raise ValueError(
                    f"{sidecar.path}: duplicate stage-begin for record {source_index}"
                )
            occurrence = next_occurrence.get(stable_stage, 0)
            next_occurrence[stable_stage] = occurrence + 1
            run_occurrences[raw_run] = occurrence
        elif raw_run not in run_occurrences:
            raise ValueError(
                f"{sidecar.path}: record {source_index} has no preceding stage-begin"
            )
        elif raw_run in closed_runs:
            raise ValueError(
                f"{sidecar.path}: record {source_index} follows a stage-terminal"
            )

        key = AlignmentKey(
            identity=identity,
            stage=record.stage,
            stage_occurrence=run_occurrences[raw_run],
            snapshot_type=record.snapshot_type,
            replay_frame=record.replay_frame,
        )
        result.append(AlignedRecord(key, source_index, record))
        if record.snapshot_type == 3:
            closed_runs.add(raw_run)

    return tuple(result)


def emit_item_spawn_witness_first(
    pair: tuple[AlignedRecord, AlignedRecord] | None,
    schema: int,
    emit: Emit,
) -> None:
    if pair is None:
        emit("item_spawn_witness_first=NONE")
        return
    left_entry, right_entry = pair
    record = left_entry.record
    emit(
        "item_spawn_witness_first="
        f"{SNAPSHOT_NAMES[schema][record.snapshot_type]} "
        f"left_record={left_entry.source_index} "
        f"right_record={right_entry.source_index} stage={record.stage} "
        f"replay_frame={record.replay_frame}"
    )
    emit(
        "item_spawn_witness_left="
        f"{format_item_spawn_witness(record.item_spawn_witness)}"
    )
    emit(
        "item_spawn_witness_right="
        f"{format_item_spawn_witness(right_entry.record.item_spawn_witness)}"
    )


ITEM_SPAWN_BURST_ENTRY_FIELD_NAMES = tuple(
    field.name for field in fields(ItemSpawnBurstEntry)
)
ITEM_SPAWN_BURST_STABLE_METADATA_FIELDS = (
    "flags",
    "stage",
    "replay_frame",
    "completed_frames",
    "total_requests",
    "stored_requests",
    "input_mask",
    "input_kind",
    "rng_seed_begin",
    "rng_seed_end",
)


def signed_u32(value: int) -> int:
    return value if value < 0x80000000 else value - 0x100000000


def format_item_spawn_burst_entry(entry: ItemSpawnBurstEntry) -> str:
    outcome_name = ITEM_SPAWN_OUTCOME_NAMES.get(
        entry.outcome, f"unknown_{entry.outcome}"
    )
    return (
        f"x_bits=0x{entry.x_bits:08x},y_bits=0x{entry.y_bits:08x},"
        f"z_bits=0x{entry.z_bits:08x},"
        f"requested_type={signed_u32(entry.requested_type)},"
        f"requested_state={signed_u32(entry.requested_state)},"
        f"requested_next_index={signed_u32(entry.requested_next_index)},"
        f"outcome={outcome_name},"
        f"effective_type={signed_u32(entry.effective_type)},"
        f"effective_state={signed_u32(entry.effective_state)},"
        f"effective_next_index={signed_u32(entry.effective_next_index)}"
    )


def format_item_spawn_burst_detail(detail: ItemSpawnBurstDetail | None) -> str:
    if detail is None:
        return "unavailable"
    return (
        f"captured={detail.flags & 1},flags=0x{detail.flags:08x},"
        f"stage={detail.stage},run_ordinal={detail.run_ordinal},"
        f"identity_index={detail.identity_index},"
        f"replay_frame={detail.replay_frame},"
        f"completed={detail.completed_frames},"
        f"requests={detail.total_requests},stored={detail.stored_requests},"
        f"input=0x{detail.input_mask:04x},input_kind={detail.input_kind},"
        f"rng_seed_begin=0x{detail.rng_seed_begin:04x},"
        f"rng_seed_end=0x{detail.rng_seed_end:04x},"
        f"rng_generation_begin={detail.rng_generation_begin},"
        f"rng_generation_end={detail.rng_generation_end}"
    )


def emit_item_spawn_burst_comparison(
    left: Sidecar, right: Sidecar, emit: Emit
) -> None:
    left_detail = left.item_spawn_burst
    right_detail = right.item_spawn_burst
    emit(
        "item_spawn_burst_left="
        f"{format_item_spawn_burst_detail(left_detail)}"
    )
    emit(
        "item_spawn_burst_right="
        f"{format_item_spawn_burst_detail(right_detail)}"
    )
    if left_detail is None or right_detail is None:
        emit("item_spawn_burst_compare=UNAVAILABLE")
        return
    left_captured = (left_detail.flags & 1) != 0
    right_captured = (right_detail.flags & 1) != 0
    if left_captured != right_captured:
        emit("item_spawn_burst_compare=DIFFERENT reason=capture_presence")
        return
    if not left_captured:
        emit("item_spawn_burst_compare=IDENTICAL captured=0")
        return

    differing_metadata = [
        name
        for name in ITEM_SPAWN_BURST_STABLE_METADATA_FIELDS
        if getattr(left_detail, name) != getattr(right_detail, name)
    ]
    if differing_metadata:
        name = differing_metadata[0]
        emit(
            "item_spawn_burst_compare=DIFFERENT reason=metadata "
            f"differing_fields={','.join(differing_metadata)}"
        )
        emit(
            f"item_spawn_burst_first_metadata_field={name} "
            f"left={getattr(left_detail, name)} "
            f"right={getattr(right_detail, name)}"
        )
        return

    # The absolute generation counter includes RNG work performed before the
    # replay stage begins.  Cross-platform comparisons already treat that
    # base as launcher/runtime metadata; only the number of generations
    # consumed by the captured frame is stable simulation evidence.
    left_generation_delta = (
        left_detail.rng_generation_end - left_detail.rng_generation_begin
    ) & 0xFFFFFFFF
    right_generation_delta = (
        right_detail.rng_generation_end - right_detail.rng_generation_begin
    ) & 0xFFFFFFFF
    if left_generation_delta != right_generation_delta:
        emit(
            "item_spawn_burst_compare=DIFFERENT reason=metadata "
            "differing_fields=rng_generation_delta"
        )
        emit(
            "item_spawn_burst_first_metadata_field=rng_generation_delta "
            f"left={left_generation_delta} right={right_generation_delta}"
        )
        return
    if (
        left_detail.rng_generation_begin != right_detail.rng_generation_begin
        or left_detail.rng_generation_end != right_detail.rng_generation_end
    ):
        emit(
            "item_spawn_burst_rng_generation_absolute_ignored=1 "
            f"left_begin={left_detail.rng_generation_begin} "
            f"right_begin={right_detail.rng_generation_begin} "
            f"delta={left_generation_delta}"
        )

    for index, (left_entry, right_entry) in enumerate(
        zip(left_detail.entries, right_detail.entries)
    ):
        if left_entry == right_entry:
            continue
        differing_fields = [
            name
            for name in ITEM_SPAWN_BURST_ENTRY_FIELD_NAMES
            if getattr(left_entry, name) != getattr(right_entry, name)
        ]
        name = differing_fields[0]
        emit(
            "item_spawn_burst_compare=DIFFERENT reason=entry "
            f"first_index={index} differing_fields={','.join(differing_fields)}"
        )
        emit(
            f"item_spawn_burst_first_field={name} "
            f"left=0x{getattr(left_entry, name):08x} "
            f"right=0x{getattr(right_entry, name):08x}"
        )
        emit(
            f"item_spawn_burst_entry_left=index={index},"
            f"{format_item_spawn_burst_entry(left_entry)}"
        )
        emit(
            f"item_spawn_burst_entry_right=index={index},"
            f"{format_item_spawn_burst_entry(right_entry)}"
        )
        return

    if len(left_detail.entries) != len(right_detail.entries):
        emit(
            "item_spawn_burst_compare=DIFFERENT reason=entry_count "
            f"left={len(left_detail.entries)} right={len(right_detail.entries)}"
        )
        return
    emit(
        "item_spawn_burst_compare=IDENTICAL captured=1 "
        f"entries={len(left_detail.entries)} normalized=run_ordinal,identity_index"
    )


SEMANTIC_RECORD_FIELDS = (
    "snapshot_type",
    "stage",
    "input_kind",
    "flags",
    "error_flags",
    "input_mask",
    "replay_frame",
    "completed_frames",
    "rng_seed_begin",
    "rng_seed_end",
    "rng_generation_begin",
    "rng_generation_end",
)
RSA04_STABLE_RECORD_FIELDS = (
    "snapshot_type",
    "stage",
    "input_kind",
    "flags",
    "error_flags",
    "input_mask",
    "replay_frame",
    "completed_frames",
    "rng_seed_begin",
    "rng_seed_end",
)


def compare_cross_platform(
    left: Sidecar, right: Sidecar, emit: Emit = print
) -> bool:
    # These fields describe the capture process, not simulation compatibility.
    # A stopped PC oracle and a completed PSP playback legitimately differ in
    # length/lifecycle metadata.  Compare their common identity/stage/frame
    # records first, then diagnose the remaining trace geometry.
    dynamic_metadata = {
        "platform",
        "run_count",
        "record_count",
        "used_bytes",
        "stage_active",
        "pending_active",
    }
    for field in fields(Header):
        if field.name in dynamic_metadata:
            continue
        left_value = getattr(left.header, field.name)
        right_value = getattr(right.header, field.name)
        if left_value != right_value:
            emit(
                "compare=CROSS_PLATFORM_SEMANTIC_DIFFERENT "
                f"left_platform={left.header.platform} "
                f"right_platform={right.header.platform} reason=header"
            )
            emit(
                f"first_header_field={field.name} "
                f"left={left_value} right={right_value}"
            )
            return False

    # Identity table indexes and table order are wire-local.  Compare the
    # explicit decoded identity values as a multiset instead.
    if sorted(left.identities) != sorted(right.identities):
        emit(
            "compare=CROSS_PLATFORM_SEMANTIC_DIFFERENT "
            f"left_platform={left.header.platform} "
            f"right_platform={right.header.platform} reason=identity_table"
        )
        left_only = next(
            (identity for identity in left.identities if identity not in right.identities),
            None,
        )
        right_only = next(
            (identity for identity in right.identities if identity not in left.identities),
            None,
        )
        if left_only is not None:
            emit(f"left_only_identity=({format_identity(left_only)})")
        if right_only is not None:
            emit(f"right_only_identity=({format_identity(right_only)})")
        return False

    left_aligned = aligned_records(left)
    right_aligned = aligned_records(right)
    left_by_key = {entry.key: entry for entry in left_aligned}
    right_by_key = {entry.key: entry for entry in right_aligned}
    if len(left_by_key) != len(left_aligned) or len(right_by_key) != len(right_aligned):
        raise ValueError("semantic replay-sync alignment produced a duplicate key")

    # Source order is trace chronology.  Keep scanning every shared aligned
    # record before diagnosing a missing tail or middle record.  RSA04's fresh
    # local lanes get a complete first pass so presentation-dependent poison in
    # an earlier legacy rolling digest/generation counter cannot mask a later
    # simulation-local divergence.
    common_pairs = tuple(
        (left_entry, right_by_key[left_entry.key])
        for left_entry in left_aligned
        if left_entry.key in right_by_key
    )
    common_records = len(common_pairs)
    item_spawn_witness_first: tuple[
        AlignedRecord, AlignedRecord
    ] | None = None
    if left.header.schema in (4, 5, 6, 7, 8):
        lane_first: list[
            tuple[AlignedRecord, AlignedRecord] | None
        ] = [None] * RSA04_LOCAL_LANE_COUNT
        lane_first_frame: list[
            tuple[AlignedRecord, AlignedRecord] | None
        ] = [None] * RSA04_LOCAL_LANE_COUNT
        diagnostic_first: list[
            tuple[AlignedRecord, AlignedRecord] | None
        ] = [None] * RSA05_DIAGNOSTIC_LANE_COUNT
        diagnostic_first_frame: list[
            tuple[AlignedRecord, AlignedRecord] | None
        ] = [None] * RSA05_DIAGNOSTIC_LANE_COUNT
        first_pair: tuple[AlignedRecord, AlignedRecord] | None = None
        first_pair_lanes: list[int] = []
        for left_entry, right_entry in common_pairs:
            left_record = left_entry.record
            right_record = right_entry.record
            differing_lanes = [
                lane
                for lane, (left_lane, right_lane) in enumerate(
                    zip(left_record.local_lanes, right_record.local_lanes)
                )
                if left_lane != right_lane
            ]
            if left.header.schema in (5, 6, 7, 8):
                differing_diagnostics = [
                    lane
                    for lane, (left_lane, right_lane) in enumerate(
                        zip(
                            left_record.diagnostic_lanes,
                            right_record.diagnostic_lanes,
                        )
                    )
                    if left_lane != right_lane
                ]
                for lane in differing_diagnostics:
                    if diagnostic_first[lane] is None:
                        diagnostic_first[lane] = (left_entry, right_entry)
                    if (
                        left_record.snapshot_type == 2
                        and diagnostic_first_frame[lane] is None
                    ):
                        diagnostic_first_frame[lane] = (
                            left_entry,
                            right_entry,
                        )
            if (
                left.header.schema in (6, 7, 8)
                and item_spawn_witness_first is None
                and left_record.item_spawn_witness
                != right_record.item_spawn_witness
            ):
                item_spawn_witness_first = (left_entry, right_entry)
            if not differing_lanes:
                continue

            if first_pair is None:
                first_pair = (left_entry, right_entry)
                first_pair_lanes = differing_lanes
            for lane in differing_lanes:
                if lane_first[lane] is None:
                    lane_first[lane] = (left_entry, right_entry)
                if (
                    left_record.snapshot_type == 2
                    and lane_first_frame[lane] is None
                ):
                    lane_first_frame[lane] = (left_entry, right_entry)

        if first_pair is not None:
            left_entry, right_entry = first_pair
            left_record = left_entry.record
            right_record = right_entry.record
            emit(
                "compare=CROSS_PLATFORM_SEMANTIC_DIFFERENT "
                f"left_platform={left.header.platform} "
                f"right_platform={right.header.platform} reason=record"
            )
            emit(
                f"first_semantic_record_left={left_entry.source_index} "
                f"right={right_entry.source_index} "
                f"{format_alignment_key(left_entry.key, left.header.schema)}"
            )
            emit(
                f"first_divergent_frame={left_record.replay_frame} "
                f"stage={left_record.stage} input=0x{left_record.input_mask:04x} "
                f"rng_seed_begin=0x{left_record.rng_seed_begin:04x} "
                f"rng_seed_end=0x{left_record.rng_seed_end:04x}"
            )
            first_lane = first_pair_lanes[0]
            emit(
                f"first_local_lane={LOCAL_LANE_NAMES[first_lane]} "
                f"left=0x{left_record.local_lanes[first_lane]:08x} "
                f"right=0x{right_record.local_lanes[first_lane]:08x}"
            )
            emit(
                "divergent_local_lanes="
                + ",".join(LOCAL_LANE_NAMES[lane] for lane in first_pair_lanes)
            )

            # A presentation-only core difference can be present from the
            # stage-begin record onward.  Report each lane independently after
            # scanning the entire common trace so that it cannot hide a later
            # player/options/projectile divergence.  The frame-only lines make
            # the first gameplay-frame occurrence explicit as well.
            for lane, name in enumerate(LOCAL_LANE_NAMES):
                for prefix, pair in (
                    ("lane_first_", lane_first[lane]),
                    ("lane_first_frame_", lane_first_frame[lane]),
                ):
                    if pair is None:
                        emit(f"{prefix}{name}=NONE")
                        continue
                    lane_left, lane_right = pair
                    lane_record = lane_left.record
                    snapshot = SNAPSHOT_NAMES[left.header.schema][
                        lane_record.snapshot_type
                    ]
                    emit(
                        f"{prefix}{name}={snapshot} "
                        f"left_record={lane_left.source_index} "
                        f"right_record={lane_right.source_index} "
                        f"stage={lane_record.stage} "
                        f"replay_frame={lane_record.replay_frame} "
                        f"left=0x{lane_record.local_lanes[lane]:08x} "
                        f"right=0x{lane_right.record.local_lanes[lane]:08x}"
                    )
            if left.header.schema in (5, 6, 7, 8):
                emit("diagnostic_values_at_first_divergence:")
                for lane, name in enumerate(DIAGNOSTIC_LANE_NAMES):
                    emit(
                        f"diagnostic_at_first_{name}="
                        f"left=0x{left_record.diagnostic_lanes[lane]:08x} "
                        f"right=0x{right_record.diagnostic_lanes[lane]:08x}"
                    )
                for lane, name in enumerate(DIAGNOSTIC_LANE_NAMES):
                    for prefix, pair in (
                        ("diagnostic_first_", diagnostic_first[lane]),
                        (
                            "diagnostic_first_frame_",
                            diagnostic_first_frame[lane],
                        ),
                    ):
                        if pair is None:
                            emit(f"{prefix}{name}=NONE")
                            continue
                        lane_left, lane_right = pair
                        lane_record = lane_left.record
                        snapshot = SNAPSHOT_NAMES[left.header.schema][
                            lane_record.snapshot_type
                        ]
                        emit(
                            f"{prefix}{name}={snapshot} "
                            f"left_record={lane_left.source_index} "
                            f"right_record={lane_right.source_index} "
                            f"stage={lane_record.stage} "
                            f"replay_frame={lane_record.replay_frame} "
                            f"left=0x{lane_record.diagnostic_lanes[lane]:08x} "
                            f"right=0x{lane_right.record.diagnostic_lanes[lane]:08x}"
                        )
            if left.header.schema in (6, 7, 8):
                emit(
                    "item_spawn_witness_at_first_divergence_left="
                    f"{format_item_spawn_witness(left_record.item_spawn_witness)}"
                )
                emit(
                    "item_spawn_witness_at_first_divergence_right="
                    f"{format_item_spawn_witness(right_record.item_spawn_witness)}"
                )
                emit_item_spawn_witness_first(
                    item_spawn_witness_first, left.header.schema, emit
                )
            if left.header.schema in (7, 8):
                emit_item_spawn_burst_comparison(left, right, emit)
            return False

    record_fields = (
        RSA04_STABLE_RECORD_FIELDS
        if left.header.schema in (4, 5, 6, 7, 8)
        else SEMANTIC_RECORD_FIELDS
    )
    for left_entry, right_entry in common_pairs:
        left_record = left_entry.record
        right_record = right_entry.record
        differing_fields = [
            name
            for name in record_fields
            if getattr(left_record, name) != getattr(right_record, name)
        ]
        differing_domains = (
            []
            if left.header.schema in (4, 5, 6, 7, 8)
            else [
                domain
                for domain, (left_digest, right_digest) in enumerate(
                    zip(left_record.digests, right_record.digests)
                )
                if left_digest != right_digest
            ]
        )
        if not differing_fields and not differing_domains:
            continue

        emit(
            "compare=CROSS_PLATFORM_SEMANTIC_DIFFERENT "
            f"left_platform={left.header.platform} "
            f"right_platform={right.header.platform} reason=record"
        )
        emit(
            f"first_semantic_record_left={left_entry.source_index} "
            f"right={right_entry.source_index} "
            f"{format_alignment_key(left_entry.key, left.header.schema)}"
        )
        if left.header.schema in (4, 5, 6, 7, 8):
            emit(
                f"first_divergent_frame={left_record.replay_frame} "
                f"stage={left_record.stage} input=0x{left_record.input_mask:04x} "
                f"rng_seed_begin=0x{left_record.rng_seed_begin:04x} "
                f"rng_seed_end=0x{left_record.rng_seed_end:04x}"
            )
        else:
            emit(
                f"first_divergent_frame={left_record.replay_frame} "
                f"stage={left_record.stage} input=0x{left_record.input_mask:04x} "
                f"rng_seed=0x{left_record.rng_seed_end:04x} "
                f"rng_generation={left_record.rng_generation_end}"
            )
        if differing_fields:
            name = differing_fields[0]
            emit(
                f"first_record_field={name} "
                f"left={getattr(left_record, name)} "
                f"right={getattr(right_record, name)}"
            )
        if differing_domains:
            domain = differing_domains[0]
            emit(
                f"first_digest_domain={DIGEST_NAMES[left.header.schema][domain]} "
                f"left={left_record.digests[domain]} "
                f"right={right_record.digests[domain]}"
            )
            emit(
                "divergent_domains="
                + ",".join(
                    DIGEST_NAMES[left.header.schema][index]
                    for index in differing_domains
                )
            )
        if left.header.schema in (6, 7, 8):
            emit_item_spawn_witness_first(
                item_spawn_witness_first, left.header.schema, emit
            )
        if left.header.schema in (7, 8):
            emit_item_spawn_burst_comparison(left, right, emit)
        return False

    left_keys = left_by_key.keys()
    right_keys = right_by_key.keys()
    if left_keys != right_keys:
        is_prefix_or_extension = (
            left_keys <= right_keys or right_keys <= left_keys
        )
        reason = "trace_length" if is_prefix_or_extension else "record_alignment"
        emit(
            "compare=CROSS_PLATFORM_SEMANTIC_DIFFERENT "
            f"left_platform={left.header.platform} "
            f"right_platform={right.header.platform} reason={reason}"
        )
        emit(
            f"common_records={common_records} "
            f"left_records={len(left_aligned)} right_records={len(right_aligned)}"
        )
        left_only = next(
            (entry for entry in left_aligned if entry.key not in right_by_key),
            None,
        )
        right_only = next(
            (entry for entry in right_aligned if entry.key not in left_by_key),
            None,
        )
        if left_only is not None:
            emit(
                f"first_left_only_record={left_only.source_index} "
                f"{format_alignment_key(left_only.key, left.header.schema)}"
            )
        if right_only is not None:
            emit(
                f"first_right_only_record={right_only.source_index} "
                f"{format_alignment_key(right_only.key, right.header.schema)}"
            )
        if left.header.schema in (6, 7, 8):
            emit_item_spawn_witness_first(
                item_spawn_witness_first, left.header.schema, emit
            )
        if left.header.schema in (7, 8):
            emit_item_spawn_burst_comparison(left, right, emit)
        return False

    if left.header.schema == 4:
        ignored = " legacy_rolling_ignored=1 rng_generation_ignored=1"
    elif left.header.schema == 5:
        ignored = (
            " diagnostic_lanes_non_authoritative=1 "
            "rng_generation_ignored=1"
        )
    elif left.header.schema in (6, 7, 8):
        ignored = (
            " diagnostic_lanes_non_authoritative=1 "
            "item_spawn_witness_non_authoritative=1 "
            "rng_generation_ignored=1"
        )
        if left.header.schema in (7, 8):
            ignored += " item_spawn_burst_non_authoritative=1"
    else:
        ignored = ""
    emit(
        "compare=CROSS_PLATFORM_SEMANTIC_IDENTICAL "
        f"left_platform={left.header.platform} "
        f"right_platform={right.header.platform} "
        f"records={common_records} platform_metadata_ignored=1 "
        f"normalized=identity_index,run_ordinal{ignored}"
    )
    if left.header.schema in (6, 7, 8):
        emit_item_spawn_witness_first(
            item_spawn_witness_first, left.header.schema, emit
        )
    if left.header.schema in (7, 8):
        emit_item_spawn_burst_comparison(left, right, emit)
    return True


def compare(left: Sidecar, right: Sidecar, emit: Emit = print) -> bool:
    if left.data == right.data:
        emit("compare=BYTE_IDENTICAL")
        return True
    if left.header.platform != right.header.platform:
        return compare_cross_platform(left, right, emit)
    return compare_byte_exact(left, right, emit)


def synthetic_sidecar_bytes(
    platform: int,
    identities: tuple[ReplayIdentity, ...],
    selected_identity: ReplayIdentity,
    run_ordinal: int,
    schema: int = 3,
) -> bytes:
    """Build a minimal valid RSA02 through RSA08 trace for self-tests."""

    if schema not in MAGICS:
        raise ValueError(f"unsupported synthetic schema {schema}")

    header_bytes = RSA07_HEADER_BYTES if schema in (7, 8) else 1024
    record_bytes = {
        2: 72,
        3: 72,
        4: 96,
        5: 96,
        6: RSA06_RECORD_BYTES,
        7: RSA06_RECORD_BYTES,
        8: RSA06_RECORD_BYTES,
    }[schema]
    record_count = 3
    identity_bytes = 32 if schema in (3, 4, 5, 6, 7, 8) else 24
    identity_capacity = 27 if schema in (3, 4, 5, 6, 7, 8) else 32
    buffer_bytes = {
        2: 256 * 1024,
        3: 1024 * 1024,
        4: RSA04_BUFFER_BYTES,
        5: RSA04_BUFFER_BYTES,
        6: RSA04_BUFFER_BYTES,
        7: RSA04_BUFFER_BYTES,
        8: RSA04_BUFFER_BYTES,
    }[schema]
    record_capacity = (buffer_bytes - header_bytes) // record_bytes
    periodic_interval = 1 if schema in (3, 4, 5, 6, 7, 8) else 60
    lifecycle_reserve = 2 if schema in (3, 4, 5, 6, 7, 8) else 18
    data = bytearray(header_bytes + record_bytes * record_count)

    def put_u16(offset: int, value: int) -> None:
        struct.pack_into("<H", data, offset, value)

    def put_u32(offset: int, value: int) -> None:
        struct.pack_into("<I", data, offset, value)

    data[:8] = MAGICS[schema]
    put_u32(8, schema)
    put_u32(12, header_bytes)
    put_u32(16, record_bytes)
    put_u32(20, 0x01020304)
    put_u32(24, record_count)
    put_u32(28, 1)
    put_u32(32, len(data))
    put_u32(36, buffer_bytes)
    put_u32(76, 1)
    put_u32(80, 1)
    put_u32(84, 1)
    put_u32(96, platform)
    put_u32(100, len(identities))
    put_u32(104, identity_capacity)
    put_u32(108, identity_bytes)
    put_u32(112, 160)
    put_u32(120, 1)
    put_u32(124, record_capacity)
    put_u32(128, periodic_interval)
    put_u32(132, record_capacity * periodic_interval)
    put_u32(136, (record_capacity - lifecycle_reserve) * periodic_interval)
    put_u32(140, lifecycle_reserve)
    if schema in (4, 5, 6, 7, 8):
        put_u32(144, RSA04_LOCAL_LANE_COUNT)
        put_u32(148, RSA04_LOCAL_LANE_ALGORITHM)
        put_u32(152, RSA04_LOCAL_LANE_RECORD_OFFSET)
        put_u32(156, RSA04_KNOWN_REQUIRED_FRAME_COUNT)

    for index, identity in enumerate(identities):
        expected_fields = 14 if schema in (3, 4, 5, 6, 7, 8) else 8
        if len(identity) != expected_fields:
            raise ValueError(
                f"schema {schema} identity has {len(identity)} fields, "
                f"expected {expected_fields}"
            )
        base = 160 + index * identity_bytes
        put_u32(base, identity[0])
        put_u16(base + 4, identity[1])
        data[base + 6] = identity[2]
        data[base + 7] = identity[3]
        for field_index, value in enumerate(identity[4:8]):
            put_u32(base + 8 + field_index * 4, value)
        if schema in (3, 4, 5, 6, 7, 8):
            data[base + 24] = identity[8]
            data[base + 25] = identity[9]
            data[base + 26] = identity[10]
            data[base + 27] = identity[11]
            struct.pack_into("<h", data, base + 28, identity[12])
            put_u16(base + 30, identity[13])

    identity_index = identities.index(selected_identity)
    if schema in (7, 8):
        detail_base = RSA07_ITEM_SPAWN_BURST_DETAIL_OFFSET
        data[detail_base : detail_base + 8] = (
            RSA07_ITEM_SPAWN_BURST_DETAIL_MAGIC
        )
        put_u32(detail_base + 8, 1)
        put_u32(
            detail_base + 12, RSA07_ITEM_SPAWN_BURST_DETAIL_HEADER_BYTES
        )
        put_u32(detail_base + 16, RSA07_ITEM_SPAWN_BURST_ENTRY_BYTES)
        put_u32(detail_base + 20, RSA07_ITEM_SPAWN_BURST_CAPACITY)
        put_u32(detail_base + 24, RSA07_ITEM_SPAWN_BURST_THRESHOLD)
        put_u32(detail_base + 28, 1)  # captured
        put_u32(detail_base + 32, 5)
        put_u32(detail_base + 36, run_ordinal)
        put_u32(detail_base + 40, identity_index)
        put_u32(detail_base + 44, 59)
        put_u32(detail_base + 48, 60)
        put_u32(detail_base + 52, RSA07_ITEM_SPAWN_BURST_THRESHOLD)
        put_u32(detail_base + 56, RSA07_ITEM_SPAWN_BURST_THRESHOLD)
        put_u32(detail_base + 60, 0x0011)
        put_u32(detail_base + 64, 1)
        put_u32(detail_base + 68, 0x1235)
        put_u32(detail_base + 72, 0x2346)
        put_u32(detail_base + 76, 11)
        put_u32(detail_base + 80, 21)
        put_u32(detail_base + 84, RSA07_ITEM_SPAWN_BURST_ENTRY_OFFSET)
        put_u32(
            detail_base + 88,
            RSA07_ITEM_SPAWN_BURST_CAPACITY
            * RSA07_ITEM_SPAWN_BURST_ENTRY_BYTES,
        )
        for request_index in range(RSA07_ITEM_SPAWN_BURST_THRESHOLD):
            entry_base = (
                RSA07_ITEM_SPAWN_BURST_ENTRY_OFFSET
                + request_index * RSA07_ITEM_SPAWN_BURST_ENTRY_BYTES
            )
            requested_type = request_index % 8
            requested_state = request_index % 4
            entry = (
                0x3F800000 + request_index,
                0x40000000 + request_index,
                0x40400000 + request_index,
                requested_type,
                requested_state,
                request_index,
                1,  # accepted
                requested_type,
                requested_state,
                request_index,
            )
            for word, value in enumerate(entry):
                put_u32(entry_base + word * 4, value)

    record_specs = (
        (1, 0xFFFFFFFF, 0, 0),
        (2, 59, 60, 0x0011),
        (3, 119, 120, 0x0000),
    )
    for record_index, (snapshot_type, replay_frame, completed, input_mask) in enumerate(
        record_specs
    ):
        base = header_bytes + record_index * record_bytes
        data[base] = snapshot_type
        data[base + 1] = 5
        data[base + 2] = 1
        put_u16(base + 4, run_ordinal)
        put_u16(base + 8, input_mask)
        put_u16(base + 10, identity_index)
        put_u32(base + 12, replay_frame)
        put_u32(base + 16, completed)
        put_u16(base + 20, 0x1234 + record_index)
        put_u16(base + 22, 0x2345 + record_index)
        put_u32(base + 24, 10 + record_index)
        put_u32(base + 28, 20 + record_index)
        if schema in (5, 6, 7, 8):
            for lane in range(RSA05_DIAGNOSTIC_LANE_COUNT):
                put_u32(
                    base + RSA05_DIAGNOSTIC_LANE_RECORD_OFFSET + lane * 4,
                    3000 + record_index * 100 + lane,
                )
        else:
            for domain in range(len(DIGEST_NAMES[schema])):
                put_u32(
                    base + 32 + domain * 8,
                    1000 + record_index * 100 + domain * 2,
                )
                put_u32(
                    base + 36 + domain * 8,
                    1001 + record_index * 100 + domain * 2,
                )
        if schema in (4, 5, 6, 7, 8):
            for lane in range(RSA04_LOCAL_LANE_COUNT):
                put_u32(
                    base + RSA04_LOCAL_LANE_RECORD_OFFSET + lane * 4,
                    2000 + record_index * 100 + lane,
                )
        if schema in (6, 7, 8):
            witness = (
                (0, 0, 0xFFFFFFFF, 0)
                if record_index != 1
                else (
                    0x01000102,
                    0x89ABCDEF,
                    0xFFFFFFFF,
                    (3 << 28) | (3 << 20) | (7 << 12) | 123,
                )
            )
            for word, value in enumerate(witness):
                put_u32(
                    base + RSA06_ITEM_SPAWN_WITNESS_RECORD_OFFSET + word * 4,
                    value,
                )
    return bytes(data)


def run_self_test() -> int:
    identity_a: ReplayIdentity = (
        0x50523854,
        6,
        1,
        0,
        0x2345,
        0x89ABCDEF,
        0x2200,
        0x4400,
        100,
        1,
        2,
        0,
        -1,
        0x0100,
    )
    identity_b: ReplayIdentity = (
        0x50523854,
        6,
        0,
        1,
        0x3456,
        0x01234567,
        0x3300,
        0x5500,
        100,
        2,
        2,
        0,
        -1,
        0x0100,
    )
    left = parse_bytes(
        Path("<self-test-psp>"),
        synthetic_sidecar_bytes(1, (identity_a, identity_b), identity_a, 7),
    )
    desktop_platform_only = parse_bytes(
        Path("<self-test-desktop-platform>"),
        synthetic_sidecar_bytes(2, (identity_a, identity_b), identity_a, 7),
    )
    desktop_normalized = parse_bytes(
        Path("<self-test-desktop-normalized>"),
        synthetic_sidecar_bytes(2, (identity_b, identity_a), identity_a, 42),
    )
    same_platform_different = parse_bytes(
        Path("<self-test-psp-different-wire>"),
        synthetic_sidecar_bytes(1, (identity_b, identity_a), identity_a, 42),
    )

    output: list[str] = []
    if not compare(left, left, output.append) or output != ["compare=BYTE_IDENTICAL"]:
        raise AssertionError("same-platform byte-identical comparison regressed")

    output.clear()
    if not compare(left, desktop_platform_only, output.append):
        raise AssertionError("platform metadata was not ignored")
    if not output[0].startswith("compare=CROSS_PLATFORM_SEMANTIC_IDENTICAL"):
        raise AssertionError("cross-platform semantic result was not reported")

    output.clear()
    if not compare(left, desktop_normalized, output.append):
        raise AssertionError("run ordinal or identity index was not normalized")

    output.clear()
    if compare(left, same_platform_different, output.append):
        raise AssertionError("same-platform comparison stopped requiring exact bytes")
    if not output[0].startswith("compare=DIFFERENT"):
        raise AssertionError("same-platform byte difference report regressed")

    changed = bytearray(desktop_normalized.data)
    player_digest_offset = 1024 + 72 + 40
    put_value = u32(changed, player_digest_offset) ^ 1
    struct.pack_into("<I", changed, player_digest_offset, put_value)
    desktop_player_difference = parse_bytes(
        Path("<self-test-player-difference>"), bytes(changed)
    )
    output.clear()
    if compare(left, desktop_player_difference, output.append):
        raise AssertionError("cross-platform domain difference was accepted")
    if not any(line.startswith("first_digest_domain=player ") for line in output):
        raise AssertionError("first differing digest domain was not identified")

    # A shorter, still-active oracle must compare its common frame records
    # before trace length/lifecycle metadata is considered.  This mirrors the
    # observed 5,977-frame PC versus 6,119-frame PSP capture geometry.
    prefix_bytes = bytearray(desktop_normalized.data[: 1024 + 2 * 72])
    struct.pack_into("<I", prefix_bytes, 24, 2)  # record_count
    struct.pack_into("<I", prefix_bytes, 32, len(prefix_bytes))  # used_bytes
    struct.pack_into("<I", prefix_bytes, 88, 1)  # stage_active
    struct.pack_into("<I", prefix_bytes, 92, 1)  # pending_active
    desktop_prefix = parse_bytes(Path("<self-test-short-prefix>"), bytes(prefix_bytes))
    output.clear()
    if compare(left, desktop_prefix, output.append):
        raise AssertionError("a shorter trace prefix was accepted as complete")
    if not output[0].endswith("reason=trace_length"):
        raise AssertionError("shorter trace did not report a length difference")
    if not any(line.startswith("common_records=2 ") for line in output):
        raise AssertionError("shorter trace common range was not compared")

    divergent_prefix_bytes = bytearray(prefix_bytes)
    put_value = u32(divergent_prefix_bytes, player_digest_offset) ^ 1
    struct.pack_into("<I", divergent_prefix_bytes, player_digest_offset, put_value)
    divergent_prefix = parse_bytes(
        Path("<self-test-short-prefix-divergence>"), bytes(divergent_prefix_bytes)
    )
    output.clear()
    if compare(left, divergent_prefix, output.append):
        raise AssertionError("shorter trace domain difference was accepted")
    if not output[0].endswith("reason=record"):
        raise AssertionError("trace length masked a common-frame divergence")
    if not any(line.startswith("first_digest_domain=player ") for line in output):
        raise AssertionError("short-prefix divergence domain was not identified")

    legacy_identity: ReplayIdentity = identity_a[:8]
    legacy = parse_bytes(
        Path("<self-test-rsa02>"),
        synthetic_sidecar_bytes(
            1, (legacy_identity,), legacy_identity, 1, schema=2
        ),
    )
    if legacy.header.schema != 2 or len(legacy.identities[0]) != 8:
        raise AssertionError("RSA02 compatibility parse regressed")

    rsa04_left = parse_bytes(
        Path("<self-test-rsa04-psp>"),
        synthetic_sidecar_bytes(
            1, (identity_a, identity_b), identity_a, 7, schema=4
        ),
    )
    if (
        rsa04_left.header.record_bytes != 96
        or len(rsa04_left.records[0].local_lanes) != RSA04_LOCAL_LANE_COUNT
    ):
        raise AssertionError("RSA04 local-lane parse regressed")

    # Stage-begin presentation state may poison RSA03's rolling domains and
    # process-local RNG generation counter.  RSA04 comparisons deliberately
    # ignore both and use the fresh local lanes as the simulation authority.
    rsa04_poisoned_bytes = bytearray(
        synthetic_sidecar_bytes(
            2, (identity_b, identity_a), identity_a, 42, schema=4
        )
    )
    stage_begin_base = 1024
    for relative_offset in (24, 28, *range(32, 72, 4)):
        offset = stage_begin_base + relative_offset
        struct.pack_into(
            "<I",
            rsa04_poisoned_bytes,
            offset,
            u32(rsa04_poisoned_bytes, offset) ^ 1,
        )
    rsa04_poisoned = parse_bytes(
        Path("<self-test-rsa04-legacy-poison>"), bytes(rsa04_poisoned_bytes)
    )
    output.clear()
    if not compare(rsa04_left, rsa04_poisoned, output.append):
        raise AssertionError("RSA04 legacy rolling/generation poison was authoritative")
    if not any("legacy_rolling_ignored=1" in line for line in output):
        raise AssertionError("RSA04 ignored legacy state was not reported")

    rsa04_prefix_bytes = bytearray(rsa04_poisoned_bytes[: 1024 + 2 * 96])
    struct.pack_into("<I", rsa04_prefix_bytes, 24, 2)
    struct.pack_into("<I", rsa04_prefix_bytes, 32, len(rsa04_prefix_bytes))
    struct.pack_into("<I", rsa04_prefix_bytes, 88, 1)
    struct.pack_into("<I", rsa04_prefix_bytes, 92, 1)
    rsa04_prefix = parse_bytes(
        Path("<self-test-rsa04-short-prefix>"), bytes(rsa04_prefix_bytes)
    )
    output.clear()
    if compare(rsa04_left, rsa04_prefix, output.append):
        raise AssertionError("RSA04 accepted a shorter trace as complete")
    if not output[0].endswith("reason=trace_length"):
        raise AssertionError("RSA04 matched lanes did not defer to trace length")

    rsa04_player_difference_bytes = bytearray(rsa04_prefix_bytes)
    player_lane_offset = (
        1024
        + 96
        + RSA04_LOCAL_LANE_RECORD_OFFSET
        + LOCAL_LANE_NAMES.index("player_body") * 4
    )
    struct.pack_into(
        "<I",
        rsa04_player_difference_bytes,
        player_lane_offset,
        u32(rsa04_player_difference_bytes, player_lane_offset) ^ 1,
    )
    rsa04_player_difference = parse_bytes(
        Path("<self-test-rsa04-player-lane>"),
        bytes(rsa04_player_difference_bytes),
    )
    output.clear()
    if compare(rsa04_left, rsa04_player_difference, output.append):
        raise AssertionError("RSA04 local player-lane difference was accepted")
    if not any(line.startswith("first_local_lane=player_body ") for line in output):
        raise AssertionError("RSA04 first differing local lane was not identified")
    if "divergent_local_lanes=player_body" not in output:
        raise AssertionError("RSA04 divergent local-lane set was not reported")
    if not any("replay_frame=59" in line for line in output):
        raise AssertionError("RSA04 stage-begin poison masked the later lane divergence")

    # A core lane mismatch at stage begin must not stop the scan before later
    # frame-only player/option differences.  Capture the first occurrence for
    # every lane, plus its first ordinary frame occurrence, in one comparison.
    rsa04_multi_lane_bytes = bytearray(rsa04_poisoned_bytes)
    lane_mutations = (
        (0, "core_gameplay"),
        (1, "core_gameplay"),
        (1, "player_options"),
        (2, "projectile_items"),
    )
    for record_index, lane_name in lane_mutations:
        offset = (
            1024
            + record_index * 96
            + RSA04_LOCAL_LANE_RECORD_OFFSET
            + LOCAL_LANE_NAMES.index(lane_name) * 4
        )
        struct.pack_into(
            "<I",
            rsa04_multi_lane_bytes,
            offset,
            u32(rsa04_multi_lane_bytes, offset) ^ 1,
        )
    rsa04_multi_lane = parse_bytes(
        Path("<self-test-rsa04-multi-lane>"), bytes(rsa04_multi_lane_bytes)
    )
    output.clear()
    if compare(rsa04_left, rsa04_multi_lane, output.append):
        raise AssertionError("RSA04 accepted multiple local-lane differences")
    required_lane_lines = {
        "lane_first_core_gameplay=stage_begin": "core stage-begin first",
        "lane_first_frame_core_gameplay=frame": "core frame first",
        "lane_first_player_options=frame": "later option first",
        "lane_first_frame_player_options=frame": "later option frame first",
        "lane_first_projectile_items=stage_terminal": "terminal projectile first",
        "lane_first_frame_projectile_items=NONE": "no projectile frame difference",
        "lane_first_player_body=NONE": "unchanged player body",
        "lane_first_frame_player_body=NONE": "unchanged player body frame",
    }
    for prefix, description in required_lane_lines.items():
        if not any(line.startswith(prefix) for line in output):
            raise AssertionError(f"RSA04 did not report {description}")
    if not any(
        line.startswith("first_local_lane=core_gameplay ") for line in output
    ):
        raise AssertionError("RSA04 compatibility first-lane output regressed")
    if "divergent_local_lanes=core_gameplay" not in output:
        raise AssertionError("RSA04 compatibility divergent-lane output regressed")

    rsa04_seed_difference_bytes = bytearray(rsa04_poisoned_bytes)
    seed_offset = 1024 + 96 + 20
    struct.pack_into(
        "<H",
        rsa04_seed_difference_bytes,
        seed_offset,
        u16(rsa04_seed_difference_bytes, seed_offset) ^ 1,
    )
    rsa04_seed_difference = parse_bytes(
        Path("<self-test-rsa04-rng-seed>"), bytes(rsa04_seed_difference_bytes)
    )
    output.clear()
    if compare(rsa04_left, rsa04_seed_difference, output.append):
        raise AssertionError("RSA04 stable RNG-seed difference was accepted")
    if not any(
        line.startswith("first_record_field=rng_seed_begin ") for line in output
    ):
        raise AssertionError("RSA04 stable RNG-seed difference was not reported")

    rsa04_undersized_bytes = bytearray(rsa04_left.data)
    struct.pack_into(
        "<I",
        rsa04_undersized_bytes,
        124,
        RSA04_KNOWN_REQUIRED_FRAME_COUNT + RSA04_LIFECYCLE_RECORDS - 1,
    )
    try:
        parse_bytes(
            Path("<self-test-rsa04-undersized>"), bytes(rsa04_undersized_bytes)
        )
    except ValueError as error:
        if "7020 frames plus begin/end" not in str(error):
            raise AssertionError("RSA04 undersized-capacity diagnostic regressed")
    else:
        raise AssertionError("RSA04 accepted capacity below 7020 frames plus begin/end")

    rsa05_left = parse_bytes(
        Path("<self-test-rsa05-psp>"),
        synthetic_sidecar_bytes(
            1, (identity_a, identity_b), identity_a, 7, schema=5
        ),
    )
    if (
        rsa05_left.header.record_bytes != 96
        or len(rsa05_left.records[0].local_lanes) != RSA04_LOCAL_LANE_COUNT
        or len(rsa05_left.records[0].diagnostic_lanes)
        != RSA05_DIAGNOSTIC_LANE_COUNT
    ):
        raise AssertionError("RSA05 diagnostic/local-lane parse regressed")

    rsa05_diagnostic_only_bytes = bytearray(
        synthetic_sidecar_bytes(
            2, (identity_a, identity_b), identity_a, 42, schema=5
        )
    )
    diagnostic_offset = (
        1024
        + 96
        + RSA05_DIAGNOSTIC_LANE_RECORD_OFFSET
        + DIAGNOSTIC_LANE_NAMES.index("bullet_lifecycle") * 4
    )
    struct.pack_into(
        "<I",
        rsa05_diagnostic_only_bytes,
        diagnostic_offset,
        u32(rsa05_diagnostic_only_bytes, diagnostic_offset) ^ 1,
    )
    rsa05_diagnostic_only = parse_bytes(
        Path("<self-test-rsa05-diagnostic-only>"),
        bytes(rsa05_diagnostic_only_bytes),
    )
    output.clear()
    if not compare(rsa05_left, rsa05_diagnostic_only, output.append):
        raise AssertionError("RSA05 diagnostic-only lane became authoritative")
    if not any("diagnostic_lanes_non_authoritative=1" in line for line in output):
        raise AssertionError("RSA05 diagnostic-only policy was not reported")

    rsa05_projectile_difference_bytes = bytearray(rsa05_diagnostic_only_bytes)
    projectile_lane_offset = (
        1024
        + 96
        + RSA04_LOCAL_LANE_RECORD_OFFSET
        + LOCAL_LANE_NAMES.index("projectile_items") * 4
    )
    struct.pack_into(
        "<I",
        rsa05_projectile_difference_bytes,
        projectile_lane_offset,
        u32(rsa05_projectile_difference_bytes, projectile_lane_offset) ^ 1,
    )
    rsa05_projectile_difference = parse_bytes(
        Path("<self-test-rsa05-projectile-diagnostic>"),
        bytes(rsa05_projectile_difference_bytes),
    )
    output.clear()
    if compare(rsa05_left, rsa05_projectile_difference, output.append):
        raise AssertionError("RSA05 projectile lane difference was accepted")
    if not any(
        line.startswith("diagnostic_first_frame_bullet_lifecycle=frame ")
        for line in output
    ):
        raise AssertionError("RSA05 diagnostic first-frame split was not reported")
    if not any(
        line.startswith("diagnostic_at_first_bullet_lifecycle=")
        for line in output
    ):
        raise AssertionError("RSA05 diagnostic value at divergence was not reported")

    rsa06_left = parse_bytes(
        Path("<self-test-rsa06-psp>"),
        synthetic_sidecar_bytes(
            1, (identity_a, identity_b), identity_a, 7, schema=6
        ),
    )
    if (
        rsa06_left.header.record_bytes != RSA06_RECORD_BYTES
        or rsa06_left.header.single_replay_frame_capacity
        < RSA06_KNOWN_DEMO2_FRAME_COUNT
        or len(rsa06_left.records[1].item_spawn_witness)
        != RSA06_ITEM_SPAWN_WITNESS_WORDS
    ):
        raise AssertionError("RSA06 SpawnItem-witness geometry regressed")

    rsa06_witness_only_bytes = bytearray(
        synthetic_sidecar_bytes(
            2, (identity_a, identity_b), identity_a, 42, schema=6
        )
    )
    witness_digest_offset = (
        1024 + RSA06_RECORD_BYTES
        + RSA06_ITEM_SPAWN_WITNESS_RECORD_OFFSET + 4
    )
    struct.pack_into(
        "<I",
        rsa06_witness_only_bytes,
        witness_digest_offset,
        u32(rsa06_witness_only_bytes, witness_digest_offset) ^ 1,
    )
    rsa06_witness_only = parse_bytes(
        Path("<self-test-rsa06-witness-only>"),
        bytes(rsa06_witness_only_bytes),
    )
    output.clear()
    if not compare(rsa06_left, rsa06_witness_only, output.append):
        raise AssertionError("RSA06 diagnostic witness became authoritative")
    if not any(
        line.startswith("item_spawn_witness_first=frame ") for line in output
    ):
        raise AssertionError("RSA06 witness first difference was not reported")
    if not any(
        "item_spawn_witness_non_authoritative=1" in line for line in output
    ):
        raise AssertionError("RSA06 witness policy was not reported")
    if not any(
        line.startswith("item_spawn_witness_left=requests=2,accepted=1,")
        for line in output
    ):
        raise AssertionError("RSA06 witness counters were not decoded")

    rsa06_projectile_difference_bytes = bytearray(rsa06_witness_only_bytes)
    rsa06_projectile_lane_offset = (
        1024
        + RSA06_RECORD_BYTES
        + RSA04_LOCAL_LANE_RECORD_OFFSET
        + LOCAL_LANE_NAMES.index("projectile_items") * 4
    )
    struct.pack_into(
        "<I",
        rsa06_projectile_difference_bytes,
        rsa06_projectile_lane_offset,
        u32(rsa06_projectile_difference_bytes, rsa06_projectile_lane_offset) ^ 1,
    )
    rsa06_projectile_difference = parse_bytes(
        Path("<self-test-rsa06-projectile-witness>"),
        bytes(rsa06_projectile_difference_bytes),
    )
    output.clear()
    if compare(rsa06_left, rsa06_projectile_difference, output.append):
        raise AssertionError("RSA06 projectile lane difference was accepted")
    if not any(
        line.startswith("item_spawn_witness_at_first_divergence_left=")
        for line in output
    ):
        raise AssertionError("RSA06 divergence witness values were not reported")

    rsa07_left = parse_bytes(
        Path("<self-test-rsa07-psp>"),
        synthetic_sidecar_bytes(
            1, (identity_a, identity_b), identity_a, 7, schema=7
        ),
    )
    rsa07_detail = rsa07_left.item_spawn_burst
    if (
        rsa07_left.header.header_bytes != RSA07_HEADER_BYTES
        or rsa07_left.header.single_replay_frame_capacity
        < RSA04_KNOWN_REQUIRED_FRAME_COUNT
        or rsa07_detail is None
        or rsa07_detail.total_requests != RSA07_ITEM_SPAWN_BURST_THRESHOLD
        or len(rsa07_detail.entries) != RSA07_ITEM_SPAWN_BURST_THRESHOLD
    ):
        raise AssertionError("RSA07 SpawnItem-burst geometry/parse regressed")

    rsa07_burst_only_bytes = bytearray(
        synthetic_sidecar_bytes(
            2, (identity_b, identity_a), identity_a, 42, schema=7
        )
    )
    # Cross-platform launchers may have consumed a different absolute number
    # of RNG values before the stage.  Preserve the same per-frame delta and
    # prove that the raw entry comparison is still reached.
    rsa07_detail_base = RSA07_ITEM_SPAWN_BURST_DETAIL_OFFSET
    struct.pack_into("<I", rsa07_burst_only_bytes, rsa07_detail_base + 76, 1011)
    struct.pack_into("<I", rsa07_burst_only_bytes, rsa07_detail_base + 80, 1021)
    burst_difference_index = 37
    burst_difference_offset = (
        RSA07_ITEM_SPAWN_BURST_ENTRY_OFFSET
        + burst_difference_index * RSA07_ITEM_SPAWN_BURST_ENTRY_BYTES
    )
    struct.pack_into(
        "<I",
        rsa07_burst_only_bytes,
        burst_difference_offset,
        u32(rsa07_burst_only_bytes, burst_difference_offset) ^ 1,
    )
    rsa07_burst_only = parse_bytes(
        Path("<self-test-rsa07-burst-only>"),
        bytes(rsa07_burst_only_bytes),
    )
    output.clear()
    if not compare(rsa07_left, rsa07_burst_only, output.append):
        raise AssertionError("RSA07 diagnostic burst became authoritative")
    if not any(
        "item_spawn_burst_non_authoritative=1" in line for line in output
    ):
        raise AssertionError("RSA07 burst policy was not reported")
    if not any(
        line.startswith(
            "item_spawn_burst_compare=DIFFERENT reason=entry first_index=37"
        )
        for line in output
    ):
        raise AssertionError("RSA07 first differing burst entry was not reported")
    if not any(
        line.startswith("item_spawn_burst_rng_generation_absolute_ignored=1 ")
        for line in output
    ):
        raise AssertionError("RSA07 absolute RNG generation was not normalized")
    if not any(
        line.startswith("item_spawn_burst_first_field=x_bits ")
        for line in output
    ):
        raise AssertionError("RSA07 first differing burst field was not reported")
    if not any(
        line.startswith("item_spawn_burst_entry_left=index=37,")
        for line in output
    ) or not any(
        line.startswith("item_spawn_burst_entry_right=index=37,")
        for line in output
    ):
        raise AssertionError("RSA07 raw differing burst entries were not reported")

    rsa07_generation_delta_bytes = bytearray(
        synthetic_sidecar_bytes(
            2, (identity_b, identity_a), identity_a, 42, schema=7
        )
    )
    struct.pack_into(
        "<I", rsa07_generation_delta_bytes, rsa07_detail_base + 76, 1011
    )
    struct.pack_into(
        "<I", rsa07_generation_delta_bytes, rsa07_detail_base + 80, 1022
    )
    rsa07_generation_delta = parse_bytes(
        Path("<self-test-rsa07-generation-delta>"),
        bytes(rsa07_generation_delta_bytes),
    )
    output.clear()
    if not compare(rsa07_left, rsa07_generation_delta, output.append):
        raise AssertionError("RSA07 diagnostic generation delta became authoritative")
    if not any(
        line.startswith(
            "item_spawn_burst_compare=DIFFERENT reason=metadata "
            "differing_fields=rng_generation_delta"
        )
        for line in output
    ):
        raise AssertionError("RSA07 differing RNG generation delta was not reported")

    rsa07_projectile_difference_bytes = bytearray(rsa07_burst_only_bytes)
    rsa07_projectile_lane_offset = (
        RSA07_HEADER_BYTES
        + RSA06_RECORD_BYTES
        + RSA04_LOCAL_LANE_RECORD_OFFSET
        + LOCAL_LANE_NAMES.index("projectile_items") * 4
    )
    struct.pack_into(
        "<I",
        rsa07_projectile_difference_bytes,
        rsa07_projectile_lane_offset,
        u32(
            rsa07_projectile_difference_bytes,
            rsa07_projectile_lane_offset,
        )
        ^ 1,
    )
    rsa07_projectile_difference = parse_bytes(
        Path("<self-test-rsa07-projectile-burst>"),
        bytes(rsa07_projectile_difference_bytes),
    )
    output.clear()
    if compare(rsa07_left, rsa07_projectile_difference, output.append):
        raise AssertionError("RSA07 projectile lane difference was accepted")
    if not any(
        line.startswith("item_spawn_burst_compare=DIFFERENT reason=entry ")
        for line in output
    ):
        raise AssertionError("RSA07 divergence omitted burst-entry detail")

    rsa08_left = parse_bytes(
        Path("<self-test-rsa08-psp>"),
        synthetic_sidecar_bytes(
            1, (identity_a, identity_b), identity_a, 7, schema=8
        ),
    )
    rsa08_desktop = parse_bytes(
        Path("<self-test-rsa08-desktop>"),
        synthetic_sidecar_bytes(
            2, (identity_b, identity_a), identity_a, 42, schema=8
        ),
    )
    if (
        rsa08_left.header.header_bytes != RSA07_HEADER_BYTES
        or rsa08_left.header.record_bytes != RSA06_RECORD_BYTES
        or len(rsa08_left.records[0].local_lanes)
        != RSA04_LOCAL_LANE_COUNT
        or rsa08_left.item_spawn_burst is None
    ):
        raise AssertionError("RSA08 compatibility geometry/parse regressed")

    output.clear()
    if not compare(rsa08_left, rsa08_desktop, output.append):
        raise AssertionError("RSA08 identity/run normalization regressed")
    if not any(
        line.startswith("compare=CROSS_PLATFORM_SEMANTIC_IDENTICAL ")
        for line in output
    ):
        raise AssertionError("RSA08 semantic identity result was not reported")

    rsa08_core_difference_bytes = bytearray(rsa08_desktop.data)
    rsa08_core_lane_offset = (
        RSA07_HEADER_BYTES
        + RSA06_RECORD_BYTES
        + RSA04_LOCAL_LANE_RECORD_OFFSET
        + LOCAL_LANE_NAMES.index("core_gameplay") * 4
    )
    struct.pack_into(
        "<I",
        rsa08_core_difference_bytes,
        rsa08_core_lane_offset,
        u32(rsa08_core_difference_bytes, rsa08_core_lane_offset) ^ 1,
    )
    rsa08_core_difference = parse_bytes(
        Path("<self-test-rsa08-core-lane>"),
        bytes(rsa08_core_difference_bytes),
    )
    output.clear()
    if compare(rsa08_left, rsa08_core_difference, output.append):
        raise AssertionError("RSA08 core_gameplay lane difference was accepted")
    if not any(
        line.startswith("first_local_lane=core_gameplay ") for line in output
    ):
        raise AssertionError("RSA08 core_gameplay strictness was not reported")

    print("self_test=PASS cases=27 schemas=2,3,4,5,6,7,8")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("sidecar", type=Path, nargs="?")
    parser.add_argument("comparison", type=Path, nargs="?")
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="run deterministic synthetic RSA02 through RSA08 comparator tests",
    )
    parser.add_argument(
        "--allow-errors",
        action="store_true",
        help="do not fail solely because a sidecar reports audit errors",
    )
    args = parser.parse_args()

    if args.self_test:
        if args.sidecar is not None or args.comparison is not None:
            parser.error("--self-test does not accept sidecar paths")
        try:
            return run_self_test()
        except (AssertionError, ValueError, struct.error) as error:
            print(f"self_test=FAIL error={error}", file=sys.stderr)
            return 2
    if args.sidecar is None:
        parser.error("the following arguments are required: sidecar")

    try:
        left = parse(args.sidecar)
        describe(left)
        valid = args.allow_errors or (
            left.header.error_flags == 0
            and left.header.stage_active == 0
            and left.header.pending_active == 0
        )
        if args.comparison is not None:
            right = parse(args.comparison)
            describe(right)
            valid = valid and (
                args.allow_errors
                or (
                    right.header.error_flags == 0
                    and right.header.stage_active == 0
                    and right.header.pending_active == 0
                )
            )
            valid = compare(left, right) and valid
        return 0 if valid else 1
    except (OSError, ValueError, struct.error) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
