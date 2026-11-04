// Expected red on a3e7822e: the fixed arena refuses at distinct key 50,001.
// The ordered model, retained enumeration and copy checks remain real
// assertions after growth is implemented; a put-only size row is insufficient.
#include "cr559_map_capacity_fixture.hpp"

int main() {
    try {
        cr559::Session session;
        auto map = cr559::Map::new_();
        cr559::Model model;
        cr559::append(map, model, 0, 6127);
        const auto retained_keys = map.keys();
        const auto retained_values = map.values();
        const auto retained_model = model;

        const auto removed = model[17];
        cr559::require(map.remove(removed.first) == removed.second, "remove previous value differs");
        model.erase(model.begin() + 17);
        cr559::require(pineforge::is_na(map.put(removed.first, -47)),
                       "reinsertion was treated as replacement");
        model.emplace_back(removed.first, -47);
        cr559::require(map.put(model[29].first, -53) == model[29].second,
                       "replace-put lost its old value");
        model[29].second = -53;

        for (const cr559::Scalar end : {8193, 32769, 65537, 80003}) {
            cr559::append(map, model, static_cast<cr559::Scalar>(model.size()), end);
            cr559::exact(map, model);
        }
        cr559::require(session.store.index_growths > 1,
                       "ordered row never crossed a real bucket rehash");
        for (std::size_t index = 0; index < retained_model.size(); ++index) {
            cr559::require(retained_keys[index] == retained_model[index].first &&
                               retained_values[index] == retained_model[index].second,
                           "rehash changed a previously retained enumeration");
        }

        auto copy = map.copy();
        auto copy_model = model;
        cr559::append(copy, copy_model, 80003, 90009);
        cr559::require(copy.remove(copy_model[5].first) == copy_model[5].second,
                       "grown copy remove lost its value");
        copy_model.erase(copy_model.begin() + 5);
        cr559::exact(copy, copy_model);
        cr559::exact(map, model);

        auto overlay = cr559::Map::new_();
        (void)overlay.put(model[13].first, -59);
        cr559::Model added;
        cr559::append(overlay, added, 90009, 100021);
        map.put_all(overlay);
        model[13].second = -59;
        model.insert(model.end(), added.begin(), added.end());
        cr559::exact(map, model);
        std::puts("CR559 rehash: order/get/contains/replace/remove/copy/put_all checked");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "CR559 rehash FAIL: %s\n", error.what());
        return 1;
    }
}
