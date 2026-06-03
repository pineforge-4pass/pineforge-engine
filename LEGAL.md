# Legal information

This file summarizes how licensing, third-party components, and trademarks apply to this repository. It is **not** legal advice; consult counsel for your use case.

## SPDX

The PineForge **runtime** (C/C++ sources under `src/`, `include/`, `tests/`, `cmake/`, and related build scripts shipped here) is distributed under:

**SPDX-License-Identifier: Apache-2.0**

See [LICENSE](LICENSE) for the full Apache License 2.0 text.

The `corpus/` and `benchmarks/assets/` git submodules are **public** companion repositories, also under **Apache-2.0** for the PineForge-authored material they contain. Each carries its own `LEGAL.md`/`NOTICE`; the per-file scope notes there control (in particular, the TradingView trade-list exports described below are *not* PineForge-licensed works — see "TradingView trade-list exports").

## The transpiler is a separate, source-available repository

The PineScript → C++ transpiler ships as **[`pineforge-codegen`](https://github.com/pineforge-4pass/pineforge-codegen-oss)**, **source-available** under the **PolyForm Noncommercial License 1.0.0** (free for personal trading; commercial license for funds, products, and hosted/embedded use). It is **not** part of this Apache-2.0 runtime and is **not required** to reproduce the parity figure — `generated.cpp` ships in the `corpus/` tree, so the engine + corpus + a C++17 compiler are sufficient end-to-end.

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

**AGPL-3.0** can impose copyleft obligations when you modify AGPL-covered software and distribute or provide network access to it. PineTS runs as an **optional, separately-installed comparison engine** and is **not** linked into `libpineforge`; we publish numerical results, not PineTS source. The PineForge runtime itself is **not** AGPL.

## TradingView trade-list exports (`tv_trades.csv`)

The `corpus/` and `benchmarks/assets/` trees include TradingView **"List of Trades"** CSVs. These are produced by running our **own clean-room `strategy.pine`** files inside a TradingView account and exporting the result **manually** — no scraping, no TradingView API, no automated extraction. They are included as **factual parity reference records**.

- **Our position:** the trade records are factual outcomes of our **own** strategies executed against public market data — factual data the author is free to keep and publish (facts are not copyrightable).
- We do **not** Apache-relicense TradingView's export *format/columns* (those are TradingView's, not ours to license), so these files sit in-tree as **factual reference**, not as an Apache-2.0 work of ours.
- The one open item is **contractual**: whether public redistribution of the exports is permitted under TradingView's **Terms of Service** (separate from copyright). This is being confirmed by **US + Taiwan counsel** (opinion targeted Q3 2026).
- Either way, parity does **not depend on shipping them**: `strategy.pine` + `generated.cpp` + public OHLCV regenerate `engine_trades.csv`, which you can diff against a TradingView export you produce yourself. If the reference CSVs are ever removed, the result stays reproducible.

## Trademarks and affiliation

**TradingView** and **PineScript** are trademarks of their respective owners. **PineForge** is the name of this project; it is **not** affiliated with, endorsed by, sponsored by, or certified by TradingView. Use of "PineScript v6", "TradingView", and any "parity" or comparison language is **nominative** — it describes factual compatibility and technical testing only, not a partnership or certification.

Other names (e.g. **PyneCore**, **PineTS**, **Eigen**) belong to their respective projects.

## Patents

Apache 2.0 includes § 3 (patent grant) and termination on litigation. That applies to **contributions** under this license as described in the LICENSE file. It does **not** guarantee freedom from third-party patents for every use of the code.

## Contributing

By contributing to this repository, you agree your contributions are licensed under Apache-2.0, as described in [CONTRIBUTING.md](CONTRIBUTING.md). Contributions are accepted with a Developer Certificate of Origin (`Signed-off-by`) attesting you have the right to submit them under that license.

## No warranty

The runtime is provided **"AS IS"** under Apache-2.0 §§ 7–8, without warranty of any kind. Parity figures describe technical testing on a reference corpus; they are **not** investment advice and are **not** a warranty of trading outcomes.

## Security

See [SECURITY.md](SECURITY.md). Vulnerabilities in the `pineforge-codegen` transpiler, TradingView's platform, or third-party strategy code are out of scope for this runtime's tracker unless they concern this runtime's build or execution of untrusted native code.
