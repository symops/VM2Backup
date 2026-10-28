#!/bin/bash
# Подпись Authenticode (SHA-256) файлов продукта через osslsigncode. Подробности: docs/SIGNING.md.
#   tools/sign.sh [параметры] файл.exe [файл2.exe ...]
# Ключ и сертификат (одно из двух):
#   --pfx файл.pfx                     PFX / PKCS#12 (по умолчанию $VM2B_SIGN_PFX)
#   --cert файл.pem --key ключ.pem     сертификат (с цепочкой) и закрытый ключ в PEM
# Пароль ключа: НИКОГДА не в командной строке. Берётся (по порядку) из
#   --password-file файл   (по умолчанию $VM2B_SIGN_PASSWORD_FILE),
#   --password-env ИМЯ     (имя переменной окружения, по умолчанию VM2B_SIGN_PASSWORD),
#   либо из stdin при --password-stdin.
# Прочее:
#   --name "VM2Backup"          имя продукта в подписи (по умолчанию VM2Backup)
#   --url https://...           адрес продукта в подписи (по умолчанию $VM2B_SIGN_URL, может быть пустым)
#   --timestamp URL             адрес службы меток времени RFC 3161 (по умолчанию $VM2B_SIGN_TIMESTAMP; БЕЗ метки подпись перестанет считаться
#                               действительной вместе с окончанием срока сертификата; в тестах метка не используется: интернет не нужен)
#   --verify                    после подписи проверить подпись (osslsigncode verify; корень берётся из --ca файл.pem, если задан)
#   --ca файл.pem               корневой сертификат для проверки (по умолчанию $VM2B_SIGN_CA; без него «verification: failed» из-за недоверенного корня - это нормально)
# Файл заменяется на месте (через временный файл); уже подписанный файл подписывается заново (старая подпись снимается).
# Подписанный целиком одиночный VM2Backup-Setup.exe продолжает работать: загрузчик ищет хвост "VM2BPKG1" перед таблицей сертификатов.
set -eu
PFX=${VM2B_SIGN_PFX:-}; CERT=; KEY=; PWF=${VM2B_SIGN_PASSWORD_FILE:-}; PWENV=VM2B_SIGN_PASSWORD; PWSTDIN=0
NAME=${VM2B_SIGN_NAME:-VM2Backup}; URL=${VM2B_SIGN_URL:-}; TS=${VM2B_SIGN_TIMESTAMP:-}; VERIFY=0; CA=${VM2B_SIGN_CA:-}; FILES=()
while [ $# -gt 0 ]; do case "$1" in
  --pfx) PFX=$2; shift 2;; --cert) CERT=$2; shift 2;; --key) KEY=$2; shift 2;;
  --password-file) PWF=$2; shift 2;; --password-env) PWENV=$2; shift 2;; --password-stdin) PWSTDIN=1; shift;;
  --name) NAME=$2; shift 2;; --url) URL=$2; shift 2;; --timestamp) TS=$2; shift 2;;
  --verify) VERIFY=1; shift;; --ca) CA=$2; shift 2;;
  -h|--help) sed -n 2,24p "$0"; exit 0;;
  -*) echo "sign.sh: неизвестный параметр $1" >&2; exit 2;;
  *) FILES+=("$1"); shift;; esac; done
[ ${#FILES[@]} -gt 0 ] || { echo "sign.sh: не указаны файлы" >&2; exit 2; }
command -v osslsigncode >/dev/null || { echo "sign.sh: нужен osslsigncode" >&2; exit 1; }
if [ -n "$PFX" ]; then [ -f "$PFX" ] || { echo "sign.sh: нет файла $PFX" >&2; exit 1; }
elif [ -n "$CERT" ] && [ -n "$KEY" ]; then [ -f "$CERT" ] && [ -f "$KEY" ] || { echo "sign.sh: нет файла сертификата или ключа" >&2; exit 1; }
else echo "sign.sh: нужен --pfx или пара --cert/--key" >&2; exit 2; fi
# пароль -> временный файл с правами 0600 (osslsigncode -readpass); в журнал и командную строку не попадает
TMPD=$(mktemp -d "${TMPDIR:-/tmp}/vm2b-sign.XXXXXX"); chmod 700 "$TMPD"; trap 'rm -rf "$TMPD"' EXIT
PWFILE=
if [ $PWSTDIN = 1 ]; then umask 077; head -n1 > "$TMPD/pw"; PWFILE=$TMPD/pw
elif [ -n "$PWF" ]; then [ -f "$PWF" ] || { echo "sign.sh: нет файла пароля $PWF" >&2; exit 1; }; umask 077; head -n1 "$PWF" | tr -d '\r' > "$TMPD/pw"; PWFILE=$TMPD/pw
elif [ -n "${!PWENV:-}" ]; then umask 077; printf '%s\n' "${!PWENV}" > "$TMPD/pw"; PWFILE=$TMPD/pw; fi
KEYARGS=()
if [ -n "$PFX" ]; then KEYARGS+=(-pkcs12 "$PFX"); else KEYARGS+=(-certs "$CERT" -key "$KEY"); fi
[ -n "$PWFILE" ] && KEYARGS+=(-readpass "$PWFILE")
OPTS=(-h sha256 -n "$NAME"); [ -n "$URL" ] && OPTS+=(-i "$URL")
[ -n "$TS" ] && OPTS+=(-ts "$TS")
for f in "${FILES[@]}"; do
  [ -f "$f" ] || { echo "sign.sh: нет файла $f" >&2; exit 1; }
  osslsigncode remove-signature -in "$f" -out "$TMPD/unsigned" >/dev/null 2>&1 && cp "$TMPD/unsigned" "$TMPD/in" || cp "$f" "$TMPD/in"
  osslsigncode sign "${KEYARGS[@]}" "${OPTS[@]}" -in "$TMPD/in" -out "$TMPD/out" >"$TMPD/log" 2>&1 || { cat "$TMPD/log" >&2; echo "sign.sh: подпись $f не удалась" >&2; exit 1; }
  cat "$TMPD/out" > "$f"; rm -f "$TMPD/in" "$TMPD/out" "$TMPD/unsigned"
  echo "подписан: $f"
  if [ $VERIFY = 1 ]; then
    V=(verify -in "$f"); [ -n "$CA" ] && V+=(-CAfile "$CA")
    osslsigncode "${V[@]}" 2>&1 | grep -E "^(Signature verification|Number of verified|Hash|Message digest|Current message digest|Calculated message digest)" | sed 's/^/    /' || true
  fi
done
