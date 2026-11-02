#!/usr/bin/env bash
# The project's lint and format checks (issue #52), the same locally and in CI; all tools pinned in uv.lock.
#   tools/lint.sh          check: fails on any finding
#   tools/lint.sh --fix    rewrite what the formatters can fix (clang-format, ruff format / --fix, shfmt), then check
# C: clang-format (.clang-format) on firmware/ and esp32/; clang-tidy (.clang-tidy) on the files built for the host (it needs
# build/host/compile_commands.json, configured here if missing). Python: ruff format + ruff check (pyproject.toml).
# Shell: shfmt + shellcheck. Rust (crates/, #42): cargo fmt + clippy, when cargo is installed (CI's rust job always runs them). third_party/, ground_truth/ and .toolchain/ are never touched.
set -uo pipefail
cd "$(dirname "$0")/.." || exit 1

fix=0
[[ ${1:-} == --fix ]] && fix=1
failed=()

step() { # name command...
    local name=$1
    shift
    echo "== $name"
    "$@" || failed+=("$name")
}

mapfile -t c_files < <(git ls-files 'firmware/*.c' 'firmware/*.h' 'esp32/*.c' 'esp32/*.h' 'esp32/*.cpp')
mapfile -t sh_files < <(git ls-files '*.sh')
# clang-tidy: the files in the host build (soc/cc13x0, the Wyze boards' files and the boot selector build for ARM
# only; esp32/idf for the ESP32 only, under ESP-IDF's own warnings)
mapfile -t tidy_files < <(git ls-files 'firmware/*.c' 'esp32/*.c' | grep -v -e '^firmware/soc/' -e '^firmware/boot/' -e '^esp32/idf/' \
    -e '^firmware/boards/launchxl' -e '^firmware/boards/wyze')

if ((fix)); then
    uv run clang-format -i "${c_files[@]}"
    uv run ruff format --quiet .
    uv run ruff check --fix --quiet . || true
    uv run shfmt -i 4 -ci -sr -w "${sh_files[@]}"
    if command -v cargo > /dev/null; then cargo fmt; fi
fi

step clang-format uv run clang-format --dry-run --Werror "${c_files[@]}"
if [[ ! -f build/host/compile_commands.json ]]; then
    cmake --preset host -DCMAKE_EXPORT_COMPILE_COMMANDS=ON > /dev/null
fi
step clang-tidy uv run clang-tidy -p build/host --quiet "${tidy_files[@]}"
step ruff-format uv run ruff format --check --quiet .
step ruff-check uv run ruff check --quiet .
step shfmt uv run shfmt -i 4 -ci -sr -d "${sh_files[@]}"
step shellcheck uv run shellcheck -x "${sh_files[@]}"
if command -v cargo > /dev/null; then
    step cargo-fmt cargo fmt --check
    step clippy cargo clippy --quiet --all-targets --all-features -- -D warnings
fi

if ((${#failed[@]})); then
    echo "lint: FAILED: ${failed[*]} (tools/lint.sh --fix rewrites what can be fixed automatically)"
    exit 1
fi
echo "lint: all clean"
