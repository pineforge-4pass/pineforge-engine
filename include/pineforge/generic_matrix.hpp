#pragma once
#include <vector>
#include <string>
#include <type_traits>
#include <algorithm>
#include <stdexcept>
#include <cstdint>

namespace pineforge {

template <typename T>
class PineGenericMatrix {
    std::vector<std::vector<T>> data_;

public:
    ~PineGenericMatrix() = default;
};

} // namespace pineforge
