#!/usr/bin/env bash

set -euo pipefail

readonly repo_url='https://github.com/3dEYE/aws-iot-device-client.git'
repo_root="$(git rev-parse --show-toplevel)"
readonly repo_root
readonly build_dir="${repo_root}/build"

cd "$repo_root"

version_status="$(git status --short -- .version)"
if [[ -n "$version_status" ]]; then
    printf 'Refusing to overwrite a pre-existing .version change:\n%s\n' \
        "$version_status" >&2
    exit 1
fi

restore_version() {
    git restore --source=HEAD --worktree -- .version
}
trap restore_version EXIT

# The project forces USE_OPENSSL=ON, so aws-lc is unused.
git config --global submodule.crt/aws-lc.update none

prepare_git_metadata() {
    local version_tag

    if [[ "$(git rev-parse --is-shallow-repository)" == "true" ]]; then
        git fetch --force --unshallow --tags "$repo_url"
    else
        git fetch --force --tags "$repo_url"
    fi

    if ! version_tag="$(
        git describe --abbrev=0 --tags --match 'v[0-9]*' 2>/dev/null
    )"; then
        printf '%s\n' \
            'Unable to find a reachable version tag matching v[0-9]*.' \
            'Fetch the repository history and tags before configuring.' >&2
        return 1
    fi

    printf 'Using version tag %s.\n' "$version_tag"
}

configure() {
    prepare_git_metadata
    mkdir -p "$build_dir"

    CCACHE_DIR="${build_dir}/ccache" \
        cmake -S "$repo_root" -B "$build_dir" -G Ninja \
            -DBUILD_SDK=ON \
            -DBUILD_TEST_DEPS=ON \
            -DBUILD_TESTING=ON \
            -DCMAKE_BUILD_TYPE=Debug \
            -DBUILD_AWS_C_IOT_TESTS=ON \
            -DBUILD_AWS_C_MQTT_TESTS=ON \
            -DENABLE_NET_TESTS=ON \
            -DLINK_DL=ON \
            -DCMAKE_C_COMPILER_LAUNCHER=ccache \
            -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
            -DCMAKE_CXX_FLAGS=-Wno-error=ignored-attributes
}

build_targets() {
    CCACHE_DIR="${build_dir}/ccache" \
        cmake --build "$build_dir" \
            --target \
                aws-iot-device-client \
                test-aws-iot-device-client \
                aws-c-iot-tests \
                aws-c-mqtt-tests \
                EventstreamRpc-cpp-tests \
                IotDeviceDefender-cpp-tests \
            --parallel 2
}

run_tests() {
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

configure
build_targets
run_tests

restore_version
trap - EXIT

printf 'Codex Cloud x64 setup completed successfully.\n'
