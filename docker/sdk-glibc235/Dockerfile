# Build context: six declared inputs prepared by diagnostic.py --prepare-only.
# No checkout metadata, host root store, or externally fetched Dockerfile frontend.
FROM --platform=linux/amd64 docker.io/library/ubuntu:22.04@sha256:8a2321a92b438238f93b4346fff61bc4259c99fdd4d9b52ee86f153fee0c344f AS gcc-build

SHELL ["/bin/bash", "-e", "-u", "-o", "pipefail", "-c"]
ENV LC_ALL=C LANG=C

COPY docker/sdk-glibc235/Dockerfile docker/sdk-glibc235/README.md docker/sdk-glibc235/ubuntu-snapshot.list docker/sdk-glibc235/gcc-13.3.0.sha512 /usr/local/share/sdk-toolchain/inputs/
COPY docker/sdk-glibc235/bootstrap-ca.sha256 docker/sdk-glibc235/ca-certificates_20260816ubuntu0.22.04.1_all.deb /usr/local/share/sdk-toolchain/bootstrap/

WORKDIR /build
RUN . /etc/os-release; \
    [[ "$ID" == ubuntu && "$VERSION_ID" == 22.04 ]]; \
    [[ "$(dpkg --print-architecture)" == amd64 && "$(uname -m)" == x86_64 ]]; \
    [[ "$(getconf GNU_LIBC_VERSION)" == 'glibc 2.35' ]]; \
    [[ -s /usr/share/keyrings/ubuntu-archive-keyring.gpg ]]; \
    mkdir -p /usr/local/share/sdk-toolchain/gcc-build; \
    bootstrap=/usr/local/share/sdk-toolchain/bootstrap; \
    (cd "$bootstrap"; sha256sum --check --strict bootstrap-ca.sha256) \
        | tee /usr/local/share/sdk-toolchain/gcc-build/bootstrap-ca-check.txt; \
    dpkg-deb --extract "$bootstrap/ca-certificates_20260816ubuntu0.22.04.1_all.deb" /build/bootstrap-ca; \
    certs=(/build/bootstrap-ca/usr/share/ca-certificates/mozilla/*.crt); \
    [[ ${#certs[@]} -gt 0 && -f "${certs[0]}" ]]; \
    mkdir -p /etc/ssl/certs; \
    for cert in "${certs[@]}"; do \
        [[ -f "$cert" && ! -L "$cert" ]]; cat "$cert"; printf '\n'; \
    done > /etc/ssl/certs/ca-certificates.crt; \
    [[ -s /etc/ssl/certs/ca-certificates.crt ]]; \
    sha256sum /etc/ssl/certs/ca-certificates.crt \
        > /usr/local/share/sdk-toolchain/gcc-build/bootstrap-bundle.sha256; \
    cp /etc/os-release /usr/local/share/sdk-toolchain/gcc-build/base-os-release; \
    getconf GNU_LIBC_VERSION > /usr/local/share/sdk-toolchain/gcc-build/base-glibc.txt; \
    apt_options=( \
        -o Dir::Etc::sourcelist=/usr/local/share/sdk-toolchain/inputs/ubuntu-snapshot.list \
        -o Dir::Etc::sourceparts=- \
        -o APT::Update::Error-Mode=any \
        -o APT::Get::AllowUnauthenticated=false \
        -o Acquire::AllowInsecureRepositories=false \
        -o Acquire::AllowDowngradeToInsecureRepositories=false \
        -o Acquire::https::Verify-Peer=true \
        -o Acquire::https::Verify-Host=true \
    ); \
    apt-get "${apt_options[@]}" update; \
    DEBIAN_FRONTEND=noninteractive apt-get "${apt_options[@]}" --yes --no-install-recommends install \
        build-essential binutils bison ca-certificates curl file flex gawk \
        libgmp-dev libmpc-dev libmpfr-dev texinfo xz-utils zlib1g-dev; \
    [[ "$(getconf GNU_LIBC_VERSION)" == 'glibc 2.35' ]]; \
    dpkg-query -W -f='${binary:Package}\t${Version}\t${Architecture}\n' \
        > /usr/local/share/sdk-toolchain/gcc-build/installed-packages.tsv; \
    /usr/bin/gcc -v > /usr/local/share/sdk-toolchain/gcc-build/bootstrap-gcc.txt 2>&1; \
    /usr/bin/g++ -v > /usr/local/share/sdk-toolchain/gcc-build/bootstrap-g++.txt 2>&1

RUN curl --fail --location --show-error --silent --proto '=https' --proto-redir '=https' --tlsv1.2 \
        --output gcc-13.3.0.tar.xz \
        https://gcc.gnu.org/pub/gcc/releases/gcc-13.3.0/gcc-13.3.0.tar.xz; \
    sha512sum --check --strict /usr/local/share/sdk-toolchain/inputs/gcc-13.3.0.sha512 \
        | tee /usr/local/share/sdk-toolchain/gcc-build/source-check.txt; \
    sha512sum gcc-13.3.0.tar.xz \
        > /usr/local/share/sdk-toolchain/gcc-build/source-archive.sha512; \
    tar --extract --xz --file gcc-13.3.0.tar.xz --no-same-owner; \
    mkdir /build/gcc-obj

WORKDIR /build/gcc-obj
RUN CC=/usr/bin/gcc CXX=/usr/bin/g++ ../gcc-13.3.0/configure \
        --prefix=/opt/gcc-13.3.0 \
        --enable-languages=c,c++ \
        --enable-bootstrap \
        --disable-multilib \
        --without-isl \
        --with-system-zlib \
        2>&1 | tee /usr/local/share/sdk-toolchain/gcc-build/configure.txt; \
    make -j8 2>&1 | tee /usr/local/share/sdk-toolchain/gcc-build/make.txt; \
    make -j8 install 2>&1 | tee /usr/local/share/sdk-toolchain/gcc-build/install.txt; \
    mkdir -p /usr/local/share/sdk-toolchain/source /usr/local/share/sdk-toolchain/licenses/gcc-13.3.0; \
    cp /build/gcc-13.3.0.tar.xz /usr/local/share/sdk-toolchain/source/; \
    cp /build/gcc-13.3.0/COPYING* /usr/local/share/sdk-toolchain/licenses/gcc-13.3.0/

FROM --platform=linux/amd64 docker.io/library/ubuntu:22.04@sha256:8a2321a92b438238f93b4346fff61bc4259c99fdd4d9b52ee86f153fee0c344f AS sdk-toolchain

SHELL ["/bin/bash", "-e", "-u", "-o", "pipefail", "-c"]
ENV LC_ALL=C LANG=C
COPY --from=gcc-build /usr/local/share/sdk-toolchain/ /usr/local/share/sdk-toolchain/
COPY docker/sdk-glibc235/bootstrap-ca.sha256 docker/sdk-glibc235/ca-certificates_20260816ubuntu0.22.04.1_all.deb /usr/local/share/sdk-toolchain/bootstrap/

RUN . /etc/os-release; \
    [[ "$ID" == ubuntu && "$VERSION_ID" == 22.04 ]]; \
    [[ "$(dpkg --print-architecture)" == amd64 && "$(uname -m)" == x86_64 ]]; \
    [[ "$(getconf GNU_LIBC_VERSION)" == 'glibc 2.35' ]]; \
    [[ -s /usr/share/keyrings/ubuntu-archive-keyring.gpg ]]; \
    bootstrap=/usr/local/share/sdk-toolchain/bootstrap; \
    (cd "$bootstrap"; sha256sum --check --strict bootstrap-ca.sha256) \
        | tee /usr/local/share/sdk-toolchain/bootstrap/final-ca-check.txt; \
    dpkg-deb --extract "$bootstrap/ca-certificates_20260816ubuntu0.22.04.1_all.deb" /bootstrap-ca; \
    certs=(/bootstrap-ca/usr/share/ca-certificates/mozilla/*.crt); \
    [[ ${#certs[@]} -gt 0 && -f "${certs[0]}" ]]; \
    mkdir -p /etc/ssl/certs; \
    for cert in "${certs[@]}"; do \
        [[ -f "$cert" && ! -L "$cert" ]]; cat "$cert"; printf '\n'; \
    done > /etc/ssl/certs/ca-certificates.crt; \
    [[ -s /etc/ssl/certs/ca-certificates.crt ]]; \
    sha256sum /etc/ssl/certs/ca-certificates.crt \
        > /usr/local/share/sdk-toolchain/bootstrap/final-bundle.sha256; \
    apt_options=( \
        -o Dir::Etc::sourcelist=/usr/local/share/sdk-toolchain/inputs/ubuntu-snapshot.list \
        -o Dir::Etc::sourceparts=- \
        -o APT::Update::Error-Mode=any \
        -o APT::Get::AllowUnauthenticated=false \
        -o Acquire::AllowInsecureRepositories=false \
        -o Acquire::AllowDowngradeToInsecureRepositories=false \
        -o Acquire::https::Verify-Peer=true \
        -o Acquire::https::Verify-Host=true \
    ); \
    apt-get "${apt_options[@]}" update; \
    DEBIAN_FRONTEND=noninteractive apt-get "${apt_options[@]}" --yes --no-install-recommends install \
        binutils ca-certificates cmake file git gzip libc6-dev libeigen3-dev \
        libgmp10 libmpc3 libmpfr6 make python3 tar tzdata xz-utils zlib1g

COPY --from=gcc-build /opt/gcc-13.3.0/ /opt/gcc-13.3.0/
ENV PATH=/opt/gcc-13.3.0/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
    CC=/opt/gcc-13.3.0/bin/gcc \
    CXX=/opt/gcc-13.3.0/bin/g++ \
    LD_LIBRARY_PATH=/opt/gcc-13.3.0/lib64:/opt/gcc-13.3.0/lib

RUN provenance=/usr/local/share/sdk-toolchain/image; \
    mkdir "$provenance"; \
    cp /etc/os-release "$provenance/os-release"; \
    dpkg --print-architecture > "$provenance/dpkg-architecture.txt"; \
    uname -m > "$provenance/uname-machine.txt"; \
    getconf GNU_LIBC_VERSION > "$provenance/glibc.txt"; \
    [[ "$(cat "$provenance/dpkg-architecture.txt")" == amd64 ]]; \
    [[ "$(cat "$provenance/uname-machine.txt")" == x86_64 ]]; \
    [[ "$(cat "$provenance/glibc.txt")" == 'glibc 2.35' ]]; \
    dpkg-query -W -f='${binary:Package}\t${Version}\t${Architecture}\n' > "$provenance/installed-packages.tsv"; \
    [[ "$(command -v gcc)" == "$CC" && "$(command -v g++)" == "$CXX" ]]; \
    gcc -dumpfullversion > "$provenance/gcc-version.txt"; \
    g++ -dumpfullversion > "$provenance/g++-version.txt"; \
    [[ "$(cat "$provenance/gcc-version.txt")" == 13.3.0 ]]; \
    [[ "$(cat "$provenance/g++-version.txt")" == 13.3.0 ]]; \
    gcc -dumpmachine > "$provenance/gcc-target.txt"; \
    g++ -dumpmachine > "$provenance/g++-target.txt"; \
    for target_file in "$provenance/gcc-target.txt" "$provenance/g++-target.txt"; do \
        case "$(cat "$target_file")" in \
            x86_64-linux-gnu|x86_64-*-linux-gnu) ;; \
            *) printf 'Unsupported compiler target in %s\n' "$target_file" >&2; exit 1 ;; \
        esac; \
    done; \
    gcc -v > "$provenance/gcc.txt" 2>&1; \
    g++ -v > "$provenance/g++.txt" 2>&1; \
    cmake --version > "$provenance/cmake.txt"; \
    python3 --version > "$provenance/python3.txt" 2>&1; \
    readelf --version > "$provenance/readelf.txt"; \
    macros=/usr/include/eigen3/Eigen/src/Core/util/Macros.h; \
    grep -E '^#define EIGEN_(WORLD|MAJOR|MINOR)_VERSION[[:space:]]' "$macros" \
        > "$provenance/eigen-version-macros.txt"; \
    grep -Eq '^#define EIGEN_WORLD_VERSION[[:space:]]+3[[:space:]]*$' "$provenance/eigen-version-macros.txt"; \
    grep -Eq '^#define EIGEN_MAJOR_VERSION[[:space:]]+4[[:space:]]*$' "$provenance/eigen-version-macros.txt"; \
    grep -Eq '^#define EIGEN_MINOR_VERSION[[:space:]]+0[[:space:]]*$' "$provenance/eigen-version-macros.txt"; \
    [[ -s /usr/share/eigen3/cmake/Eigen3Config.cmake ]]; \
    sha256sum "$macros" /usr/share/eigen3/cmake/Eigen3Config.cmake > "$provenance/eigen-files.sha256"; \
    sha256sum /usr/local/share/sdk-toolchain/inputs/* > "$provenance/input-files.sha256"; \
    cd /usr/local/share/sdk-toolchain/source; \
    sha512sum --check --strict /usr/local/share/sdk-toolchain/inputs/gcc-13.3.0.sha512 \
        > "$provenance/source-check.txt"; \
    cc1=$(gcc -print-prog-name=cc1); \
    cc1plus=$(g++ -print-prog-name=cc1plus); \
    libstdcpp=$(g++ -print-file-name=libstdc++.so.6); \
    libgcc=$(gcc -print-file-name=libgcc_s.so.1); \
    for binary in "$CC" "$CXX" "$cc1" "$cc1plus" "$libstdcpp" "$libgcc"; do \
        binary=$(readlink --canonicalize-existing "$binary"); \
        [[ "$binary" == /opt/gcc-13.3.0/* && -s "$binary" ]]; \
        name=$(basename "$binary"); \
        sha256sum "$binary" >> "$provenance/gcc-elf-files.sha256"; \
        readelf --wide --file-header --program-headers --dynamic --version-info "$binary" \
            > "$provenance/$name.elf.txt"; \
        grep -Eq 'Class:[[:space:]]+ELF64' "$provenance/$name.elf.txt"; \
        grep -Eq 'Machine:[[:space:]]+Advanced Micro Devices X86-64' "$provenance/$name.elf.txt"; \
        ldd "$binary" > "$provenance/$name.ldd.txt" 2>&1; \
        if grep -Fq 'not found' "$provenance/$name.ldd.txt"; then \
            cat "$provenance/$name.ldd.txt" >&2; \
            exit 1; \
        fi; \
        if grep -Eq '^[[:space:]]*libstdc\+\+\.so\.6[[:space:]]+=>' "$provenance/$name.ldd.txt"; then \
            grep -Eq '^[[:space:]]*libstdc\+\+\.so\.6[[:space:]]+=> /opt/gcc-13\.3\.0/' "$provenance/$name.ldd.txt"; \
        fi; \
        if grep -Eq '^[[:space:]]*libgcc_s\.so\.1[[:space:]]+=>' "$provenance/$name.ldd.txt"; then \
            grep -Eq '^[[:space:]]*libgcc_s\.so\.1[[:space:]]+=> /opt/gcc-13\.3\.0/' "$provenance/$name.ldd.txt"; \
        fi; \
    done

WORKDIR /workspace
CMD ["/bin/bash"]
