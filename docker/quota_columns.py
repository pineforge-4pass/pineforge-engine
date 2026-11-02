"""Two-file, bounded-window storage for comma-separated encoded JSON streams.

Streams 0..4 are trades (T), equity (E), opens, closes and events; numeric
columns start at 5. Callers supply encoded bytes without surrounding brackets
or leading/trailing commas. A batch's item_count includes its internal commas.
No JSON spelling, escaping or numeric conversion happens here.

The store is private and single-threaded. Its first read freezes all streams.
Readers must be exhausted before publishing their destination: corruption or
an incomplete append invalidates the provisional store.
"""
from __future__ import annotations

from dataclasses import dataclass
import os
import secrets
import struct
import tempfile

if __package__:
    from . import run_quota as quota
else:
    import run_quota as quota


MAX_SIZE = (1 << 53) - 1
NO_BLOCK = (1 << 64) - 1
BLOCK_SIZE = 65536
INDEX_ENTRY = struct.Struct("<QQQQ")
BLOCK_HEADER = struct.Struct("<QII")
NEXT_BLOCK = struct.Struct("<Q")
PAYLOAD_SIZE = BLOCK_SIZE - BLOCK_HEADER.size
DATA_WINDOWS = 4
FIXED_STREAMS = 5
_EMPTY_ENTRY = INDEX_ENTRY.pack(NO_BLOCK, NO_BLOCK, 0, 0)


class ColumnStoreCorruptError(ValueError):
    """A private store's framing or chain disagrees with its index."""


def _size(value):
    if type(value) is not int or value < 0:
        raise ValueError("column store sizes must be nonnegative integers")
    if value > MAX_SIZE:
        raise OverflowError("column store size exceeds 2**53-1")
    return value


def _add(left, right):
    _size(left)
    _size(right)
    if right > MAX_SIZE - left:
        raise OverflowError("column store size addition exceeds 2**53-1")
    return left + right


def _multiply(left, right):
    _size(left)
    _size(right)
    if right and left > MAX_SIZE // right:
        raise OverflowError("column store size multiplication exceeds 2**53-1")
    return left * right


def _rounded(size):
    return _add(size, (-size) % BLOCK_SIZE)


def _pwrite_all(descriptor, data, offset, produced=None):
    """Write a bounded view, retaining partial realization on an I/O error."""
    view = memoryview(data).cast("B")
    _add(offset, len(view))
    done = 0
    while done < len(view):
        written = os.pwrite(descriptor, view[done:], _add(offset, done))
        if written <= 0 or written > len(view) - done:
            raise OSError("short column store write")
        done = _add(done, written)
        if produced is not None:
            produced(written)


class _Window:
    __slots__ = ("base", "buffer")

    def __init__(self):
        self.base = None
        self.buffer = bytearray(BLOCK_SIZE)

    def load(self, descriptor, base):
        _add(base, BLOCK_SIZE)
        self.base = None
        done = 0
        while done < BLOCK_SIZE:
            chunk = os.pread(descriptor, BLOCK_SIZE - done, _add(base, done))
            if not chunk:
                raise ColumnStoreCorruptError("truncated column store window")
            end = _add(done, len(chunk))
            self.buffer[done:end] = chunk
            done = end
        self.base = base


@dataclass(frozen=True)
class StreamInfo:
    head: int
    tail: int
    encoded_length: int
    item_count: int


class _Backing:
    """One unbuffered, unlinked file and its one quota object."""

    def __init__(self, store, artifact, object_id, generation):
        self.store = store
        self.artifact = artifact
        self.object_id = object_id
        self.generation = generation
        self.file = None
        self.extent = 0
        self.identity = (0, 0)
        self.registered = False
        self.uncertain = False

    def change(self, new_extent, output_bytes=0, *, creating=False):
        return _Change(self, new_extent, output_bytes, creating)

    def close(self):
        if self.file is None:
            return
        file, self.file = self.file, None
        file.close()
        # Last close of our unlinked, unaliased file realizes the release.
        # Unknown/pending realization stays charged for host reconciliation.
        if self.store.quota_client is not None and self.registered and not self.uncertain:
            self.store.quota_client.release(
                quota.Amounts(self.extent, 1, 0, 1),
                object_id=self.object_id, generation=self.generation,
                old_extent=self.extent, new_extent=0,
                stage=self.store.stage, artifact=self.artifact,
                device=self.identity[0], inode=self.identity[1])
            self.registered = False


class _Change:
    """Reserve before mutation; commit cumulative realization; cancel remainder."""

    def __init__(self, backing, new_extent, output_bytes, creating):
        self.backing = backing
        self.old_extent = backing.extent
        self.new_extent = _size(new_extent)
        if self.new_extent < self.old_extent:
            raise ValueError("column store growth cannot shrink a file")
        self.output_limit = _size(output_bytes)
        self.output_realized = 0
        self.creating = creating
        self.ticket = None

    def __enter__(self):
        backing = self.backing
        client = backing.store.quota_client
        if client is not None:
            # A refusal escapes before either TemporaryFile or a growing write.
            self.ticket = client.reserve(
                quota.Amounts(self.new_extent - self.old_extent,
                             int(self.creating), self.output_limit, int(self.creating)),
                object_id=backing.object_id, generation=backing.generation,
                old_extent=self.old_extent, new_extent=self.new_extent,
                stage=backing.store.stage, artifact=backing.artifact,
                device=backing.identity[0], inode=backing.identity[1])
        backing.uncertain = True
        return self

    def produced(self, count):
        if self.ticket is None:
            return
        value = _add(self.output_realized, count)
        if value > self.output_limit:
            raise ValueError("column store wrote beyond its output reservation")
        self.output_realized = value

    def _settle(self):
        backing = self.backing
        if backing.file is None:
            if self.ticket is not None:
                self.ticket.cancel()
            backing.uncertain = False
            return
        metadata = os.fstat(backing.file.fileno())
        actual = _size(metadata.st_size)
        if not self.old_extent <= actual <= self.new_extent:
            raise ColumnStoreCorruptError("column store extent changed unexpectedly")
        backing.extent = actual
        backing.identity = (metadata.st_dev, metadata.st_ino)
        if self.ticket is not None:
            confirm = backing.store.confirm_backing
            if confirm is not None:
                # This is a host integration hook, not child self-attestation.
                confirm(backing.object_id, backing.generation, backing.file.fileno())
            self.ticket.commit(
                quota.Amounts(actual - self.old_extent, int(self.creating),
                             self.output_realized, int(self.creating)),
                actual, device=metadata.st_dev, inode=metadata.st_ino)
            self.ticket.cancel()
            backing.registered = True
        backing.uncertain = False

    def __exit__(self, exc_type, exc_value, traceback):
        if exc_type is None:
            self._settle()
        else:
            try:
                self._settle()
            except BaseException:
                # Preserve the original error, especially a typed refusal.
                # Do not cancel an extent whose realization is now unknown.
                self.backing.uncertain = True
        return False


class ColumnStore:
    """P+5 streams, two descriptors, four data windows and one index window.

    index_bytes is the logical 32*(P+5) byte representation. index_extent
    rounds it to a full window for scratch admission and on-disk allocation;
    padding and data-block tail slack never count as produced output.

    A supplied client also admits both descriptor tokens. object_ids, when
    supplied, are the host's (index, data) u64 identities; otherwise fresh
    opaque identities are generated. generation identifies this store attempt.
    Q0 requires the host to confirm backing inodes before COMMIT. A host may
    supply confirm_backing(object_id, generation, fd), or arrange confirmation
    through its independent creation helper. No confirmation is forged here.
    """

    TRADES = 0
    EQUITY = 1
    OPENS = 2
    CLOSES = 3
    EVENTS = 4
    NUMERIC_BASE = FIXED_STREAMS

    def __init__(self, numeric_columns, *, directory=None, quota_client=None,
                 stage=quota.Stage.ASSEMBLY, object_ids=None, generation=1,
                 confirm_backing=None):
        self.numeric_columns = _size(numeric_columns)
        self.stream_count = _add(self.numeric_columns, FIXED_STREAMS)
        self.index_bytes = _multiply(INDEX_ENTRY.size, self.stream_count)
        self.index_extent = _rounded(self.index_bytes)
        self.quota_client = quota_client
        self.stage = stage
        self.confirm_backing = confirm_backing
        if quota_client is None:
            object_ids = (0, 0)
        else:
            if object_ids is None:
                object_ids = (secrets.randbelow(NO_BLOCK) + 1,
                              secrets.randbelow(NO_BLOCK) + 1)
                while object_ids[0] == object_ids[1]:
                    object_ids = (object_ids[0], secrets.randbelow(NO_BLOCK) + 1)
            if (len(object_ids) != 2 or object_ids[0] == object_ids[1]
                    or any(type(value) is not int or not 0 < value <= NO_BLOCK
                           for value in (*object_ids, generation))):
                raise ValueError("column store requires distinct nonzero u64 object identities")
        self._index = _Backing(self, quota.Artifact.COLUMN_INDEX, object_ids[0], generation)
        self._data = _Backing(self, quota.Artifact.COLUMN_DATA, object_ids[1], generation)
        self._data_windows = [_Window() for _ in range(DATA_WINDOWS)]
        self._index_window = _Window()
        self._replacement = 0
        self._closed = self._failed = self._reading = False
        try:
            with self._index.change(self.index_extent, self.index_bytes, creating=True) as change:
                self._index.file = tempfile.TemporaryFile(mode="w+b", buffering=0, dir=directory)
                os.ftruncate(self.index_fd, self.index_extent)
                change.produced(self.index_bytes)
                self._index_window.buffer[:] = _EMPTY_ENTRY * (BLOCK_SIZE // INDEX_ENTRY.size)
                position = 0
                while position < self.index_bytes:
                    take = min(BLOCK_SIZE, self.index_bytes - position)
                    _pwrite_all(self.index_fd, memoryview(self._index_window.buffer)[:take], position)
                    position = _add(position, take)
            with self._data.change(0, creating=True):
                self._data.file = tempfile.TemporaryFile(mode="w+b", buffering=0, dir=directory)
        except BaseException:
            try:
                self.close()
            except BaseException:
                pass
            raise

    @property
    def data_fd(self):
        """Borrowed descriptor; callers must not close, duplicate or mutate it."""
        self._check_open()
        return self._data.file.fileno()

    @property
    def index_fd(self):
        """Borrowed descriptor, with the same ownership rules as data_fd."""
        self._check_open()
        return self._index.file.fileno()

    def _check_open(self):
        if self._closed:
            raise ValueError("column store is closed")

    def _check_usable(self):
        self._check_open()
        if self._failed:
            raise ValueError("column store has an incomplete append or corrupt stream")

    def _stream(self, stream):
        _size(stream)
        if stream >= self.stream_count:
            raise IndexError("column store stream is out of range")
        return stream

    def numeric_stream(self, column):
        _size(column)
        if column >= self.numeric_columns:
            raise IndexError("numeric column is out of range")
        return _add(FIXED_STREAMS, column)

    def _entry_offset(self, stream):
        return _multiply(self._stream(stream), INDEX_ENTRY.size)

    @staticmethod
    def _block_bound(offset, extent):
        if (type(offset) is not int or not 0 <= offset <= MAX_SIZE
                or offset % BLOCK_SIZE or extent < BLOCK_SIZE
                or offset > extent - BLOCK_SIZE):
            raise ColumnStoreCorruptError("column store block is out of bounds")

    def _info(self, stream):
        position = self._entry_offset(stream)
        base = position - position % BLOCK_SIZE
        if self._index_window.base != base:
            self._index_window.load(self.index_fd, base)
        info = StreamInfo(*INDEX_ENTRY.unpack_from(self._index_window.buffer, position - base))
        if info.encoded_length > MAX_SIZE or info.item_count > MAX_SIZE:
            raise ColumnStoreCorruptError("column store index counter exceeds 2**53-1")
        if not info.encoded_length:
            if info != StreamInfo(NO_BLOCK, NO_BLOCK, 0, 0):
                raise ColumnStoreCorruptError("invalid empty column store stream")
        else:
            if not 0 < info.item_count <= info.encoded_length:
                raise ColumnStoreCorruptError("invalid column store item count")
            self._block_bound(info.head, self._data.extent)
            self._block_bound(info.tail, self._data.extent)
        return info

    def stream_info(self, stream):
        """Return the on-disk entry, without retaining per-stream metadata."""
        self._check_usable()
        return self._info(stream)

    def _put_info(self, stream, info):
        position = self._entry_offset(stream)
        encoded = INDEX_ENTRY.pack(info.head, info.tail, info.encoded_length, info.item_count)
        _pwrite_all(self.index_fd, encoded, position)
        base = self._index_window.base
        if base is not None and base <= position < _add(base, BLOCK_SIZE):
            start = position - base
            self._index_window.buffer[start:_add(start, INDEX_ENTRY.size)] = encoded

    def _select_window(self):
        window = self._data_windows[self._replacement]
        self._replacement = (self._replacement + 1) % DATA_WINDOWS
        window.base = None
        return window

    def _data_window(self, offset):
        self._block_bound(offset, self._data.extent)
        for window in self._data_windows:
            if window.base == offset:
                return window
        window = self._select_window()
        window.load(self.data_fd, offset)
        return window

    def _patch_next(self, tail, new_block):
        encoded = NEXT_BLOCK.pack(new_block)
        _pwrite_all(self.data_fd, encoded, tail)
        for window in self._data_windows:
            if window.base == tail:
                window.buffer[:NEXT_BLOCK.size] = encoded

    def _append_piece(self, stream, info, piece, final_count, last_piece):
        window = None
        used = PAYLOAD_SIZE
        if info.tail != NO_BLOCK:
            window = self._data_window(info.tail)
            next_block, used, reserved = BLOCK_HEADER.unpack_from(window.buffer)
            if next_block != NO_BLOCK or not 0 < used <= PAYLOAD_SIZE or reserved:
                raise ColumnStoreCorruptError("invalid column store tail")
        new_block = used == PAYLOAD_SIZE
        take = min(len(piece), PAYLOAD_SIZE if new_block else PAYLOAD_SIZE - used)
        new_length = _add(info.encoded_length, take)
        count = final_count if last_piece and take == len(piece) else info.item_count
        if new_block:
            offset = self._data.extent
            extent = _add(offset, BLOCK_SIZE)
            _add(self.index_extent, extent)
            with self._data.change(extent, _add(BLOCK_HEADER.size, take)) as change:
                os.ftruncate(self.data_fd, extent)
                window = self._select_window()
                window.buffer[:] = b"\0" * BLOCK_SIZE
                BLOCK_HEADER.pack_into(window.buffer, 0, NO_BLOCK, take, 0)
                _pwrite_all(self.data_fd, memoryview(window.buffer)[:BLOCK_HEADER.size],
                            offset, change.produced)
                _pwrite_all(self.data_fd, piece[:take], _add(offset, BLOCK_HEADER.size),
                            change.produced)
                window.buffer[BLOCK_HEADER.size:_add(BLOCK_HEADER.size, take)] = piece[:take]
                window.base = offset
                # The initialized new block exists before its predecessor links it.
                if info.tail != NO_BLOCK:
                    self._patch_next(info.tail, offset)
                info = StreamInfo(offset if info.head == NO_BLOCK else info.head,
                                  offset, new_length, count)
                self._put_info(stream, info)
        else:
            offset = info.tail
            with self._data.change(self._data.extent, take) as change:
                start = _add(BLOCK_HEADER.size, used)
                _pwrite_all(self.data_fd, piece[:take], _add(offset, start), change.produced)
                window.buffer[start:_add(start, take)] = piece[:take]
                BLOCK_HEADER.pack_into(window.buffer, 0, NO_BLOCK, _add(used, take), 0)
                _pwrite_all(self.data_fd, memoryview(window.buffer)[:BLOCK_HEADER.size], offset)
                info = StreamInfo(info.head, info.tail, new_length, count)
                self._put_info(stream, info)
        return info, take

    def append_encoded(self, stream, data, *, item_count=1):
        """Append one encoded item or an internally comma-separated byte batch.

        Empty bytes with item_count=0 are a no-op. Exactly one comma separates
        successive nonempty batches. Input is borrowed, never copied wholesale.
        Any mutation failure poisons the store; no partial batch can be spliced.
        """
        self._check_usable()
        self._stream(stream)
        if self._reading:
            raise ValueError("column store is frozen for reading")
        count = _size(item_count)
        view = memoryview(data).cast("B")
        length = _size(len(view))
        if not length and not count:
            return
        if not length or not count:
            raise ValueError("encoded bytes and item_count must both be nonempty")
        if count > length:
            raise ValueError("item_count exceeds the encoded byte count")
        info = self._info(stream)
        final_count = _add(info.item_count, count)
        _add(info.encoded_length, _add(length, int(bool(info.item_count))))
        parts = (memoryview(b","), view) if info.item_count else (view,)
        try:
            for index, part in enumerate(parts):
                position = 0
                while position < len(part):
                    info, take = self._append_piece(
                        stream, info, part[position:], final_count, index == len(parts) - 1)
                    position = _add(position, take)
        except BaseException:
            self._failed = True
            raise

    def iter_bytes(self, stream):
        """Yield at most 65520 bytes at a time, walking only this stream's chain.

        No visited set or private reader window is allocated. The number of
        visited blocks cannot exceed the file's block count, including on a
        cycle. Index length, bounds, tail and termination are all checked.
        """
        self._check_usable()
        self._stream(stream)
        self._reading = True
        try:
            yield from self._read_stream(stream)
        except (ColumnStoreCorruptError, OSError, OverflowError):
            self._failed = True
            raise

    def _read_stream(self, stream):
        """Validate and walk a frozen stream through the shared windows."""
        # Re-read framing at each splice; cached tails must not hide corruption.
        self._index_window.base = None
        for window in self._data_windows:
            window.base = None
        index_size = _size(os.fstat(self.index_fd).st_size)
        extent = _size(os.fstat(self.data_fd).st_size)
        if index_size != self.index_extent or extent != self._data.extent or extent % BLOCK_SIZE:
            raise ColumnStoreCorruptError("column store file extent disagrees with allocation")
        info = self._info(stream)
        remaining = info.encoded_length
        offset = info.head
        visited = 0
        block_limit = extent // BLOCK_SIZE
        last = NO_BLOCK
        while offset != NO_BLOCK:
            self._check_usable()
            if visited >= block_limit:
                raise ColumnStoreCorruptError("column store cycle or excessive chain length")
            visited = _add(visited, 1)
            window = self._data_window(offset)
            next_block, used, reserved = BLOCK_HEADER.unpack_from(window.buffer)
            if not 0 < used <= PAYLOAD_SIZE or reserved:
                raise ColumnStoreCorruptError("invalid column store block header")
            if next_block != NO_BLOCK:
                self._block_bound(next_block, extent)
            if used > remaining:
                raise ColumnStoreCorruptError("column store chain exceeds indexed length")
            remaining -= used
            last = offset
            offset = next_block
            yield bytes(memoryview(window.buffer)[BLOCK_HEADER.size:_add(BLOCK_HEADER.size, used)])
        if remaining or last != info.tail:
            raise ColumnStoreCorruptError("column store chain disagrees with indexed length or tail")

    def copy_to(self, stream, destination, *, brackets=True):
        """Splice into a binary destination, optionally surrounding it with []."""
        self._check_usable()
        self._stream(stream)
        if brackets:
            self._copy_piece(destination, b"[")
        for piece in self.iter_bytes(stream):
            self._copy_piece(destination, piece)
        if brackets:
            self._copy_piece(destination, b"]")

    @staticmethod
    def _copy_piece(destination, piece):
        view = memoryview(piece)
        done = 0
        while done < len(view):
            written = destination.write(view[done:])
            if type(written) is not int or not 0 < written <= len(view) - done:
                raise OSError("short column store splice write")
            done = _add(done, written)

    def close(self):
        """Close both files; release known live storage, never produced output."""
        if self._closed:
            return
        self._closed = True
        failure = None
        for backing in (self._data, self._index):
            try:
                backing.close()
            except BaseException as error:
                if failure is None:
                    failure = error
        if failure is not None:
            raise failure

    def __enter__(self):
        self._check_usable()
        return self

    def __exit__(self, exc_type, exc_value, traceback):
        try:
            self.close()
        except BaseException:
            if exc_type is None:
                raise
        return False
