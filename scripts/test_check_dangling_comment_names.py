#!/usr/bin/env python3
"""Must-fail mutations for the dangling comment-name guard."""
from pathlib import Path
import tempfile

from check_dangling_comment_names import findings


def main() -> int:
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        header = root / "include/pineforge/sample.hpp"
        header.parent.mkdir(parents=True)
        header.write_text("// phantom_broker_step() settles this request.\n"
                          "int live_broker_step();\n", encoding="utf-8")
        bad = findings(root)
        assert any("phantom_broker_step" in item for item in bad), bad
        header.write_text("// OrderType::EXIT is the fill branch.\n",
                          encoding="utf-8")
        bad = findings(root)
        assert any("OrderType" in item for item in bad), bad
        header.write_text("// live_broker_step() settles this request.\n"
                          "int live_broker_step();\n", encoding="utf-8")
        assert findings(root) == [], findings(root)
        header.write_text("// historical: phantom_broker_step() was removed.\n",
                          encoding="utf-8")
        assert findings(root) == [], findings(root)
    print("dangling comment-name guard: must-fail mutation detected")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
