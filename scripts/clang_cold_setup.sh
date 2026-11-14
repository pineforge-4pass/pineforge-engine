#!/usr/bin/env bash
# Shared, uncached hosted prerequisites for collection, planning and both shards.
set -euo pipefail
test "$GITHUB_REPOSITORY" = pineforge-4pass/pineforge-engine
case "$GITHUB_EVENT_NAME" in
  push) test "$GITHUB_REF" = refs/heads/ci/clang-cold-shards ;;
  workflow_dispatch) ;;
  *) exit 1 ;;
esac
test "$RUNNER_ENVIRONMENT" = github-hosted
test "$RUNNER_OS" = Linux
test "$RUNNER_ARCH" = X64
test "$(uname -m)" = x86_64
test "$(git rev-parse HEAD)" = "$GITHUB_SHA"
sudo apt-get update
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y \
  clang-18 g++-13 libeigen3-dev tzdata-legacy strace python3-pytest shellcheck
tools_dir="$RUNNER_TEMP/pineforge-clang-actionlint"
mkdir -p "$tools_dir"
archive="$tools_dir/actionlint.tar.gz"
curl --fail --location --proto '=https' --tlsv1.2 \
  https://github.com/rhysd/actionlint/releases/download/v1.7.12/actionlint_1.7.12_linux_amd64.tar.gz \
  --output "$archive"
echo "8aca8db96f1b94770f1b0d72b6dddcb1ebb8123cb3712530b08cc387b349a3d8  $archive" | sha256sum -c -
tar -xzf "$archive" -C "$tools_dir" actionlint
echo "$tools_dir" >> "$GITHUB_PATH"
# Materialize once before any CTest consumers. Both builds use their own checkout.
/usr/bin/python3 scripts/derive_corpus_feeds.py
