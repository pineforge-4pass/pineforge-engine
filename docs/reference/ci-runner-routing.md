The merge ruleset requires `pineforge/verify` and `pineforge/parity` commit
statuses on the PR head's exact tree. The maintainers post them from their own (private)
verification tooling after full verification on their x86-64 build hosts and a
parity verdict; an outside contributor cannot post them. GitHub Actions is advisory: PRs build Ubuntu Release only (`ci.yml`); Debug, sanitizers, kernel-only and the macOS cell run post-merge (`ci-post.yml`).
The PR build (`ci.yml`) runs only the `fast`-labelled CTest rows (`ctest -L '^fast$'`); the post-merge legs (`ci-post.yml`) run
every row, and a documentation-only change runs preflight alone among verification jobs.
The seven PR/post-merge verification and documentation workflows start with the canon `authz` job and every other job in them needs it; release uses its status gate and baseline promotion its `promote-guard`. Pull-request
proof workflows and secret-free post jobs use exact `gke-*` pins in runner group 14. Credentialed release, notification, promotion and probe jobs use `gkp-2cpu-4gb-amd64`; docs deployment uses `gkp-4cpu-8gb-amd64`, all in protected runner group 15. Hosted PR light jobs and `docs.yml`'s `build` retain `ubuntu-24.04`; post guards and aggregates use their exact light GKE pins.
Hosted fork PRs receive no secrets, so they need no VM-only fork-refusal hook. Retired main/post VM labels remain refused. Group 14's mirror is `.github/runner-group-allowlist.txt`; group 15's required refs are in `.github/runner-group-15-allowlist.txt`, with live membership confirmed by the supervisor.
Release's x86_64 prebuilt cell uses `gkp-16cpu-32gb-amd64-128gb`, installs Eigen and asserts exactly 3.4.0 before CMake; the arm64 cell retains hosted `ubuntu-22.04-arm`. App-token jobs keep their exact `release`/`release-bot` environments. The `build-gate` job in `ci-post.yml` and `native-live.yml`'s `native-live-gate` always run and refuse a non-success authz result first.
See `docs/ci.md` for the class table and docs-only exception. The commands below run the full sets.
