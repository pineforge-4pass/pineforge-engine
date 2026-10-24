#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// This is the shipped UDT shape, including the owning numeric vector. The
// optional mapping lets the SAME behavioural test compile on frozen2.
struct P2MatrixPayload {
    std::int64_t bar = 0;
    double price = 0;
    std::string label;
    std::vector<double> history;
};

inline P2MatrixPayload p2_matrix_payload(int bar) {
    P2MatrixPayload value;
    value.bar = bar;
    value.price = static_cast<double>(bar) + 0.125;
    value.label.assign(48, static_cast<char>('a' + bar % 26));
    value.label[7] = '\0';
    value.history = {static_cast<double>(bar), -0.0, 0.125, -17.25};
    return value;
}

inline bool p2_payload_equal(const P2MatrixPayload& a, const P2MatrixPayload& b) {
    return a.bar == b.bar && std::memcmp(&a.price, &b.price, sizeof(double)) == 0
        && a.label == b.label && a.history.size() == b.history.size()
        && (a.history.empty() || std::memcmp(a.history.data(), b.history.data(),
                                             a.history.size() * sizeof(double)) == 0);
}

#if __has_include(<pineforge/detail/matrix_store.hpp>)
#include <pineforge/detail/matrix_store.hpp>
namespace pineforge::detail::matrix_store {
template<>
struct codec<P2MatrixPayload, void> {
    static constexpr bool mapped = true;
    static constexpr std::uint32_t record_bytes = 48;
    static constexpr std::uint32_t owned_slots = 2;
    static void encode(Encoder& encoder, const P2MatrixPayload& value,
                       unsigned char* record) {
        std::memcpy(record, &value.bar, 8);
        std::memcpy(record + 8, &value.price, 8);
        codec<std::string>::encode(encoder, value.label, record + 16);
        const std::uint64_t count = value.history.size();
        const Id id = encoder.put(value.history.data(), count * sizeof(double));
        std::memcpy(record + 32, &count, 8);
        std::memcpy(record + 40, &id, 8);
    }
    static P2MatrixPayload decode(const Decoder& decoder, const unsigned char* record) {
        P2MatrixPayload value;
        std::memcpy(&value.bar, record, 8);
        std::memcpy(&value.price, record + 8, 8);
        value.label = codec<std::string>::decode(decoder, record + 16);
        std::uint64_t count = 0;
        Id id = 0;
        std::memcpy(&count, record + 32, 8);
        std::memcpy(&id, record + 40, 8);
        value.history.resize(static_cast<std::size_t>(count));
        if (count) decoder.get(id, value.history.data(), count * sizeof(double));
        return value;
    }
    // Direct, immutable payload leaves. There is no pointer-graph traversal in
    // this value codec; generated reference UDTs use UdtGraph ids instead.
    static Id owned(const unsigned char* record, std::uint32_t slot) {
        if (slot == 0) return codec<std::string>::owned(record + 16, 0);
        Id id = 0;
        std::memcpy(&id, record + 40, 8);
        return id;
    }
};
} // namespace pineforge::detail::matrix_store
#endif
