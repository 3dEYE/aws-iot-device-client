#!/usr/bin/env bash

set -euo pipefail

readonly repo_root="$(git rev-parse --show-toplevel)"
test_root="$(mktemp -d)"
readonly test_root
readonly test_repo="${test_root}/repository"
readonly test_home="${test_root}/home"

cleanup() {
    rm -rf -- "$test_root"
}
trap cleanup EXIT

mkdir -p "$test_repo" "$test_home"
cp "${repo_root}/CMakeLists.txt.versioning" "$test_repo/"

git -C "$test_repo" init --quiet
git -C "$test_repo" config user.email versioning-test@example.invalid
git -C "$test_repo" config user.name "Versioning test"
git -C "$test_repo" commit --quiet --allow-empty --message "Base release"
git -C "$test_repo" tag v1.9
git -C "$test_repo" commit --quiet --allow-empty --message "Change one"
git -C "$test_repo" commit --quiet --allow-empty --message "Change two"

generate_version() {
    (
        cd "$test_repo"
        HOME="$test_home" cmake \
            -DPROJECT_NAME=aws-iot-device-client \
            -DGIT_VERSION=ON \
            -DGIT_EXECUTABLE="$(command -v git)" \
            -P CMakeLists.txt.versioning >/dev/null
        cut -d'*' -f1 .version | tr -d '[:space:]'
    )
}

first_sha="$(git -C "$test_repo" rev-parse --short HEAD)"
first_version="$(generate_version)"
readonly first_sha first_version
if [[ "$first_version" != "v1.9.2-${first_sha}" ]]; then
    printf 'Expected v1.9.2-%s, got %s\n' "$first_sha" "$first_version" >&2
    exit 1
fi

git -C "$test_repo" tag "$first_version"
git -C "$test_repo" commit --quiet --allow-empty --message "Change three"
git -C "$test_repo" commit --quiet --allow-empty --message "Change four"

second_sha="$(git -C "$test_repo" rev-parse --short HEAD)"
second_version="$(generate_version)"
readonly second_sha second_version
if [[ "$second_version" != "v1.9.4-${second_sha}" ]]; then
    printf 'Expected v1.9.4-%s after generated release tag, got %s\n' \
        "$second_sha" "$second_version" >&2
    exit 1
fi

printf 'Version remained monotonic across generated release tag: %s\n' \
    "$second_version"

git -C "$test_repo" tag v1.10.0
git -C "$test_repo" commit --quiet --allow-empty --message "New line change one"
git -C "$test_repo" tag v1.10.1
git -C "$test_repo" commit --quiet --allow-empty --message "New line change two"

third_sha="$(git -C "$test_repo" rev-parse --short HEAD)"
third_version="$(generate_version)"
readonly third_sha third_version
if [[ "$third_version" != "v1.10.2-${third_sha}" ]]; then
    printf 'Expected v1.10.2-%s after numeric line tags, got %s\n' \
        "$third_sha" "$third_version" >&2
    exit 1
fi

printf 'Version advanced from numeric line base: %s\n' "$third_version"

git -C "$test_repo" tag v2.3.5

fourth_sha="$(git -C "$test_repo" rev-parse --short HEAD)"
fourth_version="$(generate_version)"
readonly fourth_sha fourth_version
if [[ "$fourth_version" != "v2.3.5-${fourth_sha}" ]]; then
    printf 'Expected v2.3.5-%s at non-zero numeric base, got %s\n' \
        "$fourth_sha" "$fourth_version" >&2
    exit 1
fi

git -C "$test_repo" commit --quiet --allow-empty \
    --message "Non-zero base change"

fifth_sha="$(git -C "$test_repo" rev-parse --short HEAD)"
fifth_version="$(generate_version)"
readonly fifth_sha fifth_version
if [[ "$fifth_version" != "v2.3.6-${fifth_sha}" ]]; then
    printf 'Expected v2.3.6-%s after non-zero numeric base, got %s\n' \
        "$fifth_sha" "$fifth_version" >&2
    exit 1
fi

printf 'Version preserved non-zero numeric base: %s\n' "$fifth_version"

git -C "$test_repo" tag v2.3.8

sixth_sha="$(git -C "$test_repo" rev-parse --short HEAD)"
sixth_version="$(generate_version)"
readonly sixth_sha sixth_version
if [[ "$sixth_version" != "v2.3.8-${sixth_sha}" ]]; then
    printf 'Expected v2.3.8-%s at later numeric tag, got %s\n' \
        "$sixth_sha" "$sixth_version" >&2
    exit 1
fi

git -C "$test_repo" commit --quiet --allow-empty \
    --message "Later numeric base change"

seventh_sha="$(git -C "$test_repo" rev-parse --short HEAD)"
seventh_version="$(generate_version)"
readonly seventh_sha seventh_version
if [[ "$seventh_version" != "v2.3.9-${seventh_sha}" ]]; then
    printf 'Expected v2.3.9-%s after later numeric tag, got %s\n' \
        "$seventh_sha" "$seventh_version" >&2
    exit 1
fi

printf 'Version advanced from nearest numeric base: %s\n' "$seventh_version"
