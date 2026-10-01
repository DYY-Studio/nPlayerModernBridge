"""Stable identity pin for one decrypted nPlayer Mach-O executable."""

from __future__ import annotations

import hashlib
from pathlib import Path
from typing import Any

import lief


_COMMAND_VALUE_OFFSET = 12
_UINT32_SIZE = 4
_UINT64_SIZE = 8

LINKEDIT = "__LINKEDIT"
# LC_SEGMENT_64 fields, relative to the command start.
_SEGMENT_VMSIZE_OFFSET = 0x20
_SEGMENT_FILESIZE_OFFSET = 0x30


def _parse(path: Path) -> tuple[Any, Any]:
    container = lief.MachO.parse(str(path))
    binaries = list(container) if container is not None else []
    if len(binaries) != 1:
        raise ValueError(f"expected one Mach-O binary in {path}")
    return container, binaries[0]


def _field_offset(
    command: Any,
    raw_size: int,
    name: str,
    relative: int = _COMMAND_VALUE_OFFSET,
    size: int = _UINT32_SIZE,
) -> int:
    offset = int(command.command_offset) + relative
    if offset < 0 or offset + size > raw_size:
        raise ValueError(f"{name} field leaves the executable")
    return offset


def main_pin_sha256(path: Path) -> str:
    """Hash stable executable bytes while excluding extraction metadata.

    The code-signature blob sits inside `__LINKEDIT`, so its size leaks into the
    segment command's `vmsize`/`filesize`. Dumps of the same program made by
    different extraction or re-signing tools therefore carry different values
    there; those two fields are normalized as well, like `cryptsize` and the
    signature `datasize`.
    """

    path = Path(path)
    raw = bytearray(path.read_bytes())
    container, binary = _parse(path)
    if not binary.has_encryption_info:
        raise ValueError("input executable carries no LC_ENCRYPTION_INFO_64")
    if int(binary.encryption_info.crypt_id) != 0:
        raise ValueError("input is still FairPlay-encrypted (crypt_id != 0)")
    if not binary.has_code_signature:
        raise ValueError("input executable carries no LC_CODE_SIGNATURE")

    encryption = binary.encryption_info
    signature = binary.code_signature
    linkedit = binary.get_segment(LINKEDIT)
    if linkedit is None:
        raise ValueError("input executable carries no __LINKEDIT segment")
    crypt_size_offset = _field_offset(encryption, len(raw), "cryptsize")
    signature_size_offset = _field_offset(signature, len(raw), "signature datasize")
    linkedit_vmsize_offset = _field_offset(
        linkedit, len(raw), "__LINKEDIT vmsize", _SEGMENT_VMSIZE_OFFSET, _UINT64_SIZE
    )
    linkedit_filesize_offset = _field_offset(
        linkedit,
        len(raw),
        "__LINKEDIT filesize",
        _SEGMENT_FILESIZE_OFFSET,
        _UINT64_SIZE,
    )
    signature_start = int(signature.data_offset)
    signature_size = int(signature.data_size)
    signature_end = signature_start + signature_size
    if signature_start < 0 or signature_size <= 0 or signature_end > len(raw):
        raise ValueError("LC_CODE_SIGNATURE range leaves the executable")

    raw[crypt_size_offset : crypt_size_offset + _UINT32_SIZE] = b"\0" * _UINT32_SIZE
    raw[signature_size_offset : signature_size_offset + _UINT32_SIZE] = (
        b"\0" * _UINT32_SIZE
    )
    raw[linkedit_vmsize_offset : linkedit_vmsize_offset + _UINT64_SIZE] = (
        b"\0" * _UINT64_SIZE
    )
    raw[linkedit_filesize_offset : linkedit_filesize_offset + _UINT64_SIZE] = (
        b"\0" * _UINT64_SIZE
    )
    del raw[signature_start:signature_end]
    del binary
    del container
    return hashlib.sha256(raw).hexdigest()
