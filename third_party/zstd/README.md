# Vendored zstd 1.5.7 (BSD-3-Clause)

Upstream: https://github.com/facebook/zstd, release tag v1.5.7.
Tarball: zstd-1.5.7.tar.gz, sha256 eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3
Verified on 2026-10-19T23:40:17Z: the zstd-1.5.7.tar.gz.sha256 file published in the release equals the tarball's sha256; the GitHub release API records NO sha256 digest for this asset, so only the published .sha256 file was checked.
Licence: the BSD-3-Clause text in LICENSE (zstd is dual BSD-3-Clause / GPL-2.0; this tree uses the BSD-3-Clause option only; COPYING (GPL-2.0) is not vendored).
Vendored: lib/common, lib/compress, lib/decompress (sources and headers) and lib/zstd.h, lib/zstd_errors.h, unmodified. Dictionary builder, legacy decoders, programs and tests are not vendored.
MANIFEST.sha256 lists every vendored file's sha256 (relative paths, sorted bytewise).
To update: change the tag and the checks in the vendoring kit, never edit a file here by hand.
