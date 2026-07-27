#!/usr/bin/env bash

set -euo pipefail

script_dir="$(
    cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
    pwd
)"
readonly script_dir

exec bash "${script_dir}/setup.sh"
