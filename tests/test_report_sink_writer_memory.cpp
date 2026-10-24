// R19: allocation traffic as well as retained bytes, on the frozen writer API.
#include <pineforge/report_sink_writer.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <fcntl.h>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace allocation_probe {
struct alignas(std::max_align_t) Header { std::size_t size; bool counted; };
bool enabled = false;
std::size_t calls = 0, traffic = 0, retained = 0, peak = 0;
}

void* operator new(std::size_t size) {
    using namespace allocation_probe;
    auto* header = static_cast<Header*>(std::malloc(sizeof(Header) + size));
    if (!header) throw std::bad_alloc();
    header->size = size;
    header->counted = enabled;
    if (enabled) {
        ++calls;
        traffic += size;
        retained += size;
        if (retained > peak) peak = retained;
    }
    return header + 1;
}
void operator delete(void* value) noexcept {
    if (!value) return;
    auto* header = static_cast<allocation_probe::Header*>(value) - 1;
    if (header->counted) allocation_probe::retained -= header->size;
    std::free(header);
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete[](void* value) noexcept { ::operator delete(value); }
void operator delete(void* value, std::size_t) noexcept { ::operator delete(value); }
void operator delete[](void* value, std::size_t) noexcept { ::operator delete(value); }

namespace {
struct Sample { std::size_t calls, traffic, peak, cached; };

std::size_t cached_pages(const std::string& path) {
    const int fd = open(path.c_str(), O_RDONLY);
    struct stat info{};
    if (fd < 0 || fstat(fd, &info) != 0)
        throw std::runtime_error("R19 cache probe cannot open the output");
    const auto size = static_cast<std::size_t>(info.st_size);
    void* mapping = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (mapping == MAP_FAILED) {
        close(fd);
        throw std::runtime_error("R19 cache probe cannot map the output");
    }
    const auto page = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    std::vector<unsigned char> resident((size + page - 1) / page);
#if defined(__APPLE__)
    const int status = mincore(mapping, size, reinterpret_cast<char*>(resident.data()));
#else
    const int status = mincore(mapping, size, resident.data());
#endif
    std::size_t count = 0;
    for (const auto value : resident) count += value & 1;
    munmap(mapping, size);
    close(fd);
    if (status != 0) throw std::runtime_error("R19 cache residency query failed");
    return count;
}

Sample sample(const std::string& path, int count) {
    // Width exercises the old record/string allocation as well as the 64 KiB
    // boundary; no input allocation belongs to the measured writer.
    const std::string message(70001, '\n');
    const pf_report_sink_v1 config{sizeof(config), 1, path.c_str(), "R19", nullptr};
    allocation_probe::calls = allocation_probe::traffic = allocation_probe::peak = 0;
    allocation_probe::enabled = true;
    {
        pineforge::report_sink::LineWriter writer(config);
        writer.write_header({false, true, false, 0, 0});
        for (int index = 0; index < count; ++index)
            writer.write_x({1700000000000LL + index, index, 0, -0.}, message);
        const auto receipt = writer.write_trailer();
        if (receipt.count_x != count || receipt.count_e || receipt.count_t)
            throw std::runtime_error("R19 fixture did not emit its full population");
    }
    allocation_probe::enabled = false;
    if (allocation_probe::retained)
        throw std::runtime_error("R19 writer retained allocations after destruction");
    const auto cached = cached_pages(path);  // before the independent buffered reader
    std::ifstream artifact(path, std::ios::binary);
    std::size_t rows = 0;
    std::string line;
    while (std::getline(artifact, line)) ++rows;
    if (rows != static_cast<std::size_t>(count) + 2)
        throw std::runtime_error("R19 stream population differs from the receipt");
    return {allocation_probe::calls, allocation_probe::traffic, allocation_probe::peak, cached};
}
}

int main() {
    try {
        const char* temporary = std::getenv("TMPDIR");
        std::string directory = std::string(temporary && *temporary ? temporary : "/tmp")
            + "/pineforge-r19-XXXXXX";
        if (!mkdtemp(directory.data())) throw std::runtime_error("R19 disk directory creation failed");
        const auto small_path = directory + "/small.sink";
        const auto large_path = directory + "/large.sink";
        const auto small = sample(small_path, 8);
        const auto large = sample(large_path, 256);
        std::printf("R19 calls=%zu/%zu traffic=%zu/%zu retained_peak=%zu/%zu cache_pages=%zu/%zu\n",
                    small.calls, large.calls, small.traffic, large.traffic, small.peak, large.peak,
                    small.cached, large.cached);
        if (large.calls != small.calls || large.traffic != small.traffic
                || large.peak != small.peak || large.peak > 2 * 1024 * 1024)
            throw std::runtime_error("R19 allocation traffic/retained bound grows with records");
        if (small.cached > 1 || large.cached > 1)
            throw std::runtime_error("R19 output data cache grows with the record stream");
        std::remove(small_path.c_str());
        std::remove(large_path.c_str());
        rmdir(directory.c_str());
        return 0;
    } catch (const std::exception& error) {
        allocation_probe::enabled = false;
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
