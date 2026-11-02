#!/usr/bin/env bash
# Build the CMake "coverage" preset (Clang/LLVM source-based coverage in build/coverage), run all automated host
# tests (ctest + pytest, including test_host_e2e.py when OWS_W2M and mosquitto are available), and emit both C
# (llvm-cov report + HTML) and Python (reference/wyzesense_ref + tools/) coverage summaries.
#   tools/coverage.sh [--html-dir DIR]
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

HTML_DIR=$ROOT/build/coverage/html
while (($#)); do
    case $1 in
        --html-dir)
            HTML_DIR=$2
            shift 2
            ;;
        *)
            echo "usage: tools/coverage.sh [--html-dir DIR]" >&2
            exit 2
            ;;
    esac
done

for bin in clang llvm-profdata llvm-cov; do
    command -v "$bin" > /dev/null || {
        echo "coverage: missing $bin" >&2
        exit 1
    }
done

BUILD=$ROOT/build/coverage
rm -rf "$BUILD/profraw" "$BUILD/pyhits" "$BUILD/pyhook" "$HTML_DIR"
mkdir -p "$BUILD/profraw" "$BUILD/pyhits" "$BUILD/pyhook"

echo "== cmake --preset coverage"
cmake --preset coverage > /dev/null
cmake --build --preset coverage --parallel

cat > "$BUILD/pyhook/sitecustomize.py" << 'PY'
import atexit
import json
import os
import signal
import sys
import time

_OUT_DIR = os.environ.get('OWS_PYCOV_DIR')
_ROOT = os.environ.get('OWS_PYCOV_ROOT', '')
_PREFIXES = (os.path.join(_ROOT, 'reference', 'wyzesense_ref') + os.sep, os.path.join(_ROOT, 'tools') + os.sep)

if _OUT_DIR and _ROOT and hasattr(sys, 'monitoring'):
    _hits = {}
    _mon = sys.monitoring
    _TOOL = _mon.COVERAGE_ID
    try:
        _mon.use_tool_id(_TOOL, 'ows_pycov')
    except ValueError:
        _TOOL = None

    if _TOOL is not None:

        def _on_start(code, _offset):
            fn = os.path.abspath(code.co_filename)
            if fn.startswith(_PREFIXES):
                _hits.setdefault(fn, set())
                _mon.set_local_events(_TOOL, code, _mon.events.LINE)
            return _mon.DISABLE

        def _on_line(code, line):
            s = _hits.get(os.path.abspath(code.co_filename))
            if s is not None:
                s.add(line)
            return _mon.DISABLE

        _mon.register_callback(_TOOL, _mon.events.PY_START, _on_start)
        _mon.register_callback(_TOOL, _mon.events.LINE, _on_line)
        _mon.set_events(_TOOL, _mon.events.PY_START)
        _dumped = False

        def _dump():
            global _dumped
            if _dumped or not _hits:
                return
            _dumped = True
            os.makedirs(_OUT_DIR, exist_ok=True)
            out = {k: sorted(v) for k, v in _hits.items() if v}
            if out:
                path = os.path.join(_OUT_DIR, f'hits-{os.getpid()}-{time.time_ns()}.json')
                with open(path, 'w', encoding='utf-8') as f:
                    json.dump(out, f)

        atexit.register(_dump)
        _prev = signal.getsignal(signal.SIGTERM)

        def _on_sigterm(signum, frame):
            _dump()
            if callable(_prev):
                _prev(signum, frame)
            else:
                signal.signal(signal.SIGTERM, signal.SIG_DFL)
                os.kill(os.getpid(), signal.SIGTERM)

        signal.signal(signal.SIGTERM, _on_sigterm)
PY

export LLVM_PROFILE_FILE="$BUILD/profraw/%p-%m%c.profraw"
export OWS_HOST_BUILD="$BUILD"
export OWS_PYCOV_DIR="$BUILD/pyhits"
export OWS_PYCOV_ROOT="$ROOT"
export PYTHONPATH="$BUILD/pyhook${PYTHONPATH:+:$PYTHONPATH}"
export no_proxy="127.0.0.1,localhost" NO_PROXY="127.0.0.1,localhost"

echo "== ctest --preset coverage"
ctest --preset coverage

echo "== pytest"
uv run pytest -q

PROFDATA=$BUILD/coverage.profdata
shopt -s nullglob
profraws=("$BUILD/profraw/"*.profraw)
shopt -u nullglob
llvm-profdata merge -sparse "${profraws[@]}" -o "$PROFDATA"

bins=(
    "$BUILD/firmware/test_common"
    "$BUILD/firmware/test_gateway"
    "$BUILD/firmware/test_sensor"
    "$BUILD/firmware/test_nvstore"
    "$BUILD/firmware/test_boot"
    "$BUILD/firmware/test_apps"
    "$BUILD/firmware/ows-host-gateway"
    "$BUILD/firmware/ows-host-sensor"
    "$BUILD/firmware/ows-host-sniffer"
    "$BUILD/esp32/test_relay"
    "$BUILD/esp32/test_gwnv"
    "$BUILD/esp32/test_buffer"
    "$BUILD/esp32/test_multipart"
    "$BUILD/esp32/test_http_auth"
    "$BUILD/esp32/test_mdns"
    "$BUILD/esp32/ows-relay"
    "$BUILD/esp32/ows-sbl"
    "$BUILD/esp32/ows-gwsync"
)
obj_args=("${bins[0]}")
for b in "${bins[@]:1}"; do
    obj_args+=(-object "$b")
done
ignore_re='.*(/tests/|/esp32/host/test_).*'

echo ""
echo "=== C Coverage (llvm-cov, excluding test_*.c) ==="
llvm-cov report "${obj_args[@]}" "-instr-profile=$PROFDATA" "-ignore-filename-regex=$ignore_re"
llvm-cov export --summary-only "${obj_args[@]}" "-instr-profile=$PROFDATA" "-ignore-filename-regex=$ignore_re" \
    > "$BUILD/c_summary.json"
llvm-cov show "${obj_args[@]}" "-instr-profile=$PROFDATA" "-ignore-filename-regex=$ignore_re" \
    -format=html "-output-dir=$HTML_DIR" -show-branches=count -show-line-counts-or-regions

PYTHONPATH='' uv run python - "$ROOT" "$BUILD/pyhits" "$BUILD/py_summary.json" << 'PY'
import ast
import glob
import json
import os
import sys
import types

root, hits_dir, out_json = sys.argv[1], sys.argv[2], sys.argv[3]


def executable_lines(py_path):
    with open(py_path, encoding='utf-8') as f:
        src = f.read()
    code = compile(src, py_path, 'exec')
    lines, stack = set(), [code]
    while stack:
        c = stack.pop()
        for _, _, ln in c.co_lines():
            if ln is not None and ln > 0:
                lines.add(ln)
        for const in c.co_consts:
            if isinstance(const, types.CodeType):
                stack.append(const)
    tree = ast.parse(src, filename=py_path)
    for node in ast.walk(tree):
        if isinstance(node, (ast.Module, ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)) and node.body:
            first = node.body[0]
            if isinstance(first, ast.Expr) and isinstance(first.value, ast.Constant) and isinstance(first.value.value, str):
                for ln in range(first.lineno, (first.end_lineno or first.lineno) + 1):
                    lines.discard(ln)
    return lines


hits = {}
for hf in glob.glob(os.path.join(hits_dir, 'hits-*.json')):
    with open(hf, encoding='utf-8') as f:
        for fn, lns in json.load(f).items():
            hits.setdefault(fn, set()).update(lns)

py_files = sorted(glob.glob(os.path.join(root, 'reference', 'wyzesense_ref', '*.py')) + glob.glob(os.path.join(root, 'tools', '*.py')))
print('\n=== Python Line Coverage (reference/wyzesense_ref & tools) ===')
print(f'{"File":<45} {"Exec":>6} {"Hit":>6} {"Miss":>6} {"Cover":>8}')
print('-' * 75)
tot_exec = tot_hit = 0
rows = []
for pf in py_files:
    ex = executable_lines(pf)
    if not ex:
        continue
    rel = os.path.relpath(pf, root)
    ht = ex & hits.get(os.path.abspath(pf), set())
    pct = 100.0 * len(ht) / len(ex)
    tot_exec += len(ex)
    tot_hit += len(ht)
    rows.append({'file': rel, 'exec': len(ex), 'hit': len(ht), 'miss': len(ex) - len(ht), 'pct': pct})
    print(f'{rel:<45} {len(ex):>6} {len(ht):>6} {len(ex) - len(ht):>6} {pct:>7.2f}%')
print('-' * 75)
tot_pct = 100.0 * tot_hit / tot_exec if tot_exec else 0.0
print(f'{"TOTAL":<45} {tot_exec:>6} {tot_hit:>6} {tot_exec - tot_hit:>6} {tot_pct:>7.2f}%')
with open(out_json, 'w', encoding='utf-8') as f:
    json.dump({'files': rows, 'total_exec': tot_exec, 'total_hit': tot_hit, 'total_pct': tot_pct}, f, indent=2)
PY

echo ""
echo "coverage: HTML report at $HTML_DIR/index.html"
