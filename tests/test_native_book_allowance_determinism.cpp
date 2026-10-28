#include "book_allowance_run_fixture.hpp"

#include <exception>
#include <fstream>
#include <iterator>

int main(int argc, char** argv) {
    using namespace book_allowance_run;
    try {
        const std::string small = transcript(64);
        require(small == transcript(64, false),
                "resident run agrees with a full candidate rescan after every refresh");
        // The second run spills and refreshes rows, yet is small enough to
        // capture on the unchanged engine without a long promotion sweep.
        const std::string actual = "book-allowance-reference/v1\n" + small + transcript(4100);
        if (argc == 2 && std::string(argv[1]) == "--record") {
            std::fputs(actual.c_str(), stdout);
            return 0;
        }
        const std::string source = __FILE__;
        const auto slash = source.find_last_of("/\\");
        const std::string reference = argc == 3 && std::string(argv[1]) == "--verify"
            ? argv[2] : source.substr(0, slash + 1) + "data/book_allowance_refresh/reference.txt";
        require(argc == 1 || (argc == 3 && std::string(argv[1]) == "--verify"),
                "usage: test_native_book_allowance_determinism [--record | --verify PATH]");
        std::ifstream input(reference, std::ios::binary);
        require(input.is_open(),
                "lab baseline transcript required: tests/data/book_allowance_refresh/reference.txt");
        const std::string expected((std::istreambuf_iterator<char>(input)),
                                    std::istreambuf_iterator<char>());
        require(!input.bad(), "read recorded baseline transcript");
        if (actual != expected) {
            std::fprintf(stderr, "expected:\n%sactual:\n%s", expected.c_str(), actual.c_str());
            throw std::runtime_error("continuation, queue, trades or fills differ from the baseline");
        }
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "book allowance determinism: %s\n", error.what());
        return 1;
    }
}
