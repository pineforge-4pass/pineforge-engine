import argparse
from pathlib import Path
import re


def generate(source):
    retirement = source.split("void PineExecutionAdapter::erase_retired_rows(", 1)[1]
    retirement = retirement.split("\nvoid PineExecutionAdapter::", 1)[0]
    fragments = []
    for cursor, values, name, candidate in (
        ("askable_cursor", "askable_origins", "prepass_membership", False),
        ("candidate_origin_cursor", "askable_origins", "candidate_membership", True),
        ("root_cursor", "roots", "root_membership", False),
    ):
        terminal = r"const bool askable\s*=.*?;" if candidate else r"if\s*\(.*?\)\s*return false;"
        pattern = rf"while\s*\({cursor}\b.*?\+\+{cursor};\s*{terminal}"
        matches = list(re.finditer(pattern, retirement, re.DOTALL))
        if len(matches) != 1:
            fragments.append(f'#error "retirement cursor {cursor}: expected one production membership walk"')
            continue
        body = matches[0].group()
        result = "return askable;" if candidate else "return true;"
        fragments.append(
            f"template<class Cursor, class Values>\n"
            f"bool {name}(Cursor& {cursor}, const Values& {values}, std::uint64_t incarnation) {{\n"
            f"{body}\n{result}\n}}\n"
        )
    return "#pragma once\n" + "\n".join(fragments)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    arguments = parser.parse_args()
    arguments.output.write_text(generate(arguments.source.read_text()))
