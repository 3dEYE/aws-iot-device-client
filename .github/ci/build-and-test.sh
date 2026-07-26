#!/usr/bin/env bash

set -euo pipefail

readonly architecture="${1:-x64}"
readonly openssl_version='3.5.7'
readonly openssl_sha256='a8c0d28a529ca480f9f36cf5792e2cd21984552a3c8e4aa11a24aa31aeac98e8'

repo_root="$(git rev-parse --show-toplevel)"
readonly repo_root
readonly sdk_cache_dir="${AWS_IOT_DEVICE_SDK_CACHE_DIR:?AWS_IOT_DEVICE_SDK_CACHE_DIR must be set}"
sdk_commit="$(
    sed -n \
        's/^set(AWS_IOT_DEVICE_SDK_COMMIT \([0-9a-f]\{40\}\))$/\1/p' \
        "${repo_root}/cmake/AwsIotDeviceSdkPins.cmake"
)"
readonly sdk_commit

if [[ ! "$sdk_commit" =~ ^[0-9a-f]{40}$ ]]; then
    printf 'Unable to read the pinned AWS IoT Device SDK commit\n' >&2
    exit 1
fi

version_status="$(git status --short -- .version)"
if [[ -n "$version_status" ]]; then
    printf 'Refusing to overwrite a pre-existing .version change:\n%s\n' \
        "$version_status" >&2
    exit 1
fi

restore_version() {
    git -C "$repo_root" restore --source=HEAD --worktree -- .version
}
trap restore_version EXIT

prepare_sdk_source() {
    local build_dir="$1"
    local sdk_source_dir="${build_dir}/aws-iot-device-sdk-cpp-v2-src"
    local actual_commit
    local aws_c_iot_commit
    local invalid_submodules

    mkdir -p "$build_dir"
    if [[ ! -d "$sdk_source_dir" ]]; then
        cp -a "$sdk_cache_dir" "$sdk_source_dir"
    fi

    actual_commit="$(git -C "$sdk_source_dir" rev-parse HEAD)"
    if [[ "$actual_commit" != "$sdk_commit" ]]; then
        printf 'Cached AWS IoT Device SDK is at %s; expected %s\n' \
            "$actual_commit" "$sdk_commit" >&2
        exit 1
    fi

    aws_c_iot_commit="$(
        git -C "$sdk_source_dir" rev-parse HEAD:crt/aws-c-iot
    )"
    git -C "${sdk_source_dir}/crt/aws-c-iot" \
        reset --hard "$aws_c_iot_commit" >/dev/null
    git -C "${sdk_source_dir}/crt/aws-c-iot" clean -fdq

    invalid_submodules="$(
        git -C "$sdk_source_dir" submodule status --recursive |
            awk '$1 ~ /^[-+U]/ && $2 !~ /(^|\/)aws-lc$/'
    )"
    if [[ -n "$invalid_submodules" ]]; then
        printf 'Cached AWS IoT Device SDK has invalid submodules:\n%s\n' \
            "$invalid_submodules" >&2
        exit 1
    fi

    printf '%s\n' "$sdk_source_dir"
}

configure_common() {
    local build_dir="$1"
    local sdk_source_dir="$2"
    shift 2

    cmake -S "$repo_root" -B "$build_dir" -G Ninja \
        -DBUILD_SDK=ON \
        -DBUILD_TEST_DEPS=ON \
        -DCMAKE_C_COMPILER_LAUNCHER=ccache \
        -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
        -DCMAKE_CXX_FLAGS=-Wno-error=ignored-attributes \
        -DAWS_IOT_DEVICE_SDK_SOURCE_DIR="$sdk_source_dir" \
        "$@"
}

build_targets() {
    local build_dir="$1"

    cmake --build "$build_dir" \
        --target aws-iot-device-client test-aws-iot-device-client \
        --parallel 2
}

run_native() {
    local build_dir="${repo_root}/build/x64"
    local sdk_source_dir

    sdk_source_dir="$(prepare_sdk_source "$build_dir")"

    configure_common "$build_dir" "$sdk_source_dir" \
        -DCMAKE_BUILD_TYPE=Debug \
        -DBUILD_AWS_C_IOT_TESTS=ON \
        -DENABLE_NET_TESTS=ON \
        -DLINK_DL=ON

    build_targets "$build_dir"
    cmake --build "$build_dir" \
        --target aws-c-iot-tests \
        --parallel 2

    AWS_CRT_MEMORY_TRACING=1 \
        "${build_dir}/test/test-aws-iot-device-client" --gtest_brief=1

    ctest \
        --test-dir "${build_dir}/aws-c-iot-tests" \
        --output-on-failure \
        --parallel 2 \
        --timeout 60 \
        --no-tests=error
}

prepare_cross_openssl() {
    local configure_target="$1"
    local triplet="$2"
    local prefix="/usr/lib/${triplet}"
    local archive="${RUNNER_TEMP:-/tmp}/openssl-${openssl_version}.tar.gz"
    local work_dir

    if [[ -f "${prefix}/lib/libcrypto.a" && -f "${prefix}/lib/libssl.a" ]]; then
        return
    fi

    curl \
        --silent \
        --show-error \
        --fail \
        --location \
        --retry 3 \
        --output "$archive" \
        "https://github.com/openssl/openssl/releases/download/openssl-${openssl_version}/openssl-${openssl_version}.tar.gz"

    printf '%s  %s\n' "$openssl_sha256" "$archive" | sha256sum --check -

    work_dir="$(mktemp -d)"
    tar -xzf "$archive" -C "$work_dir" --strip-components=1

    (
        cd "$work_dir"
        ./Configure "$configure_target" shared no-tests \
            --prefix="$prefix" \
            --openssldir="${prefix}/openssl" \
            --libdir=lib \
            --cross-compile-prefix="/usr/bin/${triplet}-"
        make -s -j2 build_libs
        sudo make -s install_dev
    )
}

run_cross() {
    local build_name="$1"
    local triplet="$2"
    local configure_target="$3"
    local toolchain_file="$4"
    local expected_class="$5"
    local expected_machine="$6"
    local build_dir="${repo_root}/build/${build_name}"
    local sdk_source_dir
    local readelf="${triplet}-readelf"

    sudo apt-get update -qq
    sudo apt-get install --yes --no-install-recommends "g++-${triplet}"
    prepare_cross_openssl "$configure_target" "$triplet"

    sdk_source_dir="$(prepare_sdk_source "$build_dir")"
    configure_common "$build_dir" "$sdk_source_dir" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE="${repo_root}/${toolchain_file}"

    grep -Fq \
        "/usr/lib/${triplet}/lib/libcrypto.a" \
        "${build_dir}/CMakeCache.txt"
    grep -Fq \
        "/usr/lib/${triplet}/lib/libssl.a" \
        "${build_dir}/CMakeCache.txt"

    build_targets "$build_dir"

    "$readelf" -h "${build_dir}/aws-iot-device-client" |
        grep -Eq "Class:[[:space:]]+${expected_class}"
    "$readelf" -h "${build_dir}/aws-iot-device-client" |
        grep -Eq "Machine:[[:space:]]+${expected_machine}"
    "$readelf" -h "${build_dir}/test/test-aws-iot-device-client" |
        grep -Eq "Class:[[:space:]]+${expected_class}"
    "$readelf" -h "${build_dir}/test/test-aws-iot-device-client" |
        grep -Eq "Machine:[[:space:]]+${expected_machine}"
}

case "$architecture" in
    x64)
        run_native
        ;;
    arm32)
        run_cross \
            arm32 \
            arm-linux-gnueabihf \
            linux-generic32 \
            cmake-toolchain/Toolchain-armhf.cmake \
            ELF32 \
            ARM
        ;;
    arm64)
        run_cross \
            arm64 \
            aarch64-linux-gnu \
            linux-aarch64 \
            cmake-toolchain/Toolchain-aarch64.cmake \
            ELF64 \
            AArch64
        ;;
    *)
        printf 'Unsupported architecture: %s\n' "$architecture" >&2
        exit 2
        ;;
esac

restore_version
trap - EXIT
