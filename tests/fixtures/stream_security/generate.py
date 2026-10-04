import argparse
import pathlib
import sys


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--codegen-dir", required=True, type=pathlib.Path)
    args = parser.parse_args()
    sys.path.insert(0, str(args.codegen_dir.resolve()))
    from pineforge_codegen import transpile

    for source in sorted(pathlib.Path(__file__).parent.glob("*/strategy.pine")):
        source.with_name("generated.cpp").write_text(transpile(source.read_text()))


if __name__ == "__main__":
    main()
