#include <algorithm>
#include <array>
#include <cstddef>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct LayoutNote {
    const char* text;
    const char* expected_section;
    std::string section;
    std::size_t occurrences = 0;
};

bool starts_with(std::string_view line, std::string_view prefix) {
    return line.substr(0, prefix.size()) == prefix;
}

int not_compared(const char* reason, const std::string& actual, const char* expected) {
    std::cerr << "NOT COMPARED: " << reason << "; actual=" << actual
              << "; expected=" << expected << '\n';
    return 1;
}

}  // namespace

int main() {
    std::ifstream input(PINEFORGE_CHANGELOG_PATH);
    if (!input.is_open()) {
        return not_compared("cannot open changelog", PINEFORGE_CHANGELOG_PATH,
                            "readable PineForge CHANGELOG.md");
    }

    std::vector<std::string> headings;
    std::vector<std::string> sections_with_entries;
    std::string title;
    std::string intro;
    std::string line;
    std::array<LayoutNote, 2> notes{{
        {"`PF_STREAM_ENGINE_LAYOUT_VERSION`, is now 5.", "1.14.0", {}},
        {"`PF_STREAM_ENGINE_LAYOUT_VERSION` moves from 3 to 4", "1.11.0", {}},
    }};
    std::size_t number = 0;
    while (std::getline(input, line)) {
        ++number;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (number == 1) {
            title = line;
        } else if (number == 3) {
            intro = line;
        }
        if (starts_with(line, "## ")) {
            const auto heading = std::string_view(line).substr(3);
            headings.emplace_back(heading.substr(0, heading.find_first_of(" \t")));
        } else if (!headings.empty() && starts_with(line, "- ") && line.size() > 2 &&
                   (sections_with_entries.empty() || sections_with_entries.back() != headings.back())) {
            sections_with_entries.push_back(headings.back());
        }
        for (auto& note : notes) {
            if (line.find(note.text) != std::string::npos) {
                ++note.occurrences;
                note.section = headings.empty() ? "<no heading>" : headings.back();
            }
        }
    }
    if (input.bad() || number == 0) {
        return not_compared("changelog is empty or could not be read",
                            std::to_string(number) + " lines read", "a nonempty readable file");
    }
    if (title != "# Changelog" || !starts_with(intro, "Notable changes to pineforge-engine.")) {
        return not_compared("wrong changelog identity", '"' + title + "\" / \"" + intro + '"',
                            "\"# Changelog\" / \"Notable changes to pineforge-engine.\"");
    }
    const auto anchor_count = std::count(headings.begin(), headings.end(), "1.9.1");
    if (anchor_count != 1) {
        return not_compared("missing or repeated 1.9.1 historical anchor",
                            std::to_string(anchor_count), "one 1.9.1 heading");
    }

    // Check the affected release range in order, allowing newer releases above it.
    const std::array<const char*, 5> releases = {
        "1.14.0", "1.13.0", "1.12.0", "1.11.0", "1.10.0",
    };
    const auto anchor = std::find(headings.begin(), headings.end(), "1.9.1");
    auto position = headings.begin();
    if (*position != "Unreleased") {
        std::cerr << "FAIL: first changelog heading: actual=" << *position
                  << "; expected=Unreleased\n";
        return 1;
    }
    for (const auto* version : releases) {
        const auto found = std::find(position + 1, headings.end(), version);
        const auto occurrences = std::count(headings.begin(), headings.end(), version);
        if (found >= anchor || occurrences != 1) {
            std::cerr << "FAIL: CHANGELOG.md expected ## " << version
                      << " once in order before ## 1.9.1; occurrences=" << occurrences << '\n';
            return 1;
        }
        if (std::find(sections_with_entries.begin(), sections_with_entries.end(), version) ==
            sections_with_entries.end()) {
            std::cerr << "FAIL: CHANGELOG.md ## " << version
                      << ": actual=0 entries; expected=at least 1 entry\n";
            return 1;
        }
        position = found;
    }

    for (const auto& note : notes) {
        if (note.occurrences != 1 || note.section != note.expected_section) {
            std::cerr << "FAIL: layout stamp note: actual="
                      << (note.section.empty() ? "<missing>" : note.section)
                      << " (" << note.occurrences << " occurrences); expected="
                      << note.expected_section << " (1 occurrence)\n";
            return 1;
        }
    }
    std::cout << "PASS: CHANGELOG.md files the 1.10.0 through 1.14.0 releases "
                 "and both layout stamp changes under their release headings\n";
    return 0;
}
