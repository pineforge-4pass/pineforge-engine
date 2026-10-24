#include <pineforge/detail/drawing_records.hpp>

namespace pineforge::detail {

std::shared_ptr<DrawingStorage> open_drawing_storage() {
    auto storage = std::make_shared<DrawingStorage>();
    // INTERIM factory only. Lane a5s replaces this with the run's shared
    // ScratchBacking boundary, along with the two marked provider files.
    storage->backing = o1::make_file_backing(nullptr);
    storage->owned_store = o1::Store::open(o1::Shape{}, *storage->backing);
    storage->store = storage->owned_store.get();
    return storage;
}

} // namespace pineforge::detail
