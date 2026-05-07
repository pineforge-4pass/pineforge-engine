# Legal information

This file summarizes how licensing, third-party components, and trademarks apply to this repository. It is **not** legal advice; consult counsel for your use case.

## SPDX

The PineForge **runtime** (C/C++ sources under `src/`, `include/`, `tests/`, `cmake/`, and related build scripts shipped here) is distributed under:

**SPDX-License-Identifier: Apache-2.0**

See [LICENSE](LICENSE) for the full Apache License 2.0 text.

Optional **private** git submodules (`corpus/`, `benchmarks/assets/`) are separate works maintained outside the public tree. Their contents are **not** licensed to the public by virtue of this repo alone; access is governed by those repositories and your agreement with their owners. Each private repo should carry its own legal notice.

Historical cleanup for private fixture blobs is handled by maintainer-internal release documentation, not by scripts in this public repository.

## Third-party components linked into `libpineforge`

| Component | Usage | License |
| --- | --- | --- |
| [Eigen](https://eigen.tuxfamily.org/) | Matrix-typed PineScript (`matrix.*`); linked as `Eigen3::Eigen` | [MPL-2.0](https://www.mozilla.org/en-US/MPL/2.0/) |

MPL-2.0 applies to Eigen source files as used in your build. See [NOTICE](NOTICE) for attribution required by Apache-2.0 § 4(d).

System packages (e.g. Eigen from `apt` or Homebrew) remain under their upstream licenses.

## Optional benchmark harness (`benchmarks/`)

The **library** build and CI do **not** require Node or PyneCore. The benchmark directory is an **optional** tooling tree.

If you run `bash benchmarks/run_all.sh`, `npm install`, or install the Python dependencies in `benchmarks/pyproject.toml`, you pull in additional packages. You must comply with **each** upstream license. Examples (verify versions on your machine):

| Dependency area | Typical packages | Upstream license (check exact version) |
| --- | --- | --- |
| Node | [PineTS / `pinets`](https://github.com/LuxAlgo/PineTS) | **AGPL-3.0** (and dependencies) |
| Python | [PyneCore](https://github.com/PyneSys/pynecore) | Apache-2.0 (typical; confirm with PyneSys) |
| Python | `pandas`, `numpy`, `ccxt`, etc. | BSD/MIT/Apache-style (per package metadata) |

**AGPL-3.0** can impose copyleft obligations when you modify AGPL-covered software and distribute or provide network access to it. The PineForge runtime itself is **not** AGPL; only your use of optional benchmark dependencies may trigger AGPL terms.

Do not redistribute TradingView-linked CSV fixtures from private submodules (see [CONTRIBUTING.md](CONTRIBUTING.md)) unless you have the right to do so.

## Trademarks and affiliation

**TradingView** and **PineScript** are trademarks of their respective owners. **PineForge** is a project name used here for this runtime; it is not affiliated with, endorsed by, or sponsored by TradingView. Any statement about “parity” or comparison to TradingView chart behaviour describes technical testing practices only, not a partnership or certification.

Other names (e.g. **PyneCore**, **PineTS**, **Eigen**) belong to their respective projects.

## Patents

Apache 2.0 includes § 3 (patent grant) and termination on litigation. That applies to **contributions** under this license as described in the LICENSE file. It does **not** guarantee freedom from third-party patents for every use of the code.

## Contributing

By contributing to this repository, you agree your contributions are licensed under Apache-2.0, as described in [CONTRIBUTING.md](CONTRIBUTING.md).

## Security and sensitive data

See [SECURITY.md](SECURITY.md). Validation data tied to TradingView exports is kept in **private** submodules by design.
