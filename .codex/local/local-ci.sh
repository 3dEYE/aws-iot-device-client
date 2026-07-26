#!/usr/bin/env bash

set -euo pipefail

readonly mode="${1:-all}"
readonly repo_root="/workspace"
readonly build_dir="/build/x64"

case "$mode" in
    all|build|test)
        ;;
    *)
        printf 'Unsupported local CI mode: %s\n' "$mode" >&2
        exit 2
        ;;
esac

cd "$repo_root"
git config --global --add safe.directory "$repo_root"

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
        git fetch --force --unshallow --tags
    elif ! git describe \
        --abbrev=0 --tags --match 'v[0-9]*' >/dev/null 2>&1; then
        git fetch --force --tags
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

    cmake -S "$repo_root" -B "$build_dir" -G Ninja \
        -DBUILD_SDK=ON \
        -DBUILD_TEST_DEPS=ON \
        -DBUILD_TESTING=ON \
        -DCMAKE_BUILD_TYPE=Debug \
        -DBUILD_AWS_C_IOT_TESTS=ON \
        -DENABLE_NET_TESTS=ON \
        -DLINK_DL=ON \
        -DCMAKE_C_COMPILER_LAUNCHER=ccache \
        -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
        -DCMAKE_CXX_FLAGS=-Wno-error=ignored-attributes
}

build_targets() {
    cmake --build "$build_dir" \
        --target \
            aws-iot-device-client \
            test-aws-iot-device-client \
            aws-c-iot-tests \
            EventstreamRpc-cpp-tests \
            IotDeviceDefender-cpp-tests \
        --parallel 2
}

require_test_build() {
    if [[ ! -f "${build_dir}/CMakeCache.txt" ||
          ! -x "${build_dir}/test/test-aws-iot-device-client" ||
          ! -d "${build_dir}/aws-c-iot-tests" ||
          ! -d "${build_dir}/aws-iot-device-sdk-cpp-v2-build" ]]; then
        printf '%s\n' \
            'The named Docker volume does not contain a complete x64 test build.' \
            'Run pwsh .codex/local/local-ci.ps1 -Mode All or -Mode Build first.' >&2
        exit 1
    fi
}

run_tests() {
    require_test_build

    AWS_CRT_MEMORY_TRACING=1 \
        "${build_dir}/test/test-aws-iot-device-client" --gtest_brief=1

    ctest \
        --test-dir "${build_dir}/aws-c-iot-tests" \
        --output-on-failure \
        --parallel 2 \
        --timeout 60 \
        --no-tests=error

    ctest \
        --test-dir "${build_dir}/aws-iot-device-sdk-cpp-v2-build" \
        --output-on-failure \
        --parallel 2 \
        --timeout 60 \
        --no-tests=error
}

case "$mode" in
    all)
        configure
        build_targets
        run_tests
        ;;
    build)
        configure
        build_targets
        ;;
    test)
        run_tests
        ;;
esac

restore_version
trap - EXIT

git status --short -- .version
git status --short
git diff --check
