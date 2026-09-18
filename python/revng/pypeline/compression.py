#
# This file is distributed under the MIT License. See LICENSE.md for details.
#

from collections.abc import Buffer
try:
    from compression.zstd import COMPRESSION_LEVEL_DEFAULT, ZstdError
    from compression.zstd import compress as zstd_compress
    from compression.zstd import decompress as zstd_decompress
except ImportError:
    # compression.zstd is part of Python 3.14. Keep binary distributions usable
    # with the Python 3.13 currently shipped by Homebrew and many Linux hosts.
    import zstandard

    COMPRESSION_LEVEL_DEFAULT = 3
    ZstdError = zstandard.ZstdError

    def zstd_compress(data: Buffer, level: int) -> bytes:
        return zstandard.ZstdCompressor(level=level).compress(data)

    def zstd_decompress(data: Buffer) -> bytes:
        return zstandard.ZstdDecompressor().decompress(data)

from revng.pypeline.utils.cabc import ABC, abstractmethod


class Compression(ABC):
    """
    Class family that abstracts away a given compression
    """

    name: str
    compression_error: type[Exception]
    decompression_error: type[Exception]

    @abstractmethod
    def __init__(self, options: str | None = None): ...

    @abstractmethod
    def compress(self, data: Buffer) -> Buffer: ...

    @abstractmethod
    def decompress(self, data: Buffer) -> Buffer: ...


class ZstdCompression(Compression):
    name = "zstd"
    compression_error = ZstdError
    decompression_error = ZstdError

    def __init__(self, options: str | None = None):
        if options is None:
            self.level = COMPRESSION_LEVEL_DEFAULT
        else:
            assert options.startswith("level=")
            level_value = options.removeprefix("level=")
            self.level = int(level_value)

    def compress(self, data: Buffer) -> Buffer:
        return zstd_compress(data, self.level)

    def decompress(self, data: Buffer) -> Buffer:
        return zstd_decompress(data)
