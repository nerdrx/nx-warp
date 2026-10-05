#!/usr/bin/env bash
set -euo pipefail
source_dir=$(realpath "${1:?Usage: run.sh /path/to/wivrn-nx/source [output-directory]}")
report_dir=$(cd -- "$(dirname -- "$0")" && pwd)
output_dir=${2:-$(mktemp -d)}
mkdir -p "$output_dir"
output_dir=$(realpath "$output_dir")
cd "$source_dir"
includes=(-I common -I client/decoder -I server/encoder -I build-server/common -I external -I build-server/_deps/boost-src/libs/pfr/include)
for mode in normal sanitizer; do
 flags=(-O2)
 if [[ $mode == sanitizer ]]; then
  flags=(-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
 fi
 for check in nack endpoint; do
  test_source=tests/nack_test.cpp
  [[ $check != endpoint ]] || test_source="$report_dir/endpoint_probe.cpp"
  g++ -std=c++23 "${flags[@]}" "${includes[@]}" "$test_source" common/smp.cpp -lcrypto -o "$output_dir/$check-$mode" >"$output_dir/$check-$mode-build.log" 2>&1
  ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 "$output_dir/$check-$mode" >"$output_dir/$check-$mode.log" 2>&1
 done
done
python3 - "$output_dir" <<'PYCODE'
from pathlib import Path
import sys
root=Path(sys.argv[1])
for mode in ['normal','sanitizer']:
 text=(root/f'endpoint-{mode}.log').read_text()
 lines=text.splitlines()
 assert lines[-1]=='468 checks, 0 failures'
 assert len(lines)==12
 (root/f'results-{mode}.csv').write_text('\n'.join(lines[:-1])+'\n')
 assert '832 checks, 0 failures' in (root/f'nack-{mode}.log').read_text()
assert (root/'results-normal.csv').read_bytes()==(root/'results-sanitizer.csv').read_bytes()
print('Actual history selector + endpoint replay: normal/SAN pass; CSVs match.')
print('Artifacts:',root)
PYCODE
