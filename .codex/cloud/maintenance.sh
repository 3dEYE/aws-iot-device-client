#!/usr/bin/env bash

set +x
set -euo pipefail

repo_url='https://github.com/3dEYE/aws-iot-device-client.git'
repo_root="$(git rev-parse --show-toplevel)"
log_dir='/tmp/codex-maintenance-logs'
failure_tail_lines=80
current_step=0
total_steps=4

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

refresh_git_metadata() {
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
    'Refresh Git metadata' \
    refresh_git_metadata

run_step \
    'Refresh native build configuration' \
    configure_native

run_step \
    'Refresh ARMHF build configuration' \
    configure_armhf

run_step \
    'Refresh AArch64 build configuration' \
    configure_aarch64

printf 'Maintenance completed successfully.\n'
printf 'Full logs: %s\n' "$log_dir"
