#!/bin/sh
# Статический анализ VM2Backup: предупреждения компилятора (x64 и x86), Clang Static Analyzer, CodeQL (security-and-quality).
# Результаты: .winvm-results/codecheck-<дата>/ (вне git). Подробности: docs/DEV.md, раздел «Статический анализ».
#   tools/codecheck.sh warn      сборка x64 + x86, вывод всех предупреждений (должно быть пусто)
#   tools/codecheck.sh csa       Clang Static Analyzer по src/*.cpp (долго: ~20 минут, один поток)
#   tools/codecheck.sh codeql    база CodeQL (трассировка ARCH=x64 ./build.sh) + набор security-and-quality
set -e
R=$(cd "$(dirname "$0")/.." && pwd); cd "$R"
D=${TMPDIR:-/home/symo/tmp-codecheck}; mkdir -p "$D"; export TMPDIR=$D
OUT=$R/.winvm-results/codecheck-$(date +%F); mkdir -p "$OUT"
SK=/root/.claude/skills/symo-codecheck/scripts
case "$1" in
warn) { ARCH=x64 ./build.sh 2>&1; ARCH=x86 ./build.sh 2>&1; } | grep -iE "warning:|error" || echo "0 warnings" ;;
csa)
    # Параллельно по файлам (CSA_JOBS, по умолчанию 8), у каждого файла свой таймаут (CSA_TIMEOUT, секунд), подробности в tools/csa-file.sh.
    W=$D/csa-parts; rm -rf "$W"; mkdir -p "$W"
    ls src/*.cpp | sed 's|.*/||' | xargs -P "${CSA_JOBS:-8}" -I{} sh -c 'tools/csa-file.sh {} > "$0/{}.out" 2>&1' "$W"
    cat "$W"/*.out > "$OUT/csa.log"; echo "== $(grep -c 'warning:' "$OUT/csa.log") warning(s) in $R/src" | tee -a "$OUT/csa.log" ;;
codeql)
    CQ=/home/symo/Claude/tools/codeql/bundle-2.27.4/codeql
    printf '#!/bin/sh\ncd %s\nARCH=x64 OUT=%s/qlout TMPDIR=%s exec sh ./build.sh\n' "$R" "$D" "$D" > "$D/qlbuild.sh"; chmod +x "$D/qlbuild.sh"
    "$CQ" database create "$D/db" --overwrite --language=cpp --source-root=. --command="$D/qlbuild.sh"
    "$CQ" database analyze "$D/db" codeql/cpp-queries:codeql-suites/cpp-security-and-quality.qls --format=sarifv2.1.0 --output="$OUT/security-and-quality.sarif" --threads=4
    xz -9 -f "$OUT/security-and-quality.sarif"; rm -rf "$D/db" "$D/qlout" ;;
*) echo "usage: $0 warn|csa|codeql" >&2; exit 2 ;;
esac
