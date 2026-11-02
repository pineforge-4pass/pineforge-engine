"""Real-file round trips and real host-ledger admission for column storage."""
from __future__ import annotations

from contextlib import contextmanager
import errno
import importlib.util
import io
import json
import os
from pathlib import Path
import socket
import struct
import sys
import threading

import pytest

import quota_columns as columns


HERE = Path(__file__).resolve().parent
# Independent readers of the specified disk format, not aliases of the store.
DISK_BLOCK = 64 * 1024
DISK_ENTRY = struct.Struct("<QQQQ")
DISK_HEADER = struct.Struct("<QII")
DISK_PAYLOAD = DISK_BLOCK - DISK_HEADER.size
SENTINEL = (1 << 64) - 1
COUNTER_LIMIT = (1 << 53) - 1


def fd_entries():
    # listdir's own enumeration descriptor is gone before fstat filters it.
    entries = set()
    for name in os.listdir("/proc/self/fd"):
        descriptor = int(name)
        try:
            os.fstat(descriptor)
        except OSError as error:
            if error.errno == errno.EBADF:
                continue
            raise
        entries.add(descriptor)
    return entries


def encoded(value, *, ascii=False):
    return json.dumps(value, ensure_ascii=ascii, separators=(",", ":"),
                      allow_nan=False).encode("utf-8")


def index_geometry(numeric_columns):
    logical = DISK_ENTRY.size * (numeric_columns + 5)
    extent = ((logical + DISK_BLOCK - 1) // DISK_BLOCK) * DISK_BLOCK
    return logical, extent


def block_count(reference):
    return (len(reference) + DISK_PAYLOAD - 1) // DISK_PAYLOAD


def raw_entry(store, stream):
    data = os.pread(store.index_fd, DISK_ENTRY.size, stream * DISK_ENTRY.size)
    assert len(data) == DISK_ENTRY.size
    return DISK_ENTRY.unpack(data)


def write_entry(store, stream, entry):
    data = DISK_ENTRY.pack(*entry)
    assert os.pwrite(store.index_fd, data, stream * DISK_ENTRY.size) == len(data)


def assert_splice(store, stream, pieces, item_count):
    reference = b",".join(pieces)
    chunks = list(store.iter_bytes(stream))
    assert all(0 < len(chunk) <= DISK_PAYLOAD for chunk in chunks)
    assert b"".join(chunks) == reference
    destination = io.BytesIO()
    store.copy_to(stream, destination)
    assert destination.getvalue() == b"[" + reference + b"]"
    destination = io.BytesIO()
    store.copy_to(stream, destination, brackets=False)
    assert destination.getvalue() == reference
    info = store.stream_info(stream)
    assert info.encoded_length == len(reference)
    assert info.item_count == item_count
    assert raw_entry(store, stream) == (
        info.head, info.tail, len(reference), item_count)
    return reference


@pytest.fixture
def quota_host(monkeypatch):
    """Load Q0 with real inherited marker sockets and serve its real protocol."""
    marker_sockets = (
        *socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET),
        *socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET),
    )
    monkeypatch.setenv("PF_RUN_QUOTA_MODE", "managed")
    monkeypatch.setenv("PF_RUN_QUOTA_NATIVE_FD", str(marker_sockets[1].fileno()))
    monkeypatch.setenv("PF_RUN_QUOTA_PYTHON_FD", str(marker_sockets[3].fileno()))
    module_name = "_column_store_host_quota"
    spec = importlib.util.spec_from_file_location(module_name, HERE / "run_quota.py")
    q = importlib.util.module_from_spec(spec)
    monkeypatch.setitem(sys.modules, module_name, q)
    spec.loader.exec_module(q)

    @contextmanager
    def host(limits=(1 << 30, 8, 1 << 30, 16)):
        ledger = q.RunQuota(q.Amounts(*limits))
        server, endpoint = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        q.prepare_socket(server)
        actor = ledger.open_owner(pid=os.getpid(), uid=os.getuid(), gid=os.getgid(),
                                  generation=1, role=q.Role.WORKER, soft_nofile=64)
        errors = []

        def serve():
            try:
                q.serve(ledger, server, actor)
            except BaseException as error:
                errors.append(error)
                server.close()

        def confirm(object_id, generation, descriptor):
            return ledger.confirm_object(actor, object_id, generation, descriptor)

        thread = threading.Thread(target=serve, daemon=True)
        thread.start()
        client = None
        try:
            client = q.QuotaClient(endpoint)
            yield q, ledger, client, confirm
        finally:
            try:
                if client is not None:
                    refusal = ledger.refusal
                    try:
                        client.finish()
                    except q.QuotaError as error:
                        assert refusal is not None and error.snapshot == refusal
            finally:
                endpoint.close()
                thread.join(10)
                try:
                    assert not thread.is_alive(), "quota server did not stop after endpoint close"
                    assert not errors, errors
                finally:
                    server.close()
                    ledger.close()

    try:
        yield host
    finally:
        for channel in marker_sockets:
            channel.close()


@pytest.fixture(params=("unmanaged", "managed"))
def make_store(request, tmp_path, quota_host):
    @contextmanager
    def opened(numeric_columns, **kwargs):
        # Observe BEFORE construction, after the host's unrelated descriptors
        # are open. An implementation that adds per-stream fds fails here.
        before = fd_entries()
        with columns.ColumnStore(numeric_columns, directory=tmp_path, **kwargs) as store:
            after = fd_entries()
            assert len(after) - len(before) == 2
            assert after - before == {store.data_fd, store.index_fd}
            yield store
        assert fd_entries() == before

    @contextmanager
    def create(numeric_columns):
        if request.param == "unmanaged":
            with opened(numeric_columns) as store:
                yield store
        else:
            with quota_host() as (_, ledger, client, confirm):
                with opened(
                        numeric_columns, quota_client=client,
                        confirm_backing=confirm) as store:
                    yield store
                assert ledger.refusal is None
                assert all(usage.reserved == 0 for usage in ledger.snapshot())
    return create


def test_empty_streams_allocate_index_only(make_store):
    numeric_columns = 23
    with make_store(numeric_columns) as store:
        logical, extent = index_geometry(numeric_columns)
        assert store.index_bytes == logical
        assert os.fstat(store.index_fd).st_size == extent
        assert os.fstat(store.data_fd).st_size == 0
        assert os.fstat(store.index_fd).st_nlink == os.fstat(store.data_fd).st_nlink == 0
        for stream in range(numeric_columns + 5):
            store.append_encoded(stream, b"", item_count=0)
            assert raw_entry(store, stream) == (SENTINEL, SENTINEL, 0, 0)
        for stream in range(numeric_columns + 5):
            assert_splice(store, stream, [], 0)
        assert os.fstat(store.data_fd).st_size == 0


def test_one_stream_exceeds_the_entire_data_cache(make_store):
    pieces = [
        encoded("雪\\\n" * DISK_PAYLOAD),
        encoded({"tag": "closing", "time": 1712345678}),
        b"-0.0",
    ]
    reference = b",".join(pieces)
    with make_store(0) as store:
        windows = tuple(id(window.buffer) for window in store._data_windows)
        for piece in pieces:
            store.append_encoded(store.TRADES, piece)
        expected_blocks = block_count(reference)
        assert expected_blocks > len(windows)
        assert os.fstat(store.data_fd).st_size == expected_blocks * DISK_BLOCK
        assert_splice(store, store.TRADES, pieces, len(pieces))
        assert tuple(id(window.buffer) for window in store._data_windows) == windows
        for stream in (store.EQUITY, store.OPENS, store.CLOSES, store.EVENTS):
            assert_splice(store, stream, [], 0)


def test_many_interleaved_streams_use_exactly_two_descriptors(make_store):
    numeric_columns = 2000
    before = fd_entries()
    with make_store(numeric_columns) as store:
        # make_store checks the actual before/after construction delta.
        live = fd_entries()
        assert len(store._data_windows) == 4
        buffers = tuple(window.buffer for window in store._data_windows) + (
            store._index_window.buffer,)
        assert len({id(buffer) for buffer in buffers}) == 5
        assert [len(buffer) for buffer in buffers] == [DISK_BLOCK] * 5
        references = [[] for _ in range(numeric_columns + 5)]
        for turn in range(3):
            # Alternating direction revisits both recent and evicted tails.
            streams = range(numeric_columns + 5)
            if turn % 2:
                streams = reversed(streams)
            for stream in streams:
                piece = encoded([stream, turn])
                store.append_encoded(stream, piece)
                references[stream].append(piece)
        assert fd_entries() == live
        expected_blocks = sum(block_count(b",".join(pieces)) for pieces in references)
        assert os.fstat(store.data_fd).st_size == expected_blocks * DISK_BLOCK
        for stream, pieces in enumerate(references):
            assert_splice(store, stream, pieces, len(pieces))
        assert fd_entries() == live
        assert all(current is original for current, original in zip(
            (window.buffer for window in store._data_windows), buffers[:4]))
        assert store._index_window.buffer is buffers[-1]
    assert fd_entries() == before


def test_unicode_escaping_and_already_encoded_batches(make_store):
    trades = [{"name": '雪 "quote" \\\n\t\x00', "price": -0.0}, "🚀", None]
    equity = [[0, -0.0], [1, 1.2345678901234567], [2, None]]
    events = [{"text": "\u2028\u2029", "tag": "買い"}, {"text": "\\u96ea"}]
    pieces = {
        columns.ColumnStore.TRADES: [
            b",".join(encoded(value) for value in trades[:2]), encoded(trades[2])],
        columns.ColumnStore.EQUITY: [
            b",".join(encoded(value) for value in equity[:2]), encoded(equity[2])],
        columns.ColumnStore.EVENTS: [
            b",".join((encoded(events[0], ascii=True), encoded(events[1])))],
        columns.ColumnStore.OPENS: [b"1700000000000,null"],
        columns.ColumnStore.CLOSES: [b"1700000001000"],
    }
    counts = {columns.ColumnStore.TRADES: len(trades),
              columns.ColumnStore.EQUITY: len(equity),
              columns.ColumnStore.EVENTS: len(events),
              columns.ColumnStore.OPENS: 2, columns.ColumnStore.CLOSES: 1}
    with make_store(1) as store:
        for stream, batches in pieces.items():
            for index, piece in enumerate(batches):
                count = 2 if stream in (store.TRADES, store.EQUITY) and index == 0 else 1
                if stream in (store.OPENS, store.EVENTS):
                    count = 2
                store.append_encoded(stream, piece, item_count=count)
        stream = store.numeric_stream(0)
        store.append_encoded(stream, b"-0.0,null,1e-300", item_count=3)
        for stream, batches in pieces.items():
            assert_splice(store, stream, batches, counts[stream])
        assert_splice(store, store.numeric_stream(0), [b"-0.0,null,1e-300"], 3)


def test_batch_crosses_exact_payload_boundary(make_store):
    first = encoded("a" * (DISK_PAYLOAD - 2))
    batch = b",".join((first, b"null"))
    with make_store(0) as store:
        store.append_encoded(store.EQUITY, batch, item_count=2)
        store.append_encoded(store.EQUITY, b"false")
        reference = b",".join((batch, b"false"))
        head, tail, length, count = raw_entry(store, store.EQUITY)
        next_block, used, reserved = DISK_HEADER.unpack(
            os.pread(store.data_fd, DISK_HEADER.size, head))
        assert used == len(first) == DISK_PAYLOAD
        assert next_block == tail and not reserved
        assert os.pread(store.data_fd, 1, tail + DISK_HEADER.size) == b","
        assert length == len(reference) and count == 3
        assert os.fstat(store.data_fd).st_size == block_count(reference) * DISK_BLOCK
        assert_splice(store, store.EQUITY, [batch, b"false"], 3)


def test_evicted_tail_pointer_and_used_field_are_updated(make_store):
    numeric_columns = 13
    first = encoded("z" * (DISK_PAYLOAD - 2))
    with make_store(numeric_columns) as store:
        store.append_encoded(store.TRADES, first)
        old_head = raw_entry(store, store.TRADES)[0]
        references = [[first]]
        for stream in range(1, numeric_columns + 5):
            piece = encoded({"stream": stream})
            store.append_encoded(stream, piece)
            references.append([piece])
        assert old_head not in {window.base for window in store._data_windows}
        store.append_encoded(store.TRADES, b"7")
        references[store.TRADES].append(b"7")
        old_next, old_used, reserved = DISK_HEADER.unpack(
            os.pread(store.data_fd, DISK_HEADER.size, old_head))
        assert old_next == raw_entry(store, store.TRADES)[1]
        assert old_next != SENTINEL and old_used == len(first) and not reserved
        partial_tail = raw_entry(store, store.EQUITY)[1]
        assert partial_tail not in {window.base for window in store._data_windows}
        store.append_encoded(store.EQUITY, b"null,false", item_count=2)
        references[store.EQUITY].append(b"null,false")
        next_block, used, reserved = DISK_HEADER.unpack(
            os.pread(store.data_fd, DISK_HEADER.size, partial_tail))
        assert used == len(b",".join(references[store.EQUITY]))
        assert next_block == SENTINEL and not reserved
        expected_blocks = sum(block_count(b",".join(pieces)) for pieces in references)
        assert os.fstat(store.data_fd).st_size == expected_blocks * DISK_BLOCK
        for stream, pieces in enumerate(references):
            count = 3 if stream == store.EQUITY else len(pieces)
            assert_splice(store, stream, pieces, count)


def test_index_window_eviction_preserves_both_entries(make_store):
    numeric_columns = DISK_BLOCK // DISK_ENTRY.size + 19
    with make_store(numeric_columns) as store:
        streams = (store.TRADES, store.numeric_stream(numeric_columns - 1))
        references = {stream: [] for stream in streams}
        index_buffer = store._index_window.buffer
        for turn in range(5):
            for stream in streams:
                piece = encoded({"stream": stream, "turn": turn})
                store.append_encoded(stream, piece)
                references[stream].append(piece)
                assert raw_entry(store, stream)[2:] == (
                    len(b",".join(references[stream])), len(references[stream]))
        assert store._index_window.buffer is index_buffer
        assert os.fstat(store.index_fd).st_size == index_geometry(numeric_columns)[1]
        for stream in streams:
            assert_splice(store, stream, references[stream], len(references[stream]))
        assert_splice(store, store.EVENTS, [], 0)


def test_first_over_limit_block_is_refused_before_disk_growth(quota_host, tmp_path):
    numeric_columns = 0
    logical, index_extent = index_geometry(numeric_columns)
    first = encoded("x" * (DISK_PAYLOAD - 2))
    admitted_blocks = block_count(first)
    limit = index_extent + admitted_blocks * DISK_BLOCK
    with quota_host((limit, 2, 1 << 30, 2)) as (q, ledger, client, confirm):
        with columns.ColumnStore(
                numeric_columns, directory=tmp_path, quota_client=client,
                confirm_backing=confirm) as store:
            store.append_encoded(store.TRADES, first)
            sizes = (os.fstat(store.data_fd).st_size, os.fstat(store.index_fd).st_size)
            data_before = os.pread(store.data_fd, sizes[0], 0)
            index_before = os.pread(store.index_fd, sizes[1], 0)
            usage_before = ledger.snapshot()
            with pytest.raises(q.QuotaError) as caught:
                store.append_encoded(store.TRADES, b"0")
            error = caught.value
            assert error.snapshot is client.refusal
            assert error.snapshot == ledger.refusal
            assert dict(error.arguments) == {
                "stage": "assembly", "resource": "scratch_bytes",
                "artifact": "column_data", "used": limit,
                "requested": DISK_BLOCK, "limit": limit, "peak": limit,
            }
            assert ledger.snapshot() == usage_before
            assert (os.fstat(store.data_fd).st_size, os.fstat(store.index_fd).st_size) == sizes
            assert os.pread(store.data_fd, sizes[0], 0) == data_before
            assert os.pread(store.index_fd, sizes[1], 0) == index_before
            with pytest.raises(ValueError, match="incomplete append"):
                b"".join(store.iter_bytes(store.TRADES))
        usage = ledger.snapshot()
        assert usage[q.Resource.SCRATCH_BYTES - 1].used == 0
        assert usage[q.Resource.SCRATCH_FILES - 1].used == 0
        assert usage[q.Resource.DESCRIPTORS - 1].used == 0
        assert usage[q.Resource.OUTPUT_BYTES - 1].committed == (
            logical + admitted_blocks * DISK_HEADER.size + len(first))


def test_index_reservation_precedes_any_backing_file(quota_host, tmp_path):
    numeric_columns = 2000
    _, extent = index_geometry(numeric_columns)
    with quota_host((extent - 1, 2, 1 << 30, 2)) as (q, ledger, client, confirm):
        before = fd_entries()
        with pytest.raises(q.QuotaError) as caught:
            columns.ColumnStore(numeric_columns, directory=tmp_path, quota_client=client,
                                confirm_backing=confirm)
        assert fd_entries() == before
        assert list(tmp_path.iterdir()) == []
        assert caught.value.snapshot.artifact == q.Artifact.COLUMN_INDEX
        assert caught.value.snapshot.requested == extent
        assert all(usage.used == 0 for usage in ledger.snapshot())


@pytest.mark.parametrize("resource", ("scratch_files", "descriptors"))
def test_second_backing_refusal_closes_the_first_file(quota_host, tmp_path, resource):
    numeric_columns = 0
    logical, extent = index_geometry(numeric_columns)
    limits = (extent, 1 if resource == "scratch_files" else 2,
              logical, 1 if resource == "descriptors" else 2)
    with quota_host(limits) as (q, ledger, client, confirm):
        before = fd_entries()
        with pytest.raises(q.QuotaError) as caught:
            columns.ColumnStore(numeric_columns, directory=tmp_path, quota_client=client,
                                confirm_backing=confirm)
        assert fd_entries() == before
        assert list(tmp_path.iterdir()) == []
        refusal = caught.value.snapshot
        assert refusal == ledger.refusal
        assert refusal.artifact == q.Artifact.COLUMN_DATA
        assert (refusal.used, refusal.requested, refusal.limit) == (1, 1, 1)
        if resource == "scratch_files":
            assert refusal.resource == q.Resource.SCRATCH_FILES
            assert caught.value.code == "run_resource_quota_exceeded"
        else:
            assert refusal.resource == q.Resource.DESCRIPTORS
            assert caught.value.code == "run_descriptor_limit_exceeded"
        usage = ledger.snapshot()
        assert usage[q.Resource.SCRATCH_BYTES - 1].used == 0
        assert usage[q.Resource.SCRATCH_FILES - 1].used == 0
        assert usage[q.Resource.DESCRIPTORS - 1].used == 0
        assert usage[q.Resource.OUTPUT_BYTES - 1].committed == logical


def test_output_accounting_excludes_slack_and_never_refunds_output(quota_host, tmp_path):
    numeric_columns = 9
    pieces = [[encoded([stream, turn]) for turn in range(3)]
              for stream in range(numeric_columns + 5)]
    logical, index_extent = index_geometry(numeric_columns)
    blocks = sum(block_count(b",".join(items)) for items in pieces)
    payload_bytes = sum(len(b",".join(items)) for items in pieces)
    expected_output = logical + blocks * DISK_HEADER.size + payload_bytes
    scratch_limit = index_extent + blocks * DISK_BLOCK
    with quota_host((scratch_limit, 2, expected_output, 2)) as (q, ledger, client, confirm):
        with columns.ColumnStore(
                numeric_columns, directory=tmp_path, quota_client=client,
                confirm_backing=confirm) as store:
            for turn in range(3):
                for stream, items in enumerate(pieces):
                    store.append_encoded(stream, items[turn])
            usage = ledger.snapshot()
            assert usage[q.Resource.SCRATCH_BYTES - 1].committed == (
                os.fstat(store.index_fd).st_size + os.fstat(store.data_fd).st_size)
            assert usage[q.Resource.SCRATCH_FILES - 1].committed == 2
            assert usage[q.Resource.DESCRIPTORS - 1].committed == 2
            assert usage[q.Resource.OUTPUT_BYTES - 1].committed == expected_output
            assert all(item.reserved == 0 for item in usage)
            for stream, items in enumerate(pieces):
                assert_splice(store, stream, items, len(items))
            assert ledger.snapshot() == usage  # Splicing is not a new stored representation.
        usage = ledger.snapshot()
        assert [usage[resource - 1].committed for resource in (
            q.Resource.SCRATCH_BYTES, q.Resource.SCRATCH_FILES, q.Resource.DESCRIPTORS)] == [0, 0, 0]
        assert usage[q.Resource.OUTPUT_BYTES - 1].committed == expected_output
        assert all(item.reserved == 0 for item in usage)


def test_output_refusal_does_not_patch_an_existing_tail(quota_host, tmp_path):
    logical, index_extent = index_geometry(0)
    first = b"0"
    expected_output = logical + DISK_HEADER.size * block_count(first) + len(first)
    limits = (index_extent + DISK_BLOCK * block_count(first), 2, expected_output, 2)
    with quota_host(limits) as (q, ledger, client, confirm):
        with columns.ColumnStore(
                0, directory=tmp_path, quota_client=client,
                confirm_backing=confirm) as store:
            store.append_encoded(store.EVENTS, first)
            size = os.fstat(store.data_fd).st_size
            data_before = os.pread(store.data_fd, size, 0)
            entry_before = raw_entry(store, store.EVENTS)
            usage_before = ledger.snapshot()
            with pytest.raises(q.QuotaError) as caught:
                store.append_encoded(store.EVENTS, b"1")
            refusal = caught.value.snapshot
            assert refusal is client.refusal
            assert refusal.resource == q.Resource.OUTPUT_BYTES
            assert refusal.artifact == q.Artifact.COLUMN_DATA
            assert (refusal.used, refusal.requested, refusal.limit) == (
                expected_output, len(b","), expected_output)
            assert ledger.snapshot() == usage_before
            assert os.fstat(store.data_fd).st_size == size
            assert os.pread(store.data_fd, size, 0) == data_before
            assert raw_entry(store, store.EVENTS) == entry_before
            destination = io.BytesIO()
            with pytest.raises(ValueError, match="incomplete append"):
                store.copy_to(store.EVENTS, destination)
            assert destination.getvalue() == b""


@pytest.mark.parametrize("bad_pointer", ("past_eof", "unaligned"))
def test_corrupt_next_pointer_raises(make_store, bad_pointer):
    piece = encoded("v" * DISK_PAYLOAD)
    with make_store(0) as store:
        store.append_encoded(store.TRADES, piece)
        head = raw_entry(store, store.TRADES)[0]
        pointer = os.fstat(store.data_fd).st_size if bad_pointer == "past_eof" else head + 1
        assert os.pwrite(store.data_fd, struct.pack("<Q", pointer), head) == struct.calcsize("<Q")
        with pytest.raises(columns.ColumnStoreCorruptError):
            list(store.iter_bytes(store.TRADES))


def test_cycle_is_bounded_by_actual_file_block_count(make_store):
    piece = encoded("cycle" * DISK_PAYLOAD)
    with make_store(0) as store:
        store.append_encoded(store.TRADES, piece)
        head, tail, _, count = raw_entry(store, store.TRADES)
        file_blocks = os.fstat(store.data_fd).st_size // DISK_BLOCK
        next_block, used, reserved = DISK_HEADER.unpack(
            os.pread(store.data_fd, DISK_HEADER.size, head))
        assert next_block != SENTINEL and not reserved
        assert os.pwrite(store.data_fd, struct.pack("<Q", head), head) == struct.calcsize("<Q")
        # A long advertised length forces the block-count guard itself to fire.
        write_entry(store, store.TRADES, (head, tail, (file_blocks + 1) * used, count))
        emitted = 0
        with pytest.raises(columns.ColumnStoreCorruptError, match="cycle"):
            for _ in store.iter_bytes(store.TRADES):
                emitted += 1
        assert emitted <= file_blocks


@pytest.mark.parametrize("corruption", ("used", "reserved", "short_length", "long_length",
                                      "wrong_tail", "truncated_data", "truncated_index"))
def test_corrupt_framing_or_length_raises(make_store, corruption):
    piece = encoded("framing" * DISK_PAYLOAD)
    with make_store(0) as store:
        store.append_encoded(store.EQUITY, piece)
        head, tail, length, count = raw_entry(store, store.EQUITY)
        if corruption in ("used", "reserved"):
            next_block, used, reserved = DISK_HEADER.unpack(
                os.pread(store.data_fd, DISK_HEADER.size, head))
            if corruption == "used":
                used = DISK_PAYLOAD + 1
            else:
                reserved = 1
            replacement = DISK_HEADER.pack(next_block, used, reserved)
            assert os.pwrite(store.data_fd, replacement, head) == len(replacement)
        elif corruption == "short_length":
            write_entry(store, store.EQUITY, (head, tail, length - 1, count))
        elif corruption == "long_length":
            write_entry(store, store.EQUITY, (head, tail, length + 1, count))
        elif corruption == "wrong_tail":
            write_entry(store, store.EQUITY, (head, head, length, count))
        elif corruption == "truncated_data":
            os.ftruncate(store.data_fd, os.fstat(store.data_fd).st_size - 1)
        else:
            os.ftruncate(store.index_fd, os.fstat(store.index_fd).st_size - 1)
        with pytest.raises(columns.ColumnStoreCorruptError):
            list(store.iter_bytes(store.EQUITY))


@pytest.mark.parametrize("numeric_columns", (
    -1, True, COUNTER_LIMIT + 1, COUNTER_LIMIT,
    COUNTER_LIMIT // DISK_ENTRY.size,
    COUNTER_LIMIT // DISK_ENTRY.size - 5,
))
def test_invalid_or_overflowing_index_geometry_creates_nothing(tmp_path, numeric_columns):
    before = fd_entries()
    with pytest.raises((ValueError, OverflowError)):
        columns.ColumnStore(numeric_columns, directory=tmp_path)
    assert fd_entries() == before
    assert list(tmp_path.iterdir()) == []


def test_first_read_freezes_appends_and_close_releases_descriptors(make_store):
    before = fd_entries()
    with make_store(1) as store:
        store.append_encoded(store.numeric_stream(0), b"1")
        assert_splice(store, store.numeric_stream(0), [b"1"], 1)
        with pytest.raises(ValueError, match="frozen"):
            store.append_encoded(store.TRADES, b"0")
    assert fd_entries() == before
    store.close()
    with pytest.raises(ValueError, match="closed"):
        list(store.iter_bytes(store.TRADES))
