#!/usr/bin/env bash

set +x
set -euo pipefail

repo_url='https://github.com/3dEYE/aws-iot-device-client.git'
openssl_version='3.5.7'
openssl_sha256='a8c0d28a529ca480f9f36cf5792e2cd21984552a3c8e4aa11a24aa31aeac98e8'
openssl_archive="/tmp/openssl-${openssl_version}.tar.gz"

repo_root="$(git rev-parse --show-toplevel)"
log_dir='/tmp/codex-setup-logs'
failure_tail_lines=80
current_step=0
total_steps=8

cd "$repo_root"
mkdir -p "$log_dir"

ensure_clean_version() {
    local version_status

    version_status="$(git status --short -- .version)"
    if [ -n "$version_status" ]; then
        printf 'Refusing to overwrite a pre-existing .version change:\n' >&2
        printf '%s\n' "$version_status" >&2
        exit 1
    fi
}

restore_version() {
    git -C "$repo_root" restore --source=HEAD --worktree -- .version
}

ensure_clean_version
trap restore_version EXIT

run_step() {
    local title="$1"
    shift

    current_step=$((current_step + 1))

    local log_file
    local status

    log_file="$log_dir/step-$(printf '%02d' "$current_step").log"

    printf '[%d/%d] %s ... ' \
        "$current_step" \
        "$total_steps" \
        "$title"

    set +e
    (
        set -euo pipefail
        "$@"
    ) >"$log_file" 2>&1
    status=$?
    set -e

    if [ "$status" -eq 0 ]; then
        printf 'done\n'
        return
    fi

    printf 'FAILED\n'
    printf 'Failure log: %s\n' "$log_file"
    printf '%s\n' '----- matching errors -----'
    grep -Ei 'CMake Error|fatal:|error:' "$log_file" |
        tail -n 20 || true
    printf '%s\n' '----- last log lines -----'
    tail -n "$failure_tail_lines" "$log_file"
    printf '%s\n' '--------------------------'
    exit "$status"
}

install_cross_compilers() {
    local ubuntu_sources='/etc/apt/sources.list.d/ubuntu.sources'

    if [ "$(id -u)" -ne 0 ]; then
        printf 'Codex Cloud setup must run as root.\n' >&2
        return 1
    fi

    if [ -f "$ubuntu_sources" ]; then
        sed -i \
            -e 's|http://archive.ubuntu.com/ubuntu/|https://archive.ubuntu.com/ubuntu/|g' \
            -e 's|http://security.ubuntu.com/ubuntu/|https://security.ubuntu.com/ubuntu/|g' \
            "$ubuntu_sources"
    fi

    export DEBIAN_FRONTEND=noninteractive

    apt-get update
    apt-get install -y --no-install-recommends \
        g++-arm-linux-gnueabihf \
        g++-aarch64-linux-gnu
}

download_openssl() {
    curl \
        --silent \
        --show-error \
        --fail \
        --location \
        --retry 3 \
        --output "$openssl_archive" \
        "https://github.com/openssl/openssl/releases/download/openssl-${openssl_version}/openssl-${openssl_version}.tar.gz"

    printf '%s  %s\n' \
        "$openssl_sha256" \
        "$openssl_archive" |
        sha256sum --check -
}

build_target_openssl() {
    local configure_target="$1"
    local triplet="$2"
    local prefix="/usr/lib/${triplet}"
    local work_dir

    work_dir="$(mktemp -d)"

    tar -xzf "$openssl_archive" \
        -C "$work_dir" \
        --strip-components=1

    (
        cd "$work_dir"

        ./Configure "$configure_target" shared no-tests \
            --prefix="$prefix" \
            --openssldir="$prefix/openssl" \
            --libdir=lib \
            --cross-compile-prefix="/usr/bin/${triplet}-"

        make -s -j2 build_libs
        make -s install_dev
    )

    rm -rf -- "$work_dir"
}

prepare_git_metadata() {
    # The project forces USE_OPENSSL=ON, so aws-lc is unused.
    git config --global submodule.crt/aws-lc.update none

    if [ "$(git rev-parse --is-shallow-repository)" = "true" ]; then
        git fetch --force --unshallow --tags "$repo_url"
    else
        git fetch --force --tags "$repo_url"
    fi

    git describe --abbrev=0 --tags --match 'v[0-9]*'
}

configure_native() {
    cmake -S . -B build -G Ninja \
        -DBUILD_SDK=ON \
        -DBUILD_TEST_DEPS=ON \
        -DCMAKE_CXX_FLAGS='-Wno-error=ignored-attributes'
}

configure_armhf() {
    test -f /usr/lib/arm-linux-gnueabihf/lib/libcrypto.a
    test -f /usr/lib/arm-linux-gnueabihf/lib/libssl.a

    cmake -S . -B out/armhf -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE="$repo_root/cmake-toolchain/Toolchain-armhf.cmake" \
        -DBUILD_SDK=ON \
        -DBUILD_TEST_DEPS=ON \
        -DCMAKE_CXX_FLAGS='-Wno-error=ignored-attributes'

    grep -Fq \
        '/usr/lib/arm-linux-gnueabihf/lib/libcrypto.a' \
        out/armhf/CMakeCache.txt

    grep -Fq \
        '/usr/lib/arm-linux-gnueabihf/lib/libssl.a' \
        out/armhf/CMakeCache.txt
}

configure_aarch64() {
    test -f /usr/lib/aarch64-linux-gnu/lib/libcrypto.a
    test -f /usr/lib/aarch64-linux-gnu/lib/libssl.a

    cmake -S . -B out/aarch64 -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE="$repo_root/cmake-toolchain/Toolchain-aarch64.cmake" \
        -DBUILD_SDK=ON \
        -DBUILD_TEST_DEPS=ON \
        -DCMAKE_CXX_FLAGS='-Wno-error=ignored-attributes'

    grep -Fq \
        '/usr/lib/aarch64-linux-gnu/lib/libcrypto.a' \
        out/aarch64/CMakeCache.txt

    grep -Fq \
        '/usr/lib/aarch64-linux-gnu/lib/libssl.a' \
        out/aarch64/CMakeCache.txt
}

run_step \
    'Install ARM cross-compilers' \
    install_cross_compilers

run_step \
    'Download and verify OpenSSL' \
    download_openssl

run_step \
    'Build OpenSSL for ARMHF' \
    build_target_openssl \
    linux-generic32 \
    arm-linux-gnueabihf

run_step \
    'Build OpenSSL for AArch64' \
    build_target_openssl \
    linux-aarch64 \
    aarch64-linux-gnu

rm -f -- "$openssl_archive"

run_step \
    'Fetch Git tags and prepare SDK configuration' \
    prepare_git_metadata

run_step \
    'Configure native build' \
    configure_native

run_step \
    'Configure ARMHF build' \
    configure_armhf

run_step \
    'Configure AArch64 build' \
    configure_aarch64

printf 'Setup completed successfully.\n'
printf 'Full logs: %s\n' "$log_dir"
