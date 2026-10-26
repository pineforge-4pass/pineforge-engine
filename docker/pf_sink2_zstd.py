"""ctypes codec for the separately built, pinned conformance library."""
from __future__ import annotations

import ctypes as C
import os
from functools import lru_cache


@lru_cache(maxsize=1)
def library():
    path = os.environ.get("PF_SINK_ZSTD_LIBRARY")
    if not path:
        raise ImportError("PF_SINK_ZSTD_LIBRARY must name the pinned lab build")
    try:
        lib = C.CDLL(path)
    except OSError as error:
        raise ValueError(f"PF_SINK_ZSTD_LIBRARY cannot load the pinned codec: {error}") from error
    for name, result, args in (
        ("ZSTD_versionNumber", C.c_uint, []),
        ("pineforge_sink_zstd_reference_build", C.c_char_p, []),
        ("ZSTD_isError", C.c_uint, [C.c_size_t]),
        ("ZSTD_getErrorName", C.c_char_p, [C.c_size_t]),
        ("ZSTD_compressBound", C.c_size_t, [C.c_size_t]),
        ("ZSTD_createCCtx", C.c_void_p, []),
        ("ZSTD_freeCCtx", C.c_size_t, [C.c_void_p]),
        ("ZSTD_CCtx_setParameter", C.c_size_t, [C.c_void_p, C.c_int, C.c_int]),
        ("ZSTD_CCtx_setPledgedSrcSize", C.c_size_t, [C.c_void_p, C.c_ulonglong]),
        ("ZSTD_compress2", C.c_size_t, [C.c_void_p, C.c_void_p, C.c_size_t, C.c_void_p, C.c_size_t]),
        ("ZSTD_decompress", C.c_size_t, [C.c_void_p, C.c_size_t, C.c_void_p, C.c_size_t]),
    ):
        try:
            function = getattr(lib, name)
        except AttributeError as error:
            raise ValueError(f"PF_SINK_ZSTD_LIBRARY lacks conformance symbol {name}: {error}") from error
        function.restype, function.argtypes = result, args
    if lib.ZSTD_versionNumber() != 10507:
        raise ValueError("conformance codec is not zstd 1.5.7")
    return lib


def checked(value):
    lib = library()
    if lib.ZSTD_isError(value):
        raise ValueError(lib.ZSTD_getErrorName(value).decode("ascii"))
    return value


def compress(raw: bytes, level: int, *, checksum: bool = False) -> bytes:
    lib = library()
    context = lib.ZSTD_createCCtx()
    if not context:
        raise MemoryError("zstd context")
    try:
        window = min(24, max(10, (len(raw) - 1).bit_length()))
        # Public zstd_cParameter values, pinned by the shipped zstd.h.
        for parameter, value in ((100, level), (101, window), (200, 1),
                                 (201, int(checksum)), (202, 0), (400, 0)):
            checked(lib.ZSTD_CCtx_setParameter(context, parameter, value))
        checked(lib.ZSTD_CCtx_setPledgedSrcSize(context, len(raw)))
        output = C.create_string_buffer(lib.ZSTD_compressBound(len(raw)))
        size = checked(lib.ZSTD_compress2(context, output, len(output), raw, len(raw)))
        return output.raw[:size]
    finally:
        checked(lib.ZSTD_freeCCtx(context))


def decompress(frame: bytes, size: int) -> bytes:
    # The independent reader checks window, frame extent and declared length
    # before this call. No attacker-declared allocation happens here.
    output = C.create_string_buffer(size)
    actual = checked(library().ZSTD_decompress(output, size, frame, len(frame)))
    if actual != size:
        raise ValueError("zstd decoded length mismatch")
    return output.raw[:actual]
