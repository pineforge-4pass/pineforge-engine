#include <pineforge/array.hpp>
#include <pineforge/engine.hpp>
#include "array_store_fixture.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <vector>

namespace {
using namespace pineforge;
using array_test::require;

void restored(const ScriptArray<double>& actual, const std::vector<double>& expected) {
    require(actual.size() == expected.size(), "rollback lost the size word");
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const auto value = actual.get_raw(static_cast<std::int64_t>(i));
        require(std::memcmp(&value, &expected[i], sizeof value) == 0, "rollback changed an element bit");
    }
}
} // namespace

int main() {
    try {
        array_test::StoreFixture fixture(3);
        auto& store = *fixture.store;
        ScriptArray<double> values(store);
        // More than sixteen 4 KiB pages, with a three-page Store. Changes and
        // rollback must work after both old and new versions have been evicted.
        std::vector<double> expected(8193);
        for (std::size_t i = 0; i < expected.size(); ++i) {
            expected[i] = i % 29 == 0 ? -0.0 : static_cast<double>(i) * 0.125;
            values.push(expected[i]);
        }
        auto alias = values.alias();
        const auto root = values.id();
        const auto capacity = values.capacity();
        using Child = ScriptArray<double>;
        ScriptArray<Child> nested(store);
        nested.push(Child::from(store, {1.0, 2.0, 3.0}));
        ScriptArray<std::string> strings(store, 1, std::string(8193, 'a'));
        const auto live = store.stats().bytes_live;
        std::optional<ScriptArray<double>> born;
        {
            ScriptArrayTransaction transaction(store);
            alias.set_i(0, 999.0);
            alias.push(101.0);
            alias.remove_i(4096);
            alias.insert(4095, -3.0);
            alias.reserve(capacity * 2);
            nested.mutate(0, [](Child& child) { child.clear(); child.push(55.0); });
            strings.set_i(0, std::string(16385, 'z'));
            born.emplace(store, 3, 7.0);
            // Assignment must be rolled back through the ORIGINAL root id,
            // rather than swapping a nontransactional id in the C++ handle.
            values = Child::from(store, {-1.0, -2.0});
            require(values.id() == root && alias.size() == 2 && alias.first() == -1.0,
                    "assignment bypassed the live alias");
            transaction.rollback();
        }
        require(values.id() == root && alias.id() == root, "rollback changed array identity");
        restored(values, expected);
        restored(alias, expected);
        require(values.capacity() == capacity, "rollback did not restore reserved extent");
        require(nested.first().size() == 3 && nested.first().last() == 3.0,
                "nested replacement/release did not roll back");
        require(strings.first() == std::string(8193, 'a'), "string child did not roll back");
        require(!born->alive(), "rollback kept a transaction-created array alive");
        const auto born_id = born->id();
        born.reset();
        require(store.stats().bytes_live == live, "rollback leaked a created/replaced object");
        {
            ScriptArray<double> after_rollback(store);
            require(after_rollback.id() > born_id, "array reused a transaction-aborted id");
        }
        {
            ScriptArrayTransaction transaction(store);
            alias.set_i(0, 42.0);
            transaction.commit();
        }
        require(values.first() == 42.0, "transaction commit was lost");
        {
            ScriptArrayTransaction abandoned(store);
            alias.set_i(0, -42.0);
        }
        require(values.first() == 42.0, "abandoned transaction did not roll back");
        BrokerStateHashSink before, after;
        values.hash_into(before);
        {
            ScriptArrayTransaction transaction(store);
            values.clear();
            values.push(0.0);
            transaction.rollback();
        }
        values.hash_into(after);
        require(before.h == after.h, "rollback did not restore the canonical value hash");
        {
            ScriptArrayTransaction outer(store);
            {
                ScriptArrayTransaction joined(store);
                alias.set_i(0, 84.0);
                joined.commit();
                require(!joined.active() && values.first() == 84.0,
                        "joined commit did not preserve the pending array write");
            }
            outer.rollback();
        }
        require(values.first() == 42.0, "joined commit escaped outer array rollback");
        {
            ScriptArrayTransaction outer(store);
            {
                ScriptArrayTransaction joined(store);
                alias.set_i(0, 85.0);
                joined.rollback(); // No savepoint: the outer scope is rollback-only.
            }
            bool refused_commit = false;
            try { outer.commit(); }
            catch (const std::logic_error&) { refused_commit = true; }
            require(refused_commit && !outer.active(),
                    "failed outer commit retained a stale transaction handle");
            require(values.first() == 42.0, "rollback-only commit kept pending array bytes");
            ScriptArrayTransaction following(store);
            alias.set_i(0, 86.0);
            outer.rollback(); // The finished wrapper must not touch the new outer.
            require(values.first() == 86.0, "stale array scope rolled back a later transaction");
            following.rollback();
        }
        require(values.first() == 42.0, "later transaction did not restore the array");
        std::puts("U01 rollback: aliases, assignment, resize, descendants, birth and flattened scopes");
        return 0;
    } catch (const std::exception& failure) {
        std::fprintf(stderr, "U01 rollback: %s\n", failure.what());
        return 1;
    }
}
