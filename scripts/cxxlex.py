#!/usr/bin/env python3
"""Split C/C++ lines into code, comment and literal text for source guards.
Handles // and /* */ comments, "..." strings, '.' chars, raw strings R"d(...)d",
digit separators (1'000), backslash-newline continuation of // comments."""
IDENT = set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_")

def lex_lines(text):
    lines = text.split("\n")
    out = []
    state = "N"          # N normal, LC line comment, BC block comment, S string, C char, R raw
    raw_end = ""
    for line in lines:
        code, com, lit = [], [], []
        i, n = 0, len(line)
        if state == "LC":            # continued line comment (previous ended with backslash)
            com.append(line); i = n
        while i < n:
            c = line[i]
            if state == "N":
                if line.startswith("//", i):
                    state = "LC"; com.append(line[i:]); i = n; break
                if line.startswith("/*", i):
                    state = "BC"; com.append("/*"); i += 2; continue
                if c == '"':
                    # raw string?
                    k = i - 1
                    while k >= 0 and line[k] in IDENT: k -= 1
                    prefix = line[k+1:i]
                    if prefix in ("R", "u8R", "uR", "UR", "LR"):
                        p = line.find("(", i+1)
                        delim = line[i+1:p] if p >= 0 else ""
                        raw_end = ")" + delim + '"'
                        state = "R"; i = p + 1 if p >= 0 else n; continue
                    state = "S"; i += 1; continue
                if c == "'":
                    k = i - 1
                    while k >= 0 and line[k] in IDENT: k -= 1
                    if line[k+1:i][:1].isdigit():
                        code.append(c); i += 1; continue   # digit separator (1'000)
                    state = "C"; i += 1; continue
                code.append(c); i += 1; continue
            if state == "BC":
                j = line.find("*/", i)
                if j < 0:
                    com.append(line[i:]); i = n; break
                com.append(line[i:j+2]); i = j + 2; state = "N"; continue
            if state == "S":
                if c == "\\":
                    lit.append(line[i:i+2]); i += 2; continue
                if c == '"':
                    state = "N"; lit.append(" "); i += 1; continue
                lit.append(c); i += 1; continue
            if state == "C":
                if c == "\\":
                    i += 2; continue
                if c == "'":
                    state = "N"; i += 1; continue
                i += 1; continue
            if state == "R":
                j = line.find(raw_end, i)
                if j < 0:
                    lit.append(line[i:]); i = n; break
                lit.append(line[i:j]); i = j + len(raw_end); state = "N"; continue
        # end of line
        if state == "LC":
            state = "LC" if line.endswith("\\") else "N"
        if state in ("S", "C"):
            state = "N"                # unterminated on this line: be forgiving
        out.append(("".join(code), " ".join(com), "".join(lit)))
    return out
