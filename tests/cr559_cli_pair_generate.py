"""Worker-only driver of the real gate/glue.py transpile_json interface."""

import argparse
import hashlib
import json
from pathlib import Path
import sys


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--codegen", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--artifacts", type=Path, required=True)
    args = parser.parse_args()
    codegen = args.codegen.resolve()
    sys.path.insert(0, str(codegen))
    from gate import glue
    import pineforge_codegen
    if (Path(glue.__file__).resolve() != codegen / "gate/glue.py" or
            Path(pineforge_codegen.__file__).resolve() !=
            codegen / "pineforge_codegen/__init__.py"):
        raise RuntimeError("generator import escaped the declared archive")
    source = args.fixture / "probe.pine"
    raw = glue.transpile_json(source.read_text(encoding="utf-8"))
    (args.artifacts / "generation-response.json").write_text(raw, encoding="utf-8")
    print(raw, flush=True)
    response = json.loads(raw)
    (args.artifacts / "generation-diagnostics.json").write_text(
        json.dumps(response.get("diagnostics"), indent=2) + "\n", encoding="utf-8")
    if response.get("ok") is not True or not response.get("cpp", "").strip():
        raise RuntimeError("real transpile_json did not return a translation unit")
    if any(item.get("severity") == "error" for item in response["diagnostics"]):
        raise RuntimeError("real generation returned error diagnostics")
    generated = args.fixture / "generated.cpp"
    # Only the generator response is allowed to populate this reserved file.
    generated.write_text(response["cpp"], encoding="utf-8")
    (args.artifacts / "generation.json").write_text(json.dumps({
        "interface": "gate/glue.py:transpile_json",
        "generator_argv": sys.argv, "python_executable": sys.executable,
        "python_version": sys.version, "codegen_version": (codegen / "VERSION").read_text().strip(),
        "glue_sha256": sha(codegen / "gate/glue.py"),
        "codegen_init_sha256": sha(codegen / "pineforge_codegen/__init__.py"),
        "pine_sha256": sha(source), "generated_cpp_sha256": sha(generated),
        "response_sha256": sha(args.artifacts / "generation-response.json"),
        "diagnostics_sha256": sha(args.artifacts / "generation-diagnostics.json"),
    }, sort_keys=True, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
