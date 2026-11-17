#!/bin/sh
# Clang Static Analyzer для одного файла src/<имя>.cpp (вызывается из tools/codecheck.sh csa, параллельно).
# Вывод: предупреждения и пояснения (без шума) в stdout. Таймаут: CSA_TIMEOUT секунд (по умолчанию 1500).
# Для src/tests_esxi.cpp параметры урезаны (см. docs/DEV.md, «Статический анализ»); CSA_ESXI_NODES=0 снимает урезание (около 10 минут).
f=${1:?usage: csa-file.sh <file.cpp>}
R=$(cd "$(dirname "$0")/.." && pwd); cd "$R/src" || exit 1
CF=$("$R/tools/deps-env.sh" x64 cflags | sed "s|-I\(\S*\)|-isystem \1|")
X=
case $f in
tests_esxi.cpp) [ "${CSA_ESXI_NODES:-30000}" != 0 ] && X="-Xclang -analyzer-config -Xclang max-nodes=${CSA_ESXI_NODES:-30000} -Xclang -analyzer-config -Xclang max-symbol-complexity=40 -Xclang -analyzer-max-loop -Xclang 2" ;;
esac
# shellcheck disable=SC2086
timeout "${CSA_TIMEOUT:-1500}" clang++ --target=x86_64-w64-windows-gnu -std=c++17 -fno-exceptions -fno-rtti -municode \
    -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DUNICODE -D_UNICODE -isystem /usr/x86_64-w64-mingw32/include \
    -DVM2B_HAVE_FTP -DVM2B_HAVE_SSH -DVM2B_HAVE_ESXI $CF $X --analyze -Xclang -analyzer-output=text -o /dev/null "$f" 2>&1 |
    grep -E "warning:|note:|error:" | grep -vE "note: (Assuming|Taking|Entered|Returning|Calling|Loop|Left side|expanded from)"
exit 0
