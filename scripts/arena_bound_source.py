"""Conservative Pine source inventory for the pre-start facts producer.

This is NOT a Pine compiler or a successful general-purpose bound certifier.
It counts syntactic sites, inventories literal/computed ID arguments and loops,
and recognizes unconditional emitting loops. A finite verdict still requires
the compiler/lowering FACTS record. Unsupported syntax remains UNKNOWN.
"""

from __future__ import annotations

import ast
import re


_TOKEN = re.compile(
    r'//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|'
    r"[A-Za-z_][A-Za-z_0-9]*|[0-9]+|=>|:=|[^\s]")
_ORDER_METHODS = frozenset(
    ("entry", "order", "exit", "close", "close_all", "cancel", "cancel_all"))


def _tokens(text: str) -> list[tuple[str, int]]:
    return [(match.group(), match.start()) for match in _TOKEN.finditer(text)
            if not match.group().startswith("//")]


def _arguments(tokens, opening: int):
    depth = 0
    result = []
    start = opening + 1
    for at in range(opening + 1, len(tokens)):
        word = tokens[at][0]
        if word in ("(", "[", "{"):
            depth += 1
        elif word in (")", "]", "}"):
            if word == ")" and depth == 0:
                result.append(tokens[start:at])
                return result
            depth -= 1
        elif word == "," and depth == 0:
            result.append(tokens[start:at])
            start = at + 1
    return None


def inspect_pine_source(text: str, source: str) -> dict:
    """Return an inventory; a site count is NEVER a per-bar invocation bound."""
    tokens = _tokens(text)
    calls = []
    literals = set()
    computed = []
    for at in range(len(tokens) - 3):
        if (tokens[at][0] != "strategy" or tokens[at + 1][0] != "."
                or tokens[at + 2][0] not in _ORDER_METHODS
                or tokens[at + 3][0] != "("):
            continue
        method = tokens[at + 2][0]
        line = text.count("\n", 0, tokens[at][1]) + 1
        args = _arguments(tokens, at + 3)
        calls.append({"method": method, "line": line})
        if args is None:
            computed.append({"line": line, "argument": "unparsed call"})
            continue
        wanted = ("id", "from_entry") if method == "exit" else (
            () if method in ("close_all", "cancel_all") else ("id",))
        # Named arguments may reorder the first two operands.
        positional = []
        named = {}
        for arg in args:
            words = [word for word, _ in arg]
            if len(words) >= 2 and words[1] == "=":
                named[words[0]] = words[2:]
            else:
                positional.append(words)
        for index, name in enumerate(wanted):
            value = named.get(name, positional[index] if index < len(positional) else [])
            if not value and name == "from_entry":
                literals.add("")
            elif len(value) == 1 and value[0][:1] in ("'", '"'):
                try:
                    literal = ast.literal_eval(value[0])
                except (ValueError, SyntaxError):
                    literal = None
                if isinstance(literal, str):
                    literals.add(literal)
                else:
                    computed.append({"line": line, "argument": name})
            else:
                computed.append({"line": line, "argument": name})

    # Preserve indentation after removing comments/strings from loop headers.
    # A compiler must prove trip counts; constants below are inventory only.
    lines = text.splitlines()
    loops = []
    unconditional = []
    for index, line in enumerate(lines):
        stripped = line.lstrip()
        if not re.match(r"(?:for|while)\b", stripped):
            continue
        indent = len(line) - len(stripped)
        header = stripped.split("//", 1)[0].strip()
        body = []
        for following in lines[index + 1:]:
            if not following.strip() or following.lstrip().startswith("//"):
                continue
            if len(following) - len(following.lstrip()) <= indent:
                break
            body.append(following)
        body_minimum = min((len(row) - len(row.lstrip()) for row in body),
                           default=indent)
        direct_emitter = any(
            len(row) - len(row.lstrip()) == body_minimum
            and re.match(r"strategy\.(?:entry|order|exit|close|close_all)\s*\(",
                         row.lstrip()) for row in body)
        exit_token = any(word in ("break", "return", "continue")
                         for word, _ in _tokens("\n".join(body)))
        body_tokens = _tokens("\n".join(body))
        unknown_call = any(
            body_tokens[at][0] == "("
            and re.fullmatch(r"[A-Za-z_]\w*", body_tokens[at - 1][0])
            and not (at >= 3 and body_tokens[at - 3][0] == "strategy"
                     and body_tokens[at - 2][0] == "."
                     and body_tokens[at - 1][0] in _ORDER_METHODS)
            for at in range(1, len(body_tokens)))
        constant_for = re.fullmatch(
            r"for\s+[A-Za-z_]\w*\s*=\s*(-?\d+)\s+to\s+(-?\d+)"
            r"(?:\s+by\s+(\d+))?", header)
        trips = None
        if constant_for:
            first, last, step = constant_for.groups()
            step_value = int(step or "1")
            if step_value:
                trips = abs(int(last) - int(first)) // step_value + 1
        loops.append({"line": index + 1, "header": header, "constantTrips": trips})
        if (indent == 0 and header in ("while true", "while (true)")
                and direct_emitter and not exit_token and not unknown_call):
            unconditional.append(index + 1)

    # A lexical name graph inventories direct/mutual recursion. It grants no
    # finite verdict: overloads, UDT dispatch and reachability need compiler IR.
    functions = []
    function_bodies = {}
    for index, line in enumerate(lines):
        match = re.match(r"\s*(?:method\s+)?([A-Za-z_]\w*)\s*\([^)]*\)\s*=>", line)
        if match:
            name = match.group(1)
            functions.append({"name": name, "line": index + 1})
            indent = len(line) - len(line.lstrip())
            body = [line[match.end():]]
            for following in lines[index + 1:]:
                if not following.strip() or following.lstrip().startswith("//"):
                    continue
                if len(following) - len(following.lstrip()) <= indent:
                    break
                body.append(following)
            function_bodies.setdefault(name, []).extend(_tokens("\n".join(body)))
    graph = {name: set() for name in function_bodies}
    for name, body in function_bodies.items():
        for at in range(len(body) - 1):
            if body[at][0] in graph and body[at + 1][0] == "(":
                graph[name].add(body[at][0])
    recursive = set()
    for start in graph:
        pending = list(graph[start])
        seen = set()
        while pending:
            called = pending.pop()
            if called == start:
                recursive.add(start)
                break
            if called not in seen:
                seen.add(called)
                pending.extend(graph[called])
    if unconditional:
        control = {
            "status": "unbounded",
            "reason": "unconditional order-emitting while true without an exit at lines "
                      + ",".join(map(str, unconditional)),
            "source": source,
        }
    else:
        control = {
            "status": "unknown",
            "reason": ("source inventory has no certified invocation/lowering/ID closure; "
                       "computed IDs and nonconstant loops require dataflow proofs"),
            "source": source,
        }
    return {
        "orderCallSites": calls,
        "literalIds": sorted(literals),
        "computedIdArguments": computed,
        "loops": loops,
        "functions": functions,
        "recursiveFunctions": sorted(recursive),
        "controlFlow": control,
    }
