#include <pineforge/source/pine_adapter.hpp>
#include "../src/source/exit_scratch_versions_production.hpp"

#include <array>
#include <cstddef>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {
using namespace pineforge;
namespace pd = pineforge::detail;
namespace sd = pineforge::source::detail;
using Status = pd::ScratchStatus;
using Handle = pd::ScratchHandle;
using Field = pd::ScratchField;

template<class T, class U>
void need(const char* field, const T& actual, const U& expected) {
    if (actual == expected) return;
    std::ostringstream message;
    message << field << " expected=" << expected << " actual=" << actual;
    throw std::runtime_error(message.str());
}

void ready(const char* operation, Status actual) {
    need(operation, static_cast<unsigned>(actual), static_cast<unsigned>(Status::ok));
}

std::string stored(const pd::CommandScratchStore& store, Handle handle, Field field) {
    pd::ScratchRecordInfo info{};
    ready("locate version", store.point(handle, info));
    need("located handle", info.handle, handle);
    const auto size = field == Field::key ? info.key_bytes : info.value_bytes;
    if (!size) throw std::runtime_error("stored field length expected=positive actual=0");
    std::string bytes(static_cast<std::size_t>(size), '\0');
    ready("read stored bytes", store.read(handle, field, 0,
        {reinterpret_cast<std::byte*>(bytes.data()), bytes.size()}));
    return bytes;
}

struct Reader {
    const sd::ExitScratchVersions& versions;
    bool pending;

    source::PlacementSnapshot read(Handle handle) const {
        if (!pending) return versions.placement(handle);
        native_order::Request request;
        source::PlacementSnapshot snapshot;
        std::string key;
        versions.pending(handle, request, snapshot, key);
        need("pending request", request.label, "parent-973");
        need("pending key", key, "key-973");
        return snapshot;
    }

    void expect_error(Handle handle, Status expected, const char* name,
                      const char* diagnostic = nullptr) const {
        std::string actual = "returned normally";
        try {
            (void)read(handle);
        } catch (const sd::ExitScratchError& e) {
            if (e.status() == expected) {
                if (diagnostic) {
                    need("factory diagnostic", std::string(e.what()), diagnostic);
                    need("run failure code", static_cast<int>(classify_run_failure(e).code),
                         static_cast<int>(RunFailureCode::engine_invariant));
                }
                return;
            }
            actual = "ExitScratchError(status="
                + std::to_string(static_cast<unsigned>(e.status())) + ")";
        } catch (const std::invalid_argument& e) {
            actual = std::string("std::invalid_argument: ") + e.what();
        } catch (const std::exception& e) {
            actual = std::string("other exception: ") + e.what();
        }
        throw std::runtime_error(std::string("expected=ExitScratchError(") + name
                                 + ") actual=" + actual);
    }
};

void check_valid(const Reader& reader, Handle handle, const OrderBirth& expected) {
    const auto snapshot = reader.read(handle);
    need("placement id", snapshot.source_id, "exit-973");
    const auto& birth = snapshot.birth;
    need("cause", static_cast<int>(birth.cause()), static_cast<int>(expected.cause()));
    need("bar", birth.bar(), expected.bar());
    need("timestamp", birth.timestamp(), expected.timestamp());
    need("domain", static_cast<int>(birth.cursor().domain()),
         static_cast<int>(expected.cursor().domain()));
    need("position", static_cast<int>(birth.cursor().position()),
         static_cast<int>(expected.cursor().position()));
    need("index", birth.cursor().index(), expected.cursor().index());
    need("count", birth.cursor().count(), expected.cursor().count());
    need("price", birth.cursor_price(), expected.cursor_price());
    need("first fill", birth.first_fill(), expected.first_fill());
    need("last fill", birth.last_fill(), expected.last_fill());
    need("evaluation ordinal", birth.evaluation_ordinal(), expected.evaluation_ordinal());
}

struct Mutation {
    const char* name;
    OrderBirth witness; // valid birth differing from the original in only this field
    unsigned char original, alternate, damaged;
    const char* diagnostic;
};

bool check_reader(bool pending, bool segment) {
    const std::string prefix = std::string(pending ? "pending/" : "placement/")
        + (segment ? "segment/" : "point/");
    std::string row = prefix + "setup";
    try {
        std::array<std::byte, 128 * 512> space{};
        pd::FixedMemoryScratchBacking backing({space.data(), space.size()}, 512);
        pd::CommandScratchStore store({2, 512, 1, 1, sd::exit_scratch_key_order(),
                                       pd::ScratchOpenMode::create_empty}, &backing);
        ready("open store", store.status());
        sd::ExitScratchVersionsProduction versions(store);
        const Reader reader{versions, pending};
        need("empty store record count", store.record_count(), 0u);
        reader.expect_error(0, Status::not_found, "not_found");
        reader.expect_error(1, Status::not_found, "not_found");
        native_order::Request request;
        request.label = "parent-973";
        const auto wrong_kind = versions.request(1, request);
        need("request-only version", versions.request(wrong_kind).label, request.label);
        reader.expect_error(wrong_kind, Status::programming_error, "programming_error");

        const auto write = [&](const OrderBirth& birth) {
            source::PlacementSnapshot snapshot;
            snapshot.source_id = "exit-973";
            snapshot.birth = birth;
            return pending ? versions.pending(1, request, snapshot, "key-973")
                           : versions.placement(1, snapshot);
        };
        const auto cursor = segment ? BirthCursor::segment : BirthCursor::point;
        const auto fill = [&](BirthCursorDomain domain, int index, int count, unsigned ordinal) {
            return OrderBirth::fill_evaluation(2, 120000, cursor(domain, index, count),
                                               100, 7, 7, ordinal);
        };
        constexpr auto ticks = BirthCursorDomain::MagnifierTicks;
        constexpr auto path = BirthCursorDomain::HistoricalPath;
        const auto birth = fill(ticks, 1, 4, 1);
        const auto handle = write(birth);
        check_valid(reader, handle, birth);
        const auto key = stored(store, handle, Field::key);
        const auto original = stored(store, handle, Field::value);
        constexpr auto cursor_error = "invalid order-birth dispatch cursor";
        const Mutation cases[] = {
            {"ordinal=0", fill(ticks, 1, 4, 2), 1, 2, 0, "invalid order-birth fill evaluation"},
            {"domain=None", fill(path, 1, 4, 1), static_cast<unsigned char>(ticks),
             static_cast<unsigned char>(path), static_cast<unsigned char>(BirthCursorDomain::None),
             cursor_error},
            {segment ? "index=3" : "index=4", fill(ticks, 2, 4, 1), 1, 2,
             static_cast<unsigned char>(segment ? 3 : 4), cursor_error},
            {"count=0", fill(ticks, 1, 5, 1), 4, 5, 0, cursor_error},
        };
        bool passed = true;
        for (const auto& fault : cases) {
            row = prefix + fault.name;
            const auto witness = write(fault.witness);
            need("distinct witness handle", witness != handle, true);
            check_valid(reader, witness, fault.witness);
            const auto alternate = stored(store, witness, Field::value);
            need("witness length", alternate.size(), original.size());
            std::size_t offset = 0, differences = 0;
            for (std::size_t i = 0; i < original.size(); ++i) {
                if (original[i] != alternate[i]) { offset = i; ++differences; }
            }
            need("differing field bytes", differences, std::size_t{1});
            need("original byte", +static_cast<unsigned char>(original[offset]), +fault.original);
            need("witness byte", +static_cast<unsigned char>(alternate[offset]), +fault.alternate);
            need("damage changes original byte", fault.damaged != fault.original, true);

            auto damaged = original;
            damaged[offset] = static_cast<char>(fault.damaged);
            ready("begin replacement", store.begin());
            Handle replaced = 0;
            ready("replace payload", store.put(sd::exit_scratch_text(key),
                                               sd::exit_scratch_text(damaged), replaced));
            ready("commit replacement", store.commit(pd::ScratchDurability::buffered));
            need("replaced handle", replaced, handle);
            const auto committed = stored(store, handle, Field::value);
            need("committed length", committed.size(), damaged.size());
            for (std::size_t i = 0; i < damaged.size(); ++i)
                need("committed byte", +static_cast<unsigned char>(committed[i]),
                     +static_cast<unsigned char>(damaged[i]));
            try {
                reader.expect_error(handle, Status::corrupt, "corrupt", fault.diagnostic);
                std::cout << "PASS " << row << " ExitScratchError(corrupt)\n";
            } catch (const std::exception& e) {
                std::cerr << "FAIL " << row << ' ' << e.what() << '\n';
                passed = false;
            }
        }
        return passed;
    } catch (const std::exception& e) {
        std::cerr << "NOT COMPARED " << row << ": expected=verified fixture actual=" << e.what() << '\n';
        return false;
    }
}
} // namespace

int main() {
    bool passed = true;
    for (bool pending : {false, true})
        for (bool segment : {false, true})
            if (!check_reader(pending, segment)) passed = false;
    return passed ? 0 : 1;
}
