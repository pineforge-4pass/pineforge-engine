#!/usr/bin/env python3
"""Collect the command-after book partition without executing any books."""
import ast
from collections import Counter
from itertools import product
from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
PREFIX = 'test_native_command_after'


def refuse_protected(path: Path) -> None:
    parts = path.parts
    if (path.name.endswith('.env')
            or any(part in {'.ssh', '.aws', '.codex'} for part in parts)
            or any(parts[index:index + len(blocked)] == blocked
                   for blocked in (('.config', 'zsh', 'keys'),
                                   ('.config', 'pineforge'))
                   for index in range(len(parts)))):
        raise ValueError(f'refusing protected source path: {path}')


def read_sources() -> tuple[str, str]:
    paths = (ROOT / 'tests/test_native_command_after.cpp',
             ROOT / 'tests/CMakeLists.txt')
    for path in paths:
        refuse_protected(path)
    paths = tuple(path.resolve() for path in paths)
    for path in paths:
        refuse_protected(path)
    print('Source files followed by the cover test:', flush=True)
    for path in paths:
        print(f'  {path}', flush=True)
    return tuple(path.read_text(encoding='utf-8') for path in paths)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def code(text: str) -> str:
    return re.sub(r'\s+', '', re.sub(r'//[^\n]*|/\*.*?\*/', '', text, flags=re.DOTALL))


def array_values(cpp: str, name: str) -> tuple[str, ...]:
    matches = re.findall(
        rf'constexpr std::array<[^,]+,\s*(\d+)>\s+{name}\s*=\s*\{{([^{{}}]*)\}};',
        cpp)
    require(len(matches) == 1, f'expected one case-table axis {name}')
    size, values = matches[0]
    result = tuple(value.strip() for value in values.split(',') if value.strip())
    require(len(result) == int(size), f'case-table axis size differs: {name}')
    return result


def constant(cpp: str, name: str) -> int:
    values = re.findall(rf'\b{name}\s*=\s*(\d+)\s*;', cpp)
    require(len(values) == 1, f'expected one case-table constant {name}')
    return int(values[0])


def slice_costs(cmake: str) -> dict[str, int]:
    """Read literal scheduling hints, including duplicate assignments."""
    costs = {}
    source = re.sub(r'#[^\n]*', '', cmake)
    for names, properties in re.findall(
            r'set_tests_properties\(([^()]*?)\s+PROPERTIES\s+([^()]*)\)', source):
        rows = [name for name in names.split()
                if re.fullmatch(rf'{PREFIX}_slice_\d+', name)]
        if not rows:
            continue
        tokens = properties.split()
        require(len(tokens) % 2 == 0, 'malformed slice properties')
        for key, value in zip(tokens[::2], tokens[1::2]):
            if key != 'COST':
                continue
            require(re.fullmatch(r'\d+', value) is not None,
                    'slice COST must be a literal nonnegative integer')
            for name in rows:
                require(name not in costs, f'duplicate slice COST: {name}')
                costs[name] = int(value)
    return costs


def integer_value(expression: str, values: dict[str, int],
                  arrays: dict[str, tuple[int, ...]] | None = None) -> int:
    """Evaluate only integer arithmetic, array lookups and sizes from C++."""
    expression = re.sub(r'static_cast<int>\(([^()]*)\)', r'(\1)', expression)
    try:
        tree = ast.parse(expression, mode='eval').body
    except SyntaxError as error:
        raise ValueError(f'unsupported integer expression: {expression}') from error

    def visit(node: ast.AST) -> int:
        if isinstance(node, ast.Constant) and type(node.value) is int:
            return node.value
        if isinstance(node, ast.Name) and node.id in values:
            return values[node.id]
        if isinstance(node, ast.Subscript) and isinstance(node.value, ast.Name):
            name = node.value.id
            require(arrays is not None and name in arrays,
                    f'unknown integer array: {name}')
            index = visit(node.slice)
            require(0 <= index < len(arrays[name]),
                    f'out-of-range integer array index: {name}[{index}]')
            return arrays[name][index]
        if isinstance(node, ast.Attribute) and isinstance(node.value, ast.Name):
            key = f'{node.value.id}.{node.attr}'
            require(key in values, f'unknown integer field: {key}')
            return values[key]
        if (isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute)
                and isinstance(node.func.value, ast.Name) and node.func.attr == 'size'
                and not node.args and not node.keywords):
            key = f'{node.func.value.id}.size()'
            require(key in values, f'unknown array size: {key}')
            return values[key]
        require(isinstance(node, ast.BinOp)
                and isinstance(node.op, (ast.Add, ast.Sub, ast.Mult, ast.Div, ast.Mod)),
                f'unsupported integer expression: {expression}')
        left, right = visit(node.left), visit(node.right)
        if isinstance(node.op, ast.Add):
            return left + right
        if isinstance(node.op, ast.Sub):
            return left - right
        if isinstance(node.op, ast.Mult):
            return left * right
        require(right != 0, f'zero divisor in integer expression: {expression}')
        quotient = abs(left) // abs(right)
        if (left < 0) != (right < 0):
            quotient = -quotient
        return quotient if isinstance(node.op, ast.Div) else left - quotient * right

    return visit(tree)


def book_assignments(cpp: str, indices: list[int], count: int) -> dict[int, int]:
    """Evaluate the parsed selector, not an expected membership table."""
    selectors = re.findall(r'^int book_slice\(int index\) \{(.*?)^\}',
                           cpp, re.MULTILINE | re.DOTALL)
    require(len(selectors) == 1, 'expected one book slice selector')
    body = code(selectors[0])
    simple = re.fullmatch(r'return([^;]+);', body)
    if simple is not None:
        # Retain the previous selector grammar for meaningful reversion controls.
        return {index: integer_value(simple[1], dict(index=index, kSliceCount=count))
                for index in indices}

    dealing = re.fullmatch(
        r'intordinary_rank=([^;]+);'
        r'for\(std::size_theavy=0;heavy<kHeavyBooks.size\(\);\+\+heavy\)\{'
        r'if\(index==kHeavyBooks\[heavy\]\)\{'
        r'if\(heavy==0\)return([^;]+);return([^;]+);\}'
        r'if\(kHeavyBooks\[heavy\]<index\)(--|\+\+)ordinary_rank;'
        r'\}return([^;]+);', body)
    require(dealing is not None, 'unsupported book slice dealing rule')
    arrays = {name: tuple(int(value) for value in array_values(cpp, name))
              for name in ('kHeavyBooks', 'kOrdinarySliceCycle')}
    sizes = {f'{name}.size()': len(array) for name, array in arrays.items()}
    assignments = {}
    for index in indices:
        values = dict(sizes, index=index, kSliceCount=count)
        rank = integer_value(dealing[1], values, arrays)
        for heavy, book in enumerate(arrays['kHeavyBooks']):
            values.update(heavy=heavy, ordinary_rank=rank)
            if index == book:
                expression = dealing[2] if heavy == 0 else dealing[3]
                assignments[index] = integer_value(expression, values, arrays)
                break
            if book < index:
                rank += -1 if dealing[4] == '--' else 1
        else:
            values['ordinary_rank'] = rank
            assignments[index] = integer_value(dealing[5], values, arrays)
    return assignments


def main_selection(cpp: str, indices: list[int], selector: int, count: int) -> list[int]:
    """Read the loop predicate, including the original index-modulo form."""
    source = code(cpp)
    loop = re.search(
        r'for\(constauto&book:books\)\{if\((slice.count!=1&&)?'
        r'(.+?)!=(slice.index(?:-\d+)?)\)continue;'
        r'run_one\(book.config,book.window\);\+\+runs;', source)
    require(loop is not None, 'book loop must run each selected case once')
    if loop[1] and count == 1:
        return list(indices)
    expression = loop[2]
    assignments = {}
    if 'book_slice(book.index)' in expression:
        assignments = book_assignments(cpp, indices, count)
        expression = expression.replace('book_slice(book.index)', 'assigned_slice')
    selected = []
    for index in indices:
        values = {'index': index, 'book.index': index, 'slice.index': selector,
                  'slice.count': count, 'kSliceCount': count}
        if index in assignments:
            values['assigned_slice'] = assignments[index]
        if integer_value(expression, values) == integer_value(loop[3], values):
            selected.append(index)
    return selected


def collect_cover(cpp: str, cmake: str) -> dict[str, list[int]]:
    """Read real table constants and literal registrations; reject drift."""
    helper = re.search(
        r'function\(pineforge_add_unit_test test_name\)(.*?)endfunction\(\)',
        cmake, re.DOTALL)
    require(helper is not None, 'unit-test target builder is missing')
    branch = re.search(
        rf'if\(test_name STREQUAL "{PREFIX}"\)(.*?)'
        r'elseif\(NOT test_name STREQUAL "test_l4g_runtime_budget"'
        r'(?:\s+AND\s+NOT\s+test_name\s+STREQUAL\s+"[^"]+")*\s*\)',
        helper[1], re.DOTALL)
    require(branch is not None,
            'serial registration remains: expected eight slice rows in the target builder')
    rows = re.findall(
        rf'add_test\(NAME ({PREFIX}_slice_\d+)\s+'
        r'COMMAND \$\{test_name\} --slice (\d+)/(\d+)\)', branch[1])
    expected_names = [f'{PREFIX}_slice_{index}' for index in range(1, 9)]
    require([name for name, _, _ in rows] == expected_names,
            'registered slice names must be exactly slices 1 through 8')
    require(re.findall(rf'add_test\(NAME ({PREFIX}_slice_\d+)\b', cmake)
            == expected_names, 'slice registrations must occur once in the target builder')
    require(not re.search(rf'add_test\(NAME {PREFIX}\s', cmake),
            'old serial CTest name must be absent')
    require(code(helper[1]).count('add_executable(${test_name}${test_name}.cpp)') == 1
            and not re.search(rf'add_executable\({PREFIX}\b', cmake),
            'slices must share one executable build')
    require(branch[1].count(
        f'add_test(NAME {PREFIX}_slices_cover\n'
        '            COMMAND ${Python3_EXECUTABLE}\n'
        '                ${PROJECT_SOURCE_DIR}/scripts/test_native_command_after_split.py)') == 1,
        'public cover row must use the collection-only script in the same branch')
    slow_block = re.search(r'^set\(PINEFORGE_PR_SLOW_TESTS\n(.*?)^\)',
                           cmake, re.MULTILINE | re.DOTALL)
    require(slow_block is not None, 'slow label list is missing')
    family = re.findall(rf'^    ({PREFIX}(?:_[A-Za-z0-9_]+)?)$',
                        slow_block[1], re.MULTILINE)
    require(family == expected_names,
            'only the eight slices must inherit exactly the old slow population')
    require(not re.search(rf'set_tests_properties\({PREFIX}(?:_slice_\d+|_slices_cover)'
                          r'[^)]*\b(?:SKIP_RETURN_CODE|DISABLED|TIMEOUT|ENVIRONMENT)\b',
                          cmake, re.DOTALL),
            'split rows must not gain skip, timeout, or environment overrides')

    count = constant(cpp, 'kSliceCount')
    require(count == 8, 'slice count must remain eight')
    axis_names = ('kLiveBooks', 'kPaths', 'kWindows', 'kCalculateOnFills')
    axes = tuple(array_values(cpp, name) for name in axis_names)
    sizes = {f'{name}.size()': len(axis) for name, axis in zip(axis_names, axes)}
    cases = list(product(*axes))
    require(len(cases) == len(set(cases)), 'duplicate case in the book table')
    require(axes == (('1', '5', '20', '60'),
                     ('Path::None', 'Path::Synthesized', 'Path::Lower'),
                     ('Window::Full', 'Window::Acknowledged', 'Window::EveryPoint'),
                     ('false', 'true')), 'legacy book axes or order changed')
    seed_base = constant(cpp, 'kSeedBase')
    require(seed_base == 44001, 'legacy seed base changed')
    builder = re.search(r'std::vector<BookCase> book_cases\(\) \{(.*?)^\}',
                        cpp, re.MULTILINE | re.DOTALL)
    require(builder is not None, 'book table builder is missing')
    builder_code = code(builder[1])
    require(re.findall(r'\bbooks\.(\w+)\(', builder_code) == ['push_back']
            and builder_code.count('for(') == len(axes)
            and builder_code.count('return') == 1
            and all(token not in builder_code for token in ('if(', 'continue;', 'break;')),
            'book enumeration must append every case exactly once without filtering')

    source = code(cpp)
    loops = (
            'for (std::size_t live = 0; live < kLiveBooks.size(); ++live)',
            'for (std::size_t path = 0; path < kPaths.size(); ++path)',
            'for (std::size_t window = 0; window < kWindows.size(); ++window)',
            'for (std::size_t fill = 0; fill < kCalculateOnFills.size(); ++fill)')
    require(builder_code.startswith('std::vector<BookCase>books;'
                                    + ''.join(code(loop) + '{' for loop in loops)),
            'book enumeration must traverse every table axis in legacy order')
    index_formulas = re.findall(r'constintindex=static_cast<int>\(([^;]+)\);',
                               builder_code)
    require(len(index_formulas) == 1, 'expected one legacy index formula')
    for statement in (
            'config.seed = kSeedBase + index;',
            'config.live = kLiveBooks[live];',
            'config.bars = config.live >= 20 ? 30 : 50;',
            'config.path = kPaths[path];',
            'config.calc_on_fills = kCalculateOnFills[fill];',
            'books.push_back({index, config, kWindows[window]});',
            'Slice slice;',
            'int index = 1;',
            'int count = 1;',
            'int main(int argc, char** argv)',
            'if (argc != 1 && (argc != 3 || std::strcmp(argv[1], "--slice") != 0 || '
            '!parse_slice(argv[2], slice)))',
            'const auto index = std::from_chars(argument, end, slice.index);',
            "if (index.ec != std::errc{} || index.ptr == end || *index.ptr != '/') return false;",
            'const auto count = std::from_chars(index.ptr + 1, end, slice.count);',
            'return count.ec == std::errc{} && count.ptr == end && '
            'slice.count == kSliceCount && slice.index >= 1 && slice.index <= slice.count;',
            'slice.count == kSliceCount && slice.index >= 1 && slice.index <= slice.count',
            'const auto books = book_cases();',
            'const int slice_books = slice_book_count(slice);',
            'for (const auto& book : books) {'
            'if (slice.count != 1 && book_slice(book.index) != slice.index) continue;'
            'run_one(book.config, book.window); ++runs;',
            'CHECK(runs > 0);',
            'CHECK(runs == slice_books);',
            'if (slice.count == 1) {'
            'CHECK(answered_true > 10000);'
            'CHECK(answered_false > 10000);'
            '} else {'
            'CHECK(answered_true > 10000 * slice_books / static_cast<int>(books.size()));'
            'CHECK(answered_false > 10000 * slice_books / static_cast<int>(books.size()));'
            '}',
            'CHECK(empty_journal > 0);'):
        require(code(statement) in source, f'case execution contract changed: {statement}')

    # Recover the actual indices by evaluating the formula over parsed axis sizes.
    indices = []
    seeds = []
    for live, path, window, fill in product(*(range(len(axis)) for axis in axes)):
        index = integer_value(index_formulas[0], dict(
            sizes, live=live, path=path, window=window, fill=fill))
        indices.append(index)
        seeds.append(seed_base + index)
    require(indices == list(range(len(cases))),
            'legacy index enumeration must cover every book exactly once in order')
    require(seeds == list(range(seed_base, seed_base + len(cases))),
            'legacy seeds must remain consecutive in nested-loop order')

    assignments = book_assignments(cpp, indices, count)

    counter = re.search(r'int slice_book_count\(const Slice& slice\) \{(.*?)^\}',
                        cpp, re.MULTILINE | re.DOTALL)
    require(counter is not None, 'slice book count helper is missing')
    counter_code = code(counter[1])
    population = re.search(r'constintbooks=static_cast<int>\(([^;]+)\);', counter_code)
    require(population is not None, 'slice count helper population formula is missing')
    require(integer_value(population[1], sizes) == len(cases),
            'slice count helper must enumerate the complete book population')
    require(counter_code == 'intcount=0;' + population[0] + code(
        'for (int index = 0; index < books; ++index) {'
        'if (slice.count == 1 || book_slice(index) == slice.index) ++count;'
        '} return count;'),
        'slice count helper must count each selected index once')

    selections = {}
    for name, index_text, count_text in rows:
        index, divisor = int(index_text), int(count_text)
        require(divisor == count, f'slice denominator differs from table: {name}')
        selected = main_selection(cpp, indices, index, divisor)
        require(bool(selected) and 1 <= index <= divisor, f'empty or out-of-range slice: {name}')
        selections[name] = selected
    multiplicities = Counter(case for selected in selections.values() for case in selected)
    require(all(times == 1 for times in multiplicities.values()), 'slice assignments overlap')
    require(set(multiplicities) == set(indices), 'slice union loses books')
    require(selections[f'{PREFIX}_slice_{count}'] == [67],
            'slice 8 must contain only book 67')

    heavy = tuple(int(value) for value in array_values(cpp, 'kHeavyBooks'))
    cycle = tuple(int(value) for value in array_values(cpp, 'kOrdinarySliceCycle'))
    require(heavy == (67, 49, 61, 55), 'measured heavy book table changed')
    require(cycle == tuple(range(4, count)) + tuple(range(1, 4)),
            'ordinary slice cycle must start at four and visit slices 1 through 7')
    expected_assignments = {heavy[0]: count}
    expected_assignments.update({book: selector
                                 for selector, book in enumerate(heavy[1:], 1)})
    ordinary = [index for index in indices if index not in heavy]
    expected_assignments.update({index: cycle[rank % len(cycle)]
                                 for rank, index in enumerate(ordinary)})
    require(assignments == expected_assignments,
            'slice rule must deal heavy books first and ordinary ranks from slice four')
    expected_counts = Counter(expected_assignments.values())
    require(all(len(selected) == expected_counts[int(name.rsplit('_', 1)[1])]
                for name, selected in selections.items()),
            'slice book counts differ')
    require(all(int(name.rsplit('_', 1)[1]) == int(index) for name, index, _ in rows),
            'slice names must match their one-based selectors')
    expected_costs = {f'{PREFIX}_slice_{expected_assignments[heavy[0]]}': 600}
    expected_costs.update({f'{PREFIX}_slice_{expected_assignments[book]}': 60
                           for book in heavy[1:]})
    require(slice_costs(cmake) == expected_costs,
            'slice COST hints must be 600 for slice 8 and 60 for slices 1 through 3')
    return selections


def cover_error(cpp: str, cmake: str) -> str | None:
    try:
        collect_cover(cpp, cmake)
    except ValueError as error:
        return str(error)
    return None


class CommandAfterSplitCover(unittest.TestCase):
    def test_heavy_book_dealing(self):
        cpp, _ = read_sources()
        indices = list(range(72))
        count = constant(cpp, 'kSliceCount')
        for book, expected in ((67, 8), (49, 1), (61, 2), (55, 3)):
            with self.subTest(book=book):
                containing_slices = [
                    selector for selector in range(1, count + 1)
                    if book in main_selection(cpp, indices, selector, count)]
                self.assertEqual(containing_slices, [expected])

    def test_long_slice_contains_only_book_67(self):
        cpp, _ = read_sources()
        count = constant(cpp, 'kSliceCount')
        self.assertEqual(main_selection(cpp, list(range(72)), count, count), [67])

    def test_ordinary_books_continue_from_slice_four(self):
        cpp, _ = read_sources()
        axes = tuple(array_values(cpp, name) for name in
                     ('kLiveBooks', 'kPaths', 'kWindows', 'kCalculateOnFills'))
        indices = list(range(len(list(product(*axes)))))
        count = constant(cpp, 'kSliceCount')
        ordinary = [index for index in indices if index not in (67, 49, 61, 55)]
        for selector in range(1, count + 1):
            with self.subTest(slice=selector):
                selected = main_selection(cpp, ordinary, selector, count)
                expected = [index for rank, index in enumerate(ordinary)
                            if (rank + 3) % (count - 1) + 1 == selector]
                self.assertEqual(selected, expected)
        self.assertEqual(main_selection(cpp, indices, 1, 1), indices)

    def test_slice_cost_hints(self):
        _, cmake = read_sources()
        expected = {f'{PREFIX}_slice_{selector}': cost
                    for selector, cost in ((1, 60), (2, 60), (3, 60), (8, 600))}
        self.assertEqual(slice_costs(cmake), expected)

    def test_target_builder_branch_allows_additional_literal_exclusions(self):
        cpp, cmake = read_sources()
        self.assertIsNone(cover_error(cpp, cmake))
        conditions = re.findall(
            r'elseif\(NOT test_name STREQUAL "test_l4g_runtime_budget"[^)]*\)',
            cmake)
        self.assertEqual(len(conditions), 1)
        row = (f'        add_test(NAME {PREFIX}_slice_8\n'
               '            COMMAND ${test_name} --slice 8/8)\n')
        self.assertIn(row, cmake)
        for condition in (
                'elseif(NOT test_name STREQUAL "test_l4g_runtime_budget")',
                conditions[0][:-1]
                + '\n            AND NOT test_name STREQUAL "another_program")'):
            with self.subTest(condition=condition):
                changed = cmake.replace(conditions[0], condition, 1)
                self.assertIsNone(cover_error(cpp, changed))
                error = cover_error(cpp, changed.replace(row, '', 1))
                self.assertIsNotNone(error)
                self.assertIn('registered slice names', error)

    def test_cover_and_mutation_controls(self):
        cpp, cmake = read_sources()
        self.assertIsNone(cover_error(cpp, cmake))
        selections = collect_cover(cpp, cmake)
        print('Collected books per slice: ' + ', '.join(
            f'{name}={len(selected)}' for name, selected in selections.items()), flush=True)
        for name, selected in selections.items():
            print(f'{name}: {selected}', flush=True)
        row = (f'        add_test(NAME {PREFIX}_slice_8\n'
               '            COMMAND ${test_name} --slice 8/8)\n')
        self.assertIn(row, cmake)

        def replace_slice_8(argument):
            return cmake.replace(
                row, row.replace('--slice 8/8', argument, 1), 1)

        selector = re.search(r'^int book_slice\(int index\) \{.*?^\}',
                             cpp, re.MULTILINE | re.DOTALL)
        self.assertIsNotNone(selector)
        ordinary_return = (
            'return kOrdinarySliceCycle[ordinary_rank % kOrdinarySliceCycle.size()];')
        heavy_return = 'if (heavy == 0) return kSliceCount;'
        dealing_error = (
            'slice rule must deal heavy books first and ordinary ranks from slice four')
        cost_error = (
            'slice COST hints must be 600 for slice 8 and 60 for slices 1 through 3')
        mutations = (
            ('drop a slice', cpp, cmake.replace(row, '', 1),
             'registered slice names'),
            ('duplicate a case',
             cpp.replace('Window::Acknowledged, Window::EveryPoint',
                         'Window::Acknowledged, Window::Acknowledged', 1), cmake,
             'duplicate case'),
            ('append a case twice',
             cpp.replace('books.push_back({index, config, kWindows[window]});',
                         'books.push_back({index, config, kWindows[window]});'
                         'books.push_back({index, config, kWindows[window]});', 1),
             cmake, 'book enumeration must append every case exactly once'),
            ('overlap two slices', cpp, replace_slice_8('--slice 7/8'),
             'slice assignments overlap'),
            ('shrink the slice count', cpp.replace('kSliceCount = 8;', 'kSliceCount = 7;', 1),
             cmake, 'slice count must remain eight'),
            ('restore the fill-skewed rule',
             cpp.replace(selector[0], 'int book_slice(int index) {\n'
                         '    return index % kSliceCount + 1;\n}', 1), cmake,
             'slice 8 must contain only book 67'),
            ('restore the pair-rank rule',
             cpp.replace(selector[0], 'int book_slice(int index) {\n'
                         '    return (index / 2) % kSliceCount + 1;\n}', 1), cmake,
             'slice 8 must contain only book 67'),
            ('move book 67 into another slice',
             cpp.replace(heavy_return, 'if (heavy == 0) return kSliceCount - 1;', 1),
             cmake, 'empty or out-of-range slice'),
            ('drop book 67',
             cpp.replace(heavy_return, 'if (heavy == 0) return 0;', 1),
             cmake, 'empty or out-of-range slice'),
            ('empty slice seven',
             cpp.replace('kOrdinarySliceCycle = {4, 5, 6, 7, 1, 2, 3};',
                         'kOrdinarySliceCycle = {4, 5, 6, 6, 1, 2, 3};', 1),
             cmake, 'empty or out-of-range slice'),
            ('change the modulus',
             cpp.replace(ordinary_return,
                         'return kOrdinarySliceCycle[ordinary_rank % '
                         '(kOrdinarySliceCycle.size() - 1)];', 1), cmake,
             dealing_error),
            ('restart the ordinary cursor at one',
             cpp.replace('kOrdinarySliceCycle = {4, 5, 6, 7, 1, 2, 3};',
                         'kOrdinarySliceCycle = {1, 2, 3, 4, 5, 6, 7};', 1),
             cmake, 'ordinary slice cycle must start at four'),
            ('count heavy indices in ordinary ranks',
             cpp.replace('--ordinary_rank;', '++ordinary_rank;', 1),
             cmake, dealing_error),
            ('swap the other heavy books',
             cpp.replace('kHeavyBooks = {67, 49, 61, 55};',
                         'kHeavyBooks = {67, 61, 49, 55};', 1),
             cmake, 'measured heavy book table changed'),
            ('change an array length',
             cpp.replace('std::array<int, 4> kLiveBooks',
                         'std::array<int, 3> kLiveBooks', 1), cmake,
             'case-table axis size differs: kLiveBooks'),
            ('remove an array element and resize it',
             cpp.replace('std::array<int, 4> kLiveBooks = {1, 5, 20, 60};',
                         'std::array<int, 3> kLiveBooks = {1, 5, 20};', 1), cmake,
             'legacy book axes or order changed'),
            ('change the legacy index formula',
             cpp.replace('kCalculateOnFills.size() + fill);',
                         'kCalculateOnFills.size() + fill + 1);', 1), cmake,
             'legacy index enumeration must cover every book exactly once in order'),
            ('restore the divided run-count guard',
             cpp.replace('CHECK(runs == slice_books);',
                         'CHECK(runs == static_cast<int>(books.size()) / slice.count);', 1),
             cmake, 'CHECK(runs == slice_books);'),
            ('double the expected book count',
             cpp.replace('book_slice(index) == slice.index) ++count;',
                         'book_slice(index) == slice.index) count += 2;', 1), cmake,
             'slice count helper must count each selected index once'),
            ('zero-case slice', cpp, replace_slice_8('--slice 9/8'),
             'empty or out-of-range slice'),
            ('reseed after selection',
             cpp.replace('config.seed = kSeedBase + index;', 'config.seed = kSeedBase;', 1),
             cmake, 'config.seed = kSeedBase + index;'),
            ('remove the nonempty guard', cpp.replace('CHECK(runs > 0);', '', 1),
             cmake, 'CHECK(runs > 0);'),
            ('bypass argument validation',
             cpp.replace('!parse_slice(argv[2], slice)', 'false', 1),
             cmake, '!parse_slice(argv[2], slice)'),
        )
        for selector, cost in ((1, 60), (2, 60), (3, 60), (8, 600)):
            hint = f'set_tests_properties({PREFIX}_slice_{selector} PROPERTIES COST {cost})'
            self.assertIn(hint, cmake)
            mutations += ((f'change slice {selector} COST', cpp,
                           cmake.replace(hint, hint.replace(f'COST {cost}',
                                                           f'COST {cost - 1}'), 1),
                           cost_error),)
        for counter in ('answered_true', 'answered_false', 'empty_journal'):
            guard = (f'CHECK({counter} > 0);' if counter == 'empty_journal'
                     else f'CHECK({counter} > 10000 * slice_books / '
                     'static_cast<int>(books.size()));')
            mutations += ((f'remove {counter} guard', cpp.replace(guard, '', 1),
                           cmake, guard),)
            if counter != 'empty_journal':
                guard = f'CHECK({counter} > 10000);'
                mutations += ((f'remove direct-run {counter} guard',
                               cpp.replace(guard, '', 1), cmake, guard),)
        for name, changed_cpp, changed_cmake, expected in mutations:
            with self.subTest(mutation=name):
                self.assertNotEqual((changed_cpp, changed_cmake), (cpp, cmake))
                error = cover_error(changed_cpp, changed_cmake)
                self.assertIsNotNone(error)
                self.assertIn(expected, error)


if __name__ == '__main__':
    unittest.main()
