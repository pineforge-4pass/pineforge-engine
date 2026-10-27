#pragma once

// Included within pineforge::native_order::native_order_v7 by native_order.cpp,
// after its file-local archive helpers and WorkingRequestCore::JournalStore.
// The include context preserves the existing helpers' linkage and lookup.

namespace {

// Compare the definition prefix of a just-decoded book record by walking the
// very same field lists as SaveAr/LoadAr. No output buffer or hash is involved:
// strings, vector lengths, tags, optional flags and every leaf must match.
// Raw leaves include padding, so a padding difference conservatively misses.
struct DefinitionMatchAr {
    static constexpr bool saving = true;
    const unsigned char* at;
    std::size_t left;
    bool matched = true;

    void raw(const void* value, std::size_t length) noexcept {
        if (!matched) return;
        if (length > left || (length != 0 && std::memcmp(at, value, length) != 0)) {
            matched = false;
            return;
        }
        at += length;
        left -= length;
    }
    template <class T>
    void operator()(const T& item) {
        if (matched) archive_value(*this, const_cast<T&>(item));
    }
};

// One spilled book row, filed under its priority: where its serialized
// LiveRequest sits in the blob file. state 0: the row is spilled; 1: it came
// back (or ended) and the entry is dead weight until the next compaction.
struct BookSlot {
    uint64_t offset = 0;
    uint64_t length = 0;
    uint64_t incarnation = 0;
    uint64_t state = 0;
};

// The priority of a spilled row whose handle was re-priced, filed under its
// incarnation (a row that was never re-priced stands at its incarnation).
struct IncarnationSlot {
    uint64_t priority = 0;
    uint64_t state = 0;
};

// Table keys are big endian, so that byte order is numeric order.
std::string book_key(uint64_t value) {
    std::string key(8, '\0');
    for (std::size_t i = 0; i != 8; ++i) {
        key[i] = static_cast<char>((value >> (56 - 8 * i)) & 0xffu);
    }
    return key;
}

uint64_t book_key_value(const std::string& key) {
    uint64_t value = 0;
    for (const char byte : key) value = (value << 8) | static_cast<unsigned char>(byte);
    return value;
}

}  // namespace

// The working book's spilled rows: an ordered table keyed by priority (the
// repository's DiskIdTable, an AVL tree in a scratch file read by position, no
// resident nodes) whose values say where each row's serialized LiveRequest
// sits in an append-only blob file, plus a second table that maps the
// incarnation of a re-priced row to the priority it stands at. Entries of rows
// that came back are marked dead and skipped; the tables and the blob file are
// rebuilt from the live entries once the dead outweigh them, so the files stay
// within a constant factor of the rows they hold. A small ring keeps copies of
// the last spilled rows a reader asked for.
struct WorkingRequestCore::BookSpill {
    static constexpr std::size_t kSide = 8;
    detail::DiskIdTable<BookSlot> rows;
    detail::DiskIdTable<IncarnationSlot> links;
    ExtFile blobs;
    std::string directory;
    uint64_t alive = 0;     // rows spilled and not yet back: the book's spilled count
    uint64_t gone = 0;      // dead entries in rows
    uint64_t garbage = 0;   // blob bytes no live entry reads
    // How many times compact rebuilt the tables: a table offset taken before
    // a rebuild names nothing after it (hold_scanned). Storage metadata, like
    // the offsets themselves, so nothing folds it.
    uint64_t generation = 0;
    mutable std::array<std::shared_ptr<LiveRequest>, kSide> side{};
    mutable std::size_t side_next = 0;
    // A read failure a reader that cannot throw handed over
    // (carry_book_read_failure); every later read of these rows throws it.
    std::exception_ptr failure;
    // book_lookup_reads: rows read back and table steps walked by lookups.
    mutable uint64_t lookup_rows = 0;
    mutable uint64_t lookup_steps = 0;

    // Throws the carried failure, if any: an I/O failure as the typed
    // scratch failure (scratch_io_failed), anything else as it was thrown.
    void check() const {
        if (!failure) return;
        try {
            std::rethrow_exception(failure);
        } catch (const detail::AuxiliaryScratchIoError&) {
            throw;
        } catch (const std::system_error& error) {
            throw detail::AuxiliaryScratchIoError(error);
        }
    }

    void set_directory(const std::string& value) {
        directory = value;
        rows.set_directory(value);
        links.set_directory(value);
        blobs.set_directory(value);
    }
    // Writes `row` out and files it under its priority. The caller drops the
    // resident row once this has returned.
    void spill(const LiveRequest& row) {
        ByteWriter out;
        SaveAr ar{out};
        ar(row);
        const uint64_t priority = row.priority();
        const uint64_t incarnation = row.handle().incarnation;
        BookSlot slot;
        slot.offset = blobs.append(out.data(), out.size());
        slot.length = out.size();
        slot.incarnation = incarnation;
        if (priority != incarnation) {
            IncarnationSlot link;
            link.priority = priority;
            const auto linked = links.insert(book_key(incarnation), link);
            if (!linked.second) links.set(linked.first, link);
        }
        const auto placed = rows.insert(book_key(priority), slot);
        if (!placed.second) {
            // The row was spilled before and came back: its entry is revived.
            const BookSlot old = rows.get(placed.first);
            rows.set(placed.first, slot);
            if (old.state != 0) --gone;
            else garbage += old.length;
        }
    }
    LiveRequest read(const WorkingRequestCore& core, const BookSlot& slot) const {
        std::vector<unsigned char> bytes(static_cast<std::size_t>(slot.length));
        blobs.read(slot.offset, bytes.data(), bytes.size());
        ByteReader in(bytes.data(), bytes.size());
        LoadAr ar{in};
        LiveRequest row;
        ar(row);
        if (!row.definition || !core.journal_ || core.journal_->codec) return row;
        const JournalStore& journal = *core.journal_;
        // Restrict the existing ordinal bisection to resident events: no cold
        // journal reads or new index. One O(log kMaxResident) lookup per book
        // decode, then O(definition bytes) equality, never a per-child scan.
        const auto position = journal.find_ordinal(row.birth().acceptance_ordinal, journal.first);
        if (!position) return row;
        const CommandEvent& event = journal.resident_at(*position);
        const DefinitionRef* resident = nullptr;
        if (const auto* accepted = std::get_if<AcceptedEvent>(&event)) {
            resident = &accepted->definition;
        } else if (const auto* replaced = std::get_if<ReplacedEvent>(&event)) {
            // A kept-handle re-price has the same handle but a new birth and
            // priority: only the successor can describe this book version.
            resident = &replaced->successor_definition;
        }
        if (!resident || !*resident || (*resident)->handle != row.handle()
            || (*resident)->birth != row.birth()
            || (*resident)->priority != row.definition->priority) return row;
        // LiveRequest's first field is its definition. Compare that entire
        // serialized prefix, including request (an owner's external openings
        // among it, handle by handle), predecessor, origin, root and
        // kept_binding; the decoded owner survives every mismatch unchanged.
        DefinitionMatchAr match{bytes.data(), bytes.size()};
        match(*resident);
        if (match.matched) row.definition = *resident;
        return row;
    }
    // Replaces the serialized row of the live entry at `at`, keeping its
    // priority. Bytes of the same length (a refreshed allowance usually is)
    // overwrite the old ones in place; anything else is appended and the
    // entry points at it, the old bytes becoming garbage.
    void rewrite(uint64_t at, BookSlot slot, const LiveRequest& row) {
        ByteWriter out;
        SaveAr ar{out};
        ar(row);
        if (out.size() == slot.length) {
            blobs.write(slot.offset, out.data(), out.size());
            return;
        }
        const uint64_t old_length = slot.length;
        slot.offset = blobs.append(out.data(), out.size());
        slot.length = out.size();
        rows.set(at, slot);
        garbage += old_length;
    }
    // The spilled row filed under `priority`.
    bool find(uint64_t priority, BookSlot* slot, uint64_t* at) const {
        const auto found = rows.find(book_key(priority));
        if (!found) return false;
        const BookSlot value = rows.get(*found);
        if (value.state != 0) return false;
        *slot = value;
        *at = *found;
        return true;
    }
    uint64_t priority_of(uint64_t incarnation) const {
        const auto found = links.find(book_key(incarnation));
        if (found) {
            const IncarnationSlot link = links.get(*found);
            if (link.state == 0) return link.priority;
        }
        return incarnation;
    }
    // The entry at `at` stops being a spilled row: it came back, or it ended.
    // Two steps, so the caller can make its own change between them. The row
    // entry is the one that publishes: retire_row writes first and may throw
    // while the caller has changed nothing; retired then counts the dead entry
    // and tombstones the link of a re-priced row. A link left standing by a
    // failed write is harmless: a lookup through it names a priority whose row
    // entry is dead, and BookSpill::find answers no row.
    void retire_row(uint64_t at, BookSlot slot) {
        slot.state = 1;
        rows.set(at, slot);
    }
    void retired(const BookSlot& slot, uint64_t priority) noexcept {
        garbage += slot.length;
        ++gone;
        if (priority == slot.incarnation) return;
        try {
            const auto found = links.find(book_key(slot.incarnation));
            if (found) {
                IncarnationSlot link = links.get(*found);
                link.state = 1;
                links.set(*found, link);
            }
        } catch (...) {
        }
    }
    uint64_t alive_from(uint64_t at) const {
        while (at != 0 && rows.get(at).state != 0) at = rows.next_sorted(at);
        return at;
    }
    uint64_t first_alive() const { return alive_from(rows.first_sorted()); }
    uint64_t next_alive(uint64_t at) const { return alive_from(rows.next_sorted(at)); }
    uint64_t priority_at(uint64_t at) const { return book_key_value(rows.key(at)); }
    // Keeps a copy of a spilled row alive for a reader; the last kSide copies
    // stay valid. keep hands the copy to a reader that holds it itself (the
    // ring and the reader share it, so it lives until both let go); stash
    // answers a bare pointer.
    std::shared_ptr<LiveRequest> keep(LiveRequest row) const {
        auto kept = std::make_shared<LiveRequest>(std::move(row));
        side[side_next] = kept;
        side_next = (side_next + 1) % side.size();
        return kept;
    }
    const LiveRequest* stash(LiveRequest row) const { return keep(std::move(row)).get(); }
    bool worth_compacting() const {
        return gone > 2 * alive + 64 || garbage > blobs.size() / 2 + (uint64_t{1} << 20);
    }
    // Rebuilds both tables and the blob file from the entries that still
    // stand. Table offsets taken before are no longer valid.
    void compact() {
        detail::DiskIdTable<BookSlot> fresh_rows;
        detail::DiskIdTable<IncarnationSlot> fresh_links;
        ExtFile fresh_blobs;
        if (!directory.empty()) {
            fresh_rows.set_directory(directory);
            fresh_links.set_directory(directory);
            fresh_blobs.set_directory(directory);
        }
        for (uint64_t at = first_alive(); at != 0; at = next_alive(at)) {
            BookSlot slot = rows.get(at);
            std::vector<unsigned char> bytes(static_cast<std::size_t>(slot.length));
            blobs.read(slot.offset, bytes.data(), bytes.size());
            slot.offset = fresh_blobs.append(bytes.data(), bytes.size());
            const std::string key = rows.key(at);
            const uint64_t priority = book_key_value(key);
            fresh_rows.insert(key, slot);
            if (priority != slot.incarnation) {
                IncarnationSlot link;
                link.priority = priority;
                fresh_links.insert(book_key(slot.incarnation), link);
            }
        }
        rows = std::move(fresh_rows);
        links = std::move(fresh_links);
        blobs = std::move(fresh_blobs);
        gone = 0;
        garbage = 0;
        ++generation;
    }
};
