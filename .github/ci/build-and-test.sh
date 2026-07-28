#!/usr/bin/env bash

set -euo pipefail

readonly architecture="${1:-x64}"
readonly build_mode="${2:-native}"
readonly native_build_type="${3:-Debug}"
readonly openssl_version='3.5.7'
readonly openssl_sha256='a8c0d28a529ca480f9f36cf5792e2cd21984552a3c8e4aa11a24aa31aeac98e8'

case "$native_build_type" in
    Debug|Release)
        ;;
    *)
        printf 'Unsupported native build type: %s\n' "$native_build_type" >&2
        exit 2
        ;;
esac

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

version_status="$(git -C "$repo_root" status --short -- .version)"
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
    local aws_c_mqtt_commit
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

    aws_c_mqtt_commit="$(
        git -C "${sdk_source_dir}/crt/aws-crt-cpp" \
            rev-parse HEAD:crt/aws-c-mqtt
    )"
    git -C "${sdk_source_dir}/crt/aws-crt-cpp/crt/aws-c-mqtt" \
        reset --hard "$aws_c_mqtt_commit" >/dev/null
    git -C "${sdk_source_dir}/crt/aws-crt-cpp/crt/aws-c-mqtt" clean -fdq

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
        -DBUILD_TESTING=ON \
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
    local build_name="$1"
    local build_type="$2"
    local build_dir="${repo_root}/build/${build_name}"
    local sdk_source_dir

    sdk_source_dir="$(prepare_sdk_source "$build_dir")"

    configure_common "$build_dir" "$sdk_source_dir" \
        -DCMAKE_BUILD_TYPE="$build_type" \
        -DBUILD_AWS_C_IOT_TESTS=ON \
        -DBUILD_AWS_C_MQTT_TESTS=ON \
        -DENABLE_NET_TESTS=ON \
        -DLINK_DL=ON

    build_targets "$build_dir"
    cmake --build "$build_dir" \
        --target \
            aws-c-iot-tests \
            aws-c-mqtt-tests \
            EventstreamRpc-cpp-tests \
            IotDeviceDefender-cpp-tests \
        --parallel 2

    AWS_CRT_MEMORY_TRACING=1 \
        "${build_dir}/test/test-aws-iot-device-client" --gtest_brief=1

    ctest \
        --test-dir "${build_dir}/aws-c-iot-tests" \
        --output-on-failure \
        --parallel 2 \
        --timeout 60 \
        --no-tests=error

    ctest \
        --test-dir "${build_dir}/aws-c-mqtt-tests" \
        --output-on-failure \
        --tests-regex '^mqtt_connection_(sub_timeout|resubscribe_timeout)$' \
        --timeout 60 \
        --no-tests=error

    ctest \
        --test-dir "${build_dir}/aws-iot-device-sdk-cpp-v2-build" \
        --output-on-failure \
        --parallel 2 \
        --timeout 60 \
        --no-tests=error
}

prepare_cross_openssl() {
    local configure_target="$1"
    local triplet="$2"
    local expected_machine="$3"
    local cache_root
    cache_root="$(
        realpath -m \
            "${CROSS_OPENSSL_CACHE_DIR:-${repo_root}/.ci-cache/cross-openssl}"
    )"
    local cache_prefix="${cache_root}/${triplet}/openssl-${openssl_version}"
    local archive="${RUNNER_TEMP:-/tmp}/openssl-${openssl_version}.tar.gz"
    local work_dir

    case "$cache_root" in
        "${repo_root}/.ci-cache/"*)
            ;;
        *)
            printf 'Cross OpenSSL cache must be inside %s/.ci-cache: %s\n' \
                "$repo_root" "$cache_root" >&2
            exit 1
            ;;
    esac

    if cross_openssl_cache_is_valid \
        "$cache_prefix" "$configure_target" "$triplet" "$expected_machine"; then
        return
    fi

    if [[ -e "$cache_prefix" ]]; then
        case "$cache_prefix" in
            "${cache_root}/${triplet}/openssl-${openssl_version}")
                rm -rf -- "$cache_prefix"
                ;;
            *)
                printf 'Refusing to clear unexpected OpenSSL cache path: %s\n' \
                    "$cache_prefix" >&2
                exit 1
                ;;
        esac
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

    mkdir -p "$cache_prefix"
    (
        cd "$work_dir"
        ./Configure "$configure_target" shared no-tests \
            --prefix="$cache_prefix" \
            --openssldir="${cache_prefix}/openssl" \
            --libdir=lib \
            --cross-compile-prefix="/usr/bin/${triplet}-"
        make -s -j2 build_libs
        make -s install_dev
    )

    printf '%s\n' \
        "recipe=2" \
        "openssl_version=${openssl_version}" \
        "openssl_sha256=${openssl_sha256}" \
        "configure_target=${configure_target}" \
        "triplet=${triplet}" \
        >"${cache_prefix}/device-client-cache-manifest"

    if ! cross_openssl_cache_is_valid \
        "$cache_prefix" "$configure_target" "$triplet" "$expected_machine"; then
        printf 'Cross-compiled OpenSSL cache validation failed\n' >&2
        exit 1
    fi
}

cross_openssl_cache_is_valid() {
    local cache_prefix="$1"
    local configure_target="$2"
    local triplet="$3"
    local expected_machine="$4"
    local manifest="${cache_prefix}/device-client-cache-manifest"
    local library

    [[ -f "$manifest" ]] || return 1
    grep -Fxq "recipe=2" "$manifest" || return 1
    grep -Fxq "openssl_version=${openssl_version}" "$manifest" || return 1
    grep -Fxq "openssl_sha256=${openssl_sha256}" "$manifest" || return 1
    grep -Fxq "configure_target=${configure_target}" "$manifest" || return 1
    grep -Fxq "triplet=${triplet}" "$manifest" || return 1
    [[ -f "${cache_prefix}/include/openssl/ssl.h" ]] || return 1

    for library in libcrypto.a libssl.a; do
        [[ -s "${cache_prefix}/lib/${library}" ]] || return 1
        "${triplet}-readelf" -h "${cache_prefix}/lib/${library}" 2>/dev/null |
            awk -v expected="$expected_machine" '
                $1 == "Machine:" {
                    count++
                    if (index($0, expected) == 0) {
                        invalid = 1
                    }
                }
                END {
                    exit !(count > 0 && invalid == 0)
                }
            ' || return 1
    done
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
    local openssl_prefix
    openssl_prefix="$(
        realpath -m \
            "${CROSS_OPENSSL_CACHE_DIR:-${repo_root}/.ci-cache/cross-openssl}/${triplet}/openssl-${openssl_version}"
    )"

    sudo apt-get update -qq
    sudo apt-get install --yes --no-install-recommends "g++-${triplet}"
    prepare_cross_openssl \
        "$configure_target" "$triplet" "$expected_machine"

    sdk_source_dir="$(prepare_sdk_source "$build_dir")"
    configure_common "$build_dir" "$sdk_source_dir" \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_AWS_C_IOT_TESTS=ON \
        -DBUILD_AWS_C_MQTT_TESTS=ON \
        -DENABLE_NET_TESTS=ON \
        -DAWS_IOT_DEVICE_CLIENT_OPENSSL_ROOT="$openssl_prefix" \
        -DOPENSSL_ROOT_DIR="$openssl_prefix" \
        -DCMAKE_TOOLCHAIN_FILE="${repo_root}/${toolchain_file}"

    grep -Fq \
        "${openssl_prefix}/lib/libcrypto.a" \
        "${build_dir}/CMakeCache.txt"
    grep -Fq \
        "${openssl_prefix}/lib/libssl.a" \
        "${build_dir}/CMakeCache.txt"

    build_targets "$build_dir"
    cmake --build "$build_dir" \
        --target \
            aws-c-iot-tests \
            aws-c-mqtt-tests \
            EventstreamRpc-cpp-tests \
            IotDeviceDefender-cpp-tests \
        --parallel 2

    "$readelf" -h "${build_dir}/aws-iot-device-client" |
        grep -Eq "Class:[[:space:]]+${expected_class}"
    "$readelf" -h "${build_dir}/aws-iot-device-client" |
        grep -Eq "Machine:[[:space:]]+${expected_machine}"
    "$readelf" -h "${build_dir}/test/test-aws-iot-device-client" |
        grep -Eq "Class:[[:space:]]+${expected_class}"
    "$readelf" -h "${build_dir}/test/test-aws-iot-device-client" |
        grep -Eq "Machine:[[:space:]]+${expected_machine}"
}

case "${architecture}:${build_mode}" in
    x64:native)
        run_native x64 "$native_build_type"
        ;;
    arm64:native)
        run_native arm64-native "$native_build_type"
        ;;
    arm32:cross)
        run_cross \
            arm32 \
            arm-linux-gnueabihf \
            linux-generic32 \
            cmake-toolchain/Toolchain-armhf.cmake \
            ELF32 \
            ARM
        ;;
    arm64:cross)
        run_cross \
            arm64-cross \
            aarch64-linux-gnu \
            linux-aarch64 \
            cmake-toolchain/Toolchain-aarch64.cmake \
            ELF64 \
            AArch64
        ;;
    *)
        printf 'Unsupported architecture/mode: %s/%s\n' \
            "$architecture" "$build_mode" >&2
        exit 2
        ;;
esac

restore_version
trap - EXIT
