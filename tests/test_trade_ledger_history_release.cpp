#include <pineforge/engine.hpp>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <memory>
#include <stdexcept>
#include <unistd.h>
#include <utility>

namespace pineforge::detail {
struct TradeLedgerStorageProbe {
    static void bind(TradeLedger& ledger, TradeLedger::HistoryFactory factory, std::uint64_t bytes) {
        ledger.bind_retained_history(std::move(factory), bytes);
    }
    static void release(TradeLedger& ledger) { ledger.release(); }
};
} // namespace pineforge::detail

namespace {
void require(bool ok, const char* message) {
    if (!ok) {
        std::fprintf(stderr, "%s\n", message);
        std::exit(1);
    }
}
} // namespace

int main() {
    using pineforge::TradeLedger;
    using Probe = pineforge::detail::TradeLedgerStorageProbe;
    using File = std::unique_ptr<std::FILE, decltype(&std::fclose)>;
    constexpr std::uint64_t extent = 4096;
    File original(nullptr, &std::fclose);
    int calls = 0, owned = -1;
    auto factory = [&](std::uint64_t bytes) {
        require(bytes == extent, "wrong history extent requested");
        File file(std::tmpfile(), &std::fclose);
        require(file && ::ftruncate(::fileno(file.get()), static_cast<off_t>(bytes)) == 0,
                "could not create history file");
        owned = ::fcntl(::fileno(file.get()), F_DUPFD_CLOEXEC, 0);
        require(owned >= 0, "could not duplicate history descriptor");
        if (++calls == 1) original = std::move(file);
        return owned; // Only the duplicate belongs to the ledger.
    };

    int reused_fd;
    {
        TradeLedger ledger;
        Probe::bind(ledger, factory, extent);
        pineforge::Trade trade{};
        trade.entry_id = "first run";
        ledger.push_back(trade);
        require(ledger.read(0).entry_id == trade.entry_id, "first trade was not readable");
        reused_fd = owned;
        require(::fcntl(reused_fd, F_GETFD) >= 0, "history descriptor was not open before release");
        Probe::release(ledger);
        require(::fcntl(reused_fd, F_GETFD) == -1 && errno == EBADF,
                "release left the owned history descriptor open");

        // Occupy that exact slot before another release or the old State's destruction.
        require(::dup2(::fileno(original.get()), reused_fd) == reused_fd,
                "the caller's history descriptor did not survive release");
        Probe::release(ledger);
        require(::fcntl(reused_fd, F_GETFD) == 0, "repeat release closed the caller's descriptor");

        TradeLedger copy(ledger);
        require(calls == 1, "copying released results acquired a history descriptor");
        for (auto* next : {&ledger, &copy}) {
            const int before = calls;
            next->clear();
            require(::fcntl(reused_fd, F_GETFD) == 0, "clear closed the caller's descriptor");
            require(calls == before + 1 && next->empty(), "clear did not renew the history");
            trade.entry_id = next == &ledger ? "second run" : "copy";
            next->push_back(trade);
            require(next->read(0).entry_id == trade.entry_id, "new trade was not readable");
        }
    }
    require(::close(reused_fd) == 0, "ledger cleanup closed the caller's reused descriptor");

    TradeLedger empty;
    Probe::release(empty);
    bool refused = false;
    try { Probe::bind(empty, factory, extent); }
    catch (const std::logic_error&) { refused = true; }
    require(refused && calls == 3, "released ledger accepted a binding before clear");
}
