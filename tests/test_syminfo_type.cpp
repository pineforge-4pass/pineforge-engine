// syminfo.type / string-member injection (finding 454).
//
// The engine stores no instrument metadata of its own: syminfo.type defaults
// to "crypto" and until now had NO setter, so a script's instrument branch
// (`syminfo.type == "forex" ? 0.0001 : syminfo.mintick` — the canonical pip
// idiom) could never take the forex path even when the harness supplied a
// 5-digit FX mintick. Pins: default, setter, empty-ignored, the generic
// string setter's key routing, and the C ABI entry points.
#include <cstdio>
#include <cstring>
#include <string>

#include <pineforge/engine.hpp>
#include <pineforge/pineforge.h>

using namespace pineforge;

namespace {

struct TypeHarness : public BacktestEngine {
    void on_bar(const Bar& /*bar*/) override {}
    const SymInfo& sym() const { return syminfo_; }
    // The pip idiom every FX script spells out; evaluated the way the codegen
    // emits it (syminfo_.type / syminfo_.mintick member reads).
    double pip() const {
        return syminfo_.type == "forex" ? 0.0001 : syminfo_.mintick;
    }
};

int tests_run = 0;
int tests_passed = 0;

#define CHECK(cond, msg) do { \
    ++tests_run; \
    if (cond) { ++tests_passed; printf("  PASS: %s\n", msg); } \
    else { printf("  FAIL: %s\n", msg); } \
} while (0)

void test_default_is_crypto() {
    TypeHarness h;
    CHECK(h.sym().type == "crypto", "syminfo.type defaults to \"crypto\"");
    CHECK(h.sym().ticker == "UNKNOWN", "syminfo.ticker default unchanged");
    CHECK(h.sym().currency == "USD", "syminfo.currency default unchanged");
    CHECK(h.sym().basecurrency.empty(), "syminfo.basecurrency default unchanged");
}

void test_set_type() {
    TypeHarness h;
    h.set_syminfo_mintick(0.00001);
    CHECK(h.pip() == 0.00001, "crypto default: pip idiom falls through to mintick");
    h.set_syminfo_type("forex");
    CHECK(h.sym().type == "forex", "set_syminfo_type lands on syminfo_.type");
    CHECK(h.pip() == 0.0001, "forex: pip idiom takes the 0.0001 branch");
    h.set_syminfo_type("");
    CHECK(h.sym().type == "forex", "empty type is ignored");
    h.set_syminfo_type("stock");
    CHECK(h.sym().type == "stock", "type overwrite wins");
}

void test_set_string_members() {
    TypeHarness h;
    CHECK(h.set_syminfo_string("ticker", "EURUSD"), "ticker set");
    CHECK(h.set_syminfo_string("tickerid", "OANDA:EURUSD"), "tickerid set");
    CHECK(h.set_syminfo_string("currency", "USD"), "currency set");
    CHECK(h.set_syminfo_string("basecurrency", "EUR"), "basecurrency set");
    CHECK(h.set_syminfo_string("description", "Euro / U.S. Dollar"), "description set");
    CHECK(h.set_syminfo_string("volumetype", "tick"), "volumetype set");
    CHECK(h.set_syminfo_string("type", "forex"), "type via generic setter");
    CHECK(h.sym().ticker == "EURUSD" && h.sym().tickerid == "OANDA:EURUSD"
          && h.sym().currency == "USD" && h.sym().basecurrency == "EUR"
          && h.sym().description == "Euro / U.S. Dollar"
          && h.sym().volumetype == "tick" && h.sym().type == "forex",
          "all generic string members read back");
    CHECK(!h.set_syminfo_string("mintick", "0.01"), "numeric member rejected by string setter");
    CHECK(!h.set_syminfo_string("nonsense", "x"), "unknown key rejected");
    CHECK(!h.set_syminfo_string("ticker", ""), "empty value rejected");
    CHECK(h.sym().ticker == "EURUSD", "rejected empty value leaves field intact");
}

void test_c_abi() {
    TypeHarness h;
    pf_strategy_t s = static_cast<pf_strategy_t>(static_cast<BacktestEngine*>(&h));
    strategy_set_syminfo_type(s, "forex");
    CHECK(h.sym().type == "forex", "C ABI strategy_set_syminfo_type");
    strategy_set_syminfo_type(s, nullptr);
    CHECK(h.sym().type == "forex", "C ABI NULL type ignored");
    strategy_set_syminfo_type(nullptr, "stock");  // must not crash
    CHECK(strategy_set_syminfo_string(s, "basecurrency", "EUR") == 0,
          "C ABI strategy_set_syminfo_string returns 0 on success");
    CHECK(h.sym().basecurrency == "EUR", "C ABI string member lands");
    CHECK(strategy_set_syminfo_string(s, "bogus", "x") == -1,
          "C ABI unknown key returns -1");
    CHECK(strategy_set_syminfo_string(s, nullptr, "x") == -1,
          "C ABI NULL key returns -1");
    CHECK(strategy_set_syminfo_string(nullptr, "type", "x") == -1,
          "C ABI NULL handle returns -1");
}

}  // namespace

int main() {
    printf("test_syminfo_type\n");
    test_default_is_crypto();
    test_set_type();
    test_set_string_members();
    test_c_abi();
    printf("%d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
