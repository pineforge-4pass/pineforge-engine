#include "p2_a5strmap_fixture.hpp"
#include "p2_a5strmap_store_fixture.hpp"
#include <pineforge/map.hpp>
#include <pineforge/str_utils.hpp>

#include <memory>
#include <utility>

namespace {
using namespace pineforge;
using a5_fixture::require;
using Map = PineMap<std::string, std::string>;
using Model = std::vector<std::pair<std::string, std::string>>;

std::string key_for(unsigned key) {
    std::string text(key % 2 ? 48 : 8, static_cast<char>('a' + key % 26));
    text[0] = static_cast<char>(key);
    text[3] = '\0';
    return text;
}
std::string value_for(unsigned seed) {
    std::string text(seed % 3 ? 120 : 12, static_cast<char>('A' + seed % 26));
    text[1] = '\0';
    text[4] = static_cast<char>(seed >> 8);
    return text;
}
auto locate(Model& model, const std::string& key) {
    return std::find_if(model.begin(), model.end(),
                        [&](const auto& row) { return row.first == key; });
}
std::string put(Model& model, const std::string& key, const std::string& value) {
    const auto found = locate(model, key);
    if (found == model.end()) {
        model.emplace_back(key, value);
        return na<std::string>();
    }
    auto previous = found->second;
    found->second = value;
    return previous;
}
void exact(const Map& map, const Model& model) {
    const auto keys = map.keys();
    const auto values = map.values();
    require(map.size() == static_cast<int>(model.size()) && keys.size() == model.size() &&
                values.size() == model.size(), "map differential size differs");
    for (std::size_t i = 0; i < model.size(); ++i) {
        require(keys[i] == model[i].first && values[i] == model[i].second,
                "map differential order or bytes differ");
        require(map.contains(model[i].first) && map.get(model[i].first) == model[i].second,
                "map differential lookup differs");
    }
}

void differential(a5_fixture::Session& session) {
    const auto initial_live = session.store->stats().bytes_live;
    {
        auto map = Map::new_();
        Model model;
        std::uint32_t state = 0x4658c2d9U;
        for (unsigned step = 0; step < 3000; ++step) {
            state = state * 1664525U + 1013904223U;
            const auto key = key_for((state >> 9) % 128);
            const auto value = value_for(state);
            switch ((state >> 21) % 8) {
            case 0: {
                const auto found = locate(model, key);
                const auto expected = found == model.end() ? na<std::string>() : found->second;
                require(map.remove(key) == expected, "map remove previous value differs");
                if (found != model.end()) model.erase(found);
                break;
            }
            case 1:
                require(map.contains(key) == (locate(model, key) != model.end()),
                        "map contains differs");
                break;
            case 2: {
                auto independent = map.copy();
                auto copied_model = model;
                require(independent.put(key, value) == put(copied_model, key, value),
                        "map copy previous value differs");
                exact(independent, copied_model);
                exact(map, model);
                break;
            }
            case 3: {
                const auto snapshot = map.snapshot();
                const auto alias = map;
                const auto keys = map.keys();
                const auto values = map.values();
                (void)map.put(key, value);
                map.clear();
                map.restore(snapshot);
                exact(alias, model);
                map = Map{};
                map.restore(snapshot);
                exact(map, model);
                require(keys.size() == model.size() && values.size() == model.size(),
                        "map enumeration version count changed");
                for (std::size_t i = 0; i < model.size(); ++i)
                    require(keys[i] == model[i].first && values[i] == model[i].second,
                            "enumeration changed after clear/restore/rebind");
                break;
            }
            case 4: {
                auto tx = session.store->begin();
                (void)map.put(key, value);
                if (!model.empty()) (void)map.remove(model.front().first);
                tx->rollback();
                exact(map, model);
                break;
            }
            default:
                require(map.put(key, value) == put(model, key, value),
                        "map put previous value differs");
                break;
            }
            if (step % 97 == 0) exact(map, model);
        }
        exact(map, model);
        map.clear();
        exact(map, Model{});
    }
    require(session.store->stats().bytes_live == initial_live,
            "map/snapshot/enumeration retained unreachable payloads");
}

void external_strings_and_aliases(a5_fixture::Session& session) {
    const auto initial_live = session.store->stats().bytes_live;
    {
        auto map = PineMap<ScriptString, ScriptString>::new_();
        ScriptString key(*session.store, std::string_view("key\0tail", 8));
        ScriptString original(*session.store, "value");
        (void)map.put(key, original);
        const auto values = map.values();
        const auto saved = map.snapshot();
        original += "-changed";
        require(map.get(key) == "value", "map ScriptString payload lost value semantics");
        auto copy = map.copy();
        (void)map.put(key, original);
        require(values[0] == "value" && copy.get(key) == "value",
                "map copy/enumeration changed retained ScriptString bytes");
        map.restore(saved);
        require(map.get(key) == "value", "map ScriptString snapshot did not restore");

        // The unchanged opaque fallback must preserve the old shallow
        // reference semantics until generated UDT/collection codecs exist.
        auto handles = PineMap<int, std::shared_ptr<int>>::new_();
        auto handle = std::make_shared<int>(7);
        (void)handles.put(1, handle);
        auto shallow = handles.copy();
        *handle = 19;
        require(*handles.get(1) == 19 && *shallow.get(1) == 19,
                "opaque handle fallback changed aliasing");
    }
    require(session.store->stats().bytes_live == initial_live,
            "external string map codec retained unreachable roots");
}

void escaping_value_owns_hidden_store() {
    std::weak_ptr<detail::MapStoreHolder> lifetime;
    {
        auto escaped = [&] {
            auto holder = std::make_shared<detail::MapStoreHolder>(
                std::make_unique<a5_fixture::DiskBacking>(), detail::o1::Shape{4096, 8});
            lifetime = holder;
            detail::MapStoreScope scope(holder);
            auto map = PineMap<int, ScriptString>::new_();
            ScriptString text(*holder->store, std::string_view("owned\0bytes", 11));
            (void)map.put(1, text);
            return map.values()[0];
        }();
        require(!lifetime.expired(), "escaping map value lost its hidden Store owner");
        auto slice = escaped.substr(2);
        auto prefix = "prefix:" + escaped;
        escaped = ScriptString{};
        require(slice == std::string_view("ned\0bytes", 9), "escaped slice lost bytes or lifetime");
        prefix.with_std_string([](std::string& text) { text += "!"; });
        require(prefix == std::string_view("prefix:owned\0bytes!", 19),
                "raw boundary dropped the hidden Store owner");
        prefix += "-after";
        require(prefix.size() == 25, "escaped value stopped growing exactly");
        const auto formatted = str_format(prefix, std::vector<ScriptString>{});
        prefix = ScriptString{};
        slice = ScriptString{};
        require(!lifetime.expired() &&
                    formatted == std::string_view("prefix:owned\0bytes!-after", 25),
                "formatting dropped the hidden Store owner");
    }
    require(lifetime.expired(), "hidden Store owner retained a cycle after values died");
}
} // namespace

int main() {
    try {
        a5_fixture::Session session;
        auto holder = std::make_shared<detail::MapStoreHolder>(*session.store);
        detail::MapStoreScope map_scope(holder);
        ScriptStringScope string_scope(*session.store);
        differential(session);
        external_strings_and_aliases(session);
        escaping_value_owns_hidden_store();
        std::puts("A5MAP parity: insertion order/bytes/identity/versions/ownership checked");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "A5MAP parity FAIL: %s\n", error.what());
        return 1;
    }
}
