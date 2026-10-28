#include "p2_a5strmap_fixture.hpp"
#include <pineforge/str_utils.hpp>

#if __has_include(<pineforge/script_string.hpp>)
#include "p2_a5strmap_store_fixture.hpp"
#define PF_A5_EXTERNAL_STRING 1
#endif

int main() {
    try {
        constexpr std::size_t bytes = 2 * 1024 * 1024;
#ifdef PF_A5_EXTERNAL_STRING
        a5_fixture::Session session;
        pineforge::ScriptStringScope scope(*session.store);
        pineforge::ScriptString source(*session.store);
        std::array<char, 4096> chunk{};
        chunk.fill('x'); chunk[17] = '\0';
        for (std::size_t i = 0; i < bytes / chunk.size(); ++i)
            source.append(std::string_view(chunk.data(), chunk.size()));
        const pineforge::ScriptString separator(*session.store, ",");
        const auto live_before = session.store->stats().bytes_live;
#else
        // frozen2 .split builds vector<string> and copies the complete part.
        std::string source(bytes, 'x');
        for (std::size_t i = 17; i < bytes; i += 4096) source[i] = '\0';
        const std::string separator = ",";
#endif
        const auto before = a5_heap::live.load();
        a5_heap::reset_peak();
        {
            a5_heap::Window count;
            const auto parts = pineforge::str_split(source, separator);
            a5_fixture::require(parts.size() == 1 && parts[0] == source,
                                "split changed bytes including embedded NUL");
            a5_fixture::require(parts[0][17] == '\0', "split lost embedded NUL");
#ifdef PF_A5_EXTERNAL_STRING
            a5_fixture::require(session.store->stats().bytes_live == live_before,
                                "live split result duplicated source bytes in the Store");
#endif
        }
        const auto peak = a5_heap::growth(a5_heap::peak.load(), before);
        std::printf("A5SPLIT source=%zu peak=%zu bound=%zu\n",
                    bytes, peak, a5_fixture::operation_ceiling);
        a5_fixture::require(peak <= a5_fixture::operation_ceiling,
                            "split duplicated an owning source payload");
#ifdef PF_A5_EXTERNAL_STRING
        a5_fixture::require(session.store->stats().bytes_live == live_before,
                            "split duplicated source payload in the Store");
        pineforge::ScriptString many(*session.store);
        for (int i = 0; i < 100000; ++i) many += "a,,";
        const auto many_before = a5_heap::live.load();
        const auto many_live_before = session.store->stats().bytes_live;
        a5_heap::reset_peak();
        {
            a5_heap::Window count;
            const auto parts = pineforge::str_split(many, separator);
            std::uint64_t count_parts = 0, count_bytes = 0;
            for (const auto part : parts) {
                count_bytes += part.size();
                ++count_parts;
            }
            a5_fixture::require(count_parts == 200001 && count_bytes == 100000,
                                "streaming split lost empty or trailing parts");
            a5_fixture::require(session.store->stats().bytes_live == many_live_before,
                                "many-part split allocated an owning result directory");
        }
        a5_fixture::require(a5_heap::growth(a5_heap::peak.load(), many_before) <=
                                a5_fixture::operation_ceiling,
                            "split retained a growing boundary directory");
#endif
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "A5SPLIT FAIL: %s\n", error.what());
        return 1;
    }
}
