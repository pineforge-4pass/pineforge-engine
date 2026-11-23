#include <cerrno>
#include <cstring>
#include <iostream>
#include <unistd.h>

namespace {

// Run the issue's one-line repro against the configured checkout's source.
// No module build, market data or TradingView reference is needed.
constexpr char kCheck[] = R"PY(
import json
from pathlib import Path
import runpy
import sys

source = Path(sys.argv[1])
sys.path.insert(0, str(source.parent))
try:
    convert = runpy.run_path(str(source)).get("corpus_input_values")
except (Exception, SystemExit) as error:
    sys.exit(f"NOT COMPARED: cannot load the runner; "
             f"expected corpus_input_values from {source}, got {type(error).__name__}")
if not callable(convert):
    sys.exit(f"NOT COMPARED: missing input converter; "
             f"expected corpus_input_values from {source}, got {convert!r}")

for raw, requested, token in (
    ('{"Long only": true}', True, "true"),
    ('{"Long only": false}', False, "false"),
):
    expected_input = {"Long only": requested}
    try:
        params = json.loads(raw)
    except json.JSONDecodeError:
        sys.exit(f"NOT COMPARED: invalid issue #920 JSON; "
                 f"expected {expected_input!r}, got {raw!r}")
    if params != expected_input or type(params["Long only"]) is not bool:
        sys.exit(f"NOT COMPARED: wrong issue #920 input; "
                 f"expected {expected_input!r}, got {params!r}")

    expected = {"Long only": token}
    actual = convert(params)
    if actual != expected:
        sys.exit(f"FAIL: inputs.json {raw}: expected {expected!r}, got {actual!r}")

print("PASS: Long only JSON booleans convert to 'true' and 'false'")
)PY";

}  // namespace

int main() {
    ::execl(PINEFORGE_ISSUE_920_PYTHON, PINEFORGE_ISSUE_920_PYTHON, "-I", "-B",
            "-c", kCheck, PINEFORGE_ISSUE_920_RUNNER, static_cast<char*>(nullptr));
    const int error = errno;
    std::cerr << "NOT COMPARED: cannot start the interpreter; expected "
              << PINEFORGE_ISSUE_920_PYTHON << ", got " << std::strerror(error) << '\n';
    return 1;
}
