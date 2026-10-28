#!/bin/bash
# Starts the SSH test servers (real OpenSSH sshd with a matrix of restricted algorithm sets, plus the paramiko servers of
# tools/testservers/), writes the --servers json and runs the SSH / SCP autotests under Wine. Everything is stopped BY PID
# when it is done (never pkill). Port range 22000-22999. Needs root (a throw-away unix account is created and removed again).
#   tools/run-ssh-tests.sh [--seed N] [--only a,b] ...     (extra arguments go to vm2bautotest.exe)
# Environment: EXE (default build/x64/vm2bautotest.exe), WINEPREFIX (default /tmp/wine-ssh), WORK (default /tmp/vm2b-sshrun),
#              MINUTES (soak: --minutes), KEEP=1 keeps the work dir and the servers' logs.
#              Windows VM harness (docs/WINDOWS-TESTS.md): VM2B_BIND_ADDR (sshd / paramiko listen address, default 127.0.0.1), VM2B_SERVER_HOST (address in servers.json,
#              default = VM2B_BIND_ADDR), VM2B_GUEST_MAP=<host prefix>=<guest prefix> (paths in servers.json as the guest sees them), VM2B_SERVERS_ONLY=1
#              (start the servers, write servers.json, create $WORK/ready and wait to be killed: no wine run, no tmpfs).
#              SSH_TEST_USER (unix account name, default vm2bsshtest: give each parallel runner its own), SSH_WINEDEBUG (default -all).
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
EXE=${EXE:-$HERE/build/x64/vm2bautotest.exe}
WORK=${WORK:-/tmp/vm2b-sshrun}
export WINEPREFIX=${WINEPREFIX:-/tmp/wine-ssh}
export WINEDEBUG=${SSH_WINEDEBUG:--all}
SSHD=/usr/sbin/sshd
UNAME=${SSH_TEST_USER:-vm2bsshtest}
UPASS='Tr0ub4dor&3 space'
PIDS=()
CREATED_USER=0
BIND=${VM2B_BIND_ADDR:-127.0.0.1}; SH=${VM2B_SERVER_HOST:-$BIND}
export VM2B_BIND_ADDR=$BIND VM2B_SERVER_HOST=$SH

cleanup() {
    mountpoint -q "$WORK/home/small" 2>/dev/null && umount "$WORK/home/small" 2>/dev/null
    for p in "${PIDS[@]:-}"; do [ -n "$p" ] && kill "$p" 2>/dev/null; done
    sleep 0.3
    for p in "${PIDS[@]:-}"; do [ -n "$p" ] && kill -9 "$p" 2>/dev/null; done
    if [ "$CREATED_USER" = 1 ]; then userdel "$UNAME" >/dev/null 2>&1; fi
    if [ "${KEEP:-0}" != 1 ]; then rm -rf "$WORK"; fi
}
trap cleanup EXIT INT TERM

[ "$(id -u)" = 0 ] || { echo "run as root (sshd needs it)"; exit 2; }
[ -f "$EXE" ] || { echo "missing $EXE (build with: ONLY=autotest OUT=... ./build.sh)"; exit 2; }
rm -rf "$WORK"; mkdir -p "$WORK/keys" "$WORK/home" "$WORK/cfg" "$WORK/log" "$WORK/pm"
chmod 755 "$WORK" "$WORK/keys"

# ---- the unix account the real sshd logs in as ----
if ! id "$UNAME" >/dev/null 2>&1; then useradd -M -d "$WORK/home" -s /bin/sh "$UNAME" && CREATED_USER=1; else echo "account $UNAME exists"; exit 2; fi
echo "$UNAME:$UPASS" | chpasswd
chown "$UNAME" "$WORK/home"

# ---- keys ----
K=$WORK/keys
ssh-keygen -q -t ed25519 -N '' -f $K/host_ed25519 </dev/null
ssh-keygen -q -t ecdsa -b 256 -N '' -f $K/host_ecdsa256 </dev/null
ssh-keygen -q -t ecdsa -b 384 -N '' -f $K/host_ecdsa384 </dev/null
ssh-keygen -q -t ecdsa -b 521 -N '' -f $K/host_ecdsa521 </dev/null
ssh-keygen -q -t rsa -b 2048 -N '' -f $K/host_rsa </dev/null
# user keys: every kind the client supports
PASSPH='pass phrase 123'
ssh-keygen -q -t ed25519 -N '' -f $K/u_ed25519 </dev/null
ssh-keygen -q -t ed25519 -N '' -f $K/n_unauthorized </dev/null        # a key the servers do not know (not in authorized_keys: the lock-out guard tests)
ssh-keygen -q -t ed25519 -N "$PASSPH" -f $K/u_ed25519_pw </dev/null
ssh-keygen -q -t ed25519 -N "$PASSPH" -Z aes256-cbc -f $K/u_ed25519_cbc </dev/null
ssh-keygen -q -t ed25519 -N "$PASSPH" -Z aes128-ctr -f $K/u_ed25519_ctr128 </dev/null
ssh-keygen -q -t ed25519 -N "$PASSPH" -Z aes256-gcm@openssh.com -f $K/u_ed25519_gcm </dev/null
ssh-keygen -q -t rsa -b 2048 -N '' -f $K/u_rsa </dev/null
ssh-keygen -q -t rsa -b 3072 -N "$PASSPH" -f $K/u_rsa_pw </dev/null
ssh-keygen -q -t rsa -b 2048 -m PEM -N '' -f $K/u_rsa_pem </dev/null
ssh-keygen -q -t rsa -b 2048 -m PKCS8 -N '' -f $K/u_rsa_pkcs8 </dev/null
ssh-keygen -q -t ecdsa -b 256 -N '' -f $K/u_ecdsa256 </dev/null
ssh-keygen -q -t ecdsa -b 384 -N "$PASSPH" -f $K/u_ecdsa384_pw </dev/null
ssh-keygen -q -t ecdsa -b 521 -N '' -f $K/u_ecdsa521 </dev/null
ssh-keygen -q -t ecdsa -b 256 -m PEM -N '' -f $K/u_ecdsa_pem </dev/null
ssh-keygen -q -t ed25519 -m PKCS8 -N '' -f $K/u_ed25519_pkcs8 </dev/null
cat $K/u_*.pub > $WORK/authorized_keys
chmod 644 $WORK/authorized_keys $K/*.pub; chmod 644 $K/u_* $K/n_unauthorized     # the test (root) reads them; they are throw-away keys
fp() { ssh-keygen -lf "$1" | awk '{print $2}'; }
FP_ED=$(fp $K/host_ed25519.pub); FP_E256=$(fp $K/host_ecdsa256.pub); FP_E384=$(fp $K/host_ecdsa384.pub); FP_E521=$(fp $K/host_ecdsa521.pub); FP_RSA=$(fp $K/host_rsa.pub)

# ---- sshd instances ----
PORT=22100
declare -a ENTRIES
start_sshd() {   # name port extra-config-lines...
    local name=$1 port=$2; shift 2
    local cfg=$WORK/cfg/sshd-$port.conf
    {
        for l in "$@"; do echo "$l"; done        # first: sshd keeps the FIRST value of an option, so these override the defaults below
        echo "Port $port"; echo "ListenAddress $BIND"
        echo "PidFile $WORK/cfg/sshd-$port.pid"
        echo "UsePAM no"; echo "PasswordAuthentication yes"; echo "KbdInteractiveAuthentication no"; echo "PubkeyAuthentication yes"
        echo "AuthorizedKeysFile $WORK/authorized_keys"; echo "StrictModes no"; echo "AllowUsers $UNAME"; echo "MaxAuthTries 20"; echo "MaxStartups 100:30:200"
        echo "LogLevel ERROR"
        echo "PermitRootLogin no"; echo "PerSourcePenalties no"
    } > "$cfg"
    $SSHD -D -e -f "$cfg" -E "$WORK/log/sshd-$port.log" &
    PIDS+=($!)
}
ALLHK="HostKey $K/host_ed25519
HostKey $K/host_ecdsa256
HostKey $K/host_ecdsa384
HostKey $K/host_ecdsa521
HostKey $K/host_rsa"
entry() {  # name port kex cipher mac hostkeyalg hostkeyfp
    ENTRIES+=("{\"name\":\"$1\",\"port\":$2,\"expect_kex\":\"$3\",\"expect_cipher\":\"$4\",\"expect_mac\":\"$5\",\"expect_hostkey\":\"$6\",\"host_key\":\"$7\"}")
}
# the main servers: everything the server offers (scp tests, engine round trips)
start_sshd main 22001 "$ALLHK"
MAIN_PORT=22001
# one server per algorithm: the restriction is on the server side, the client keeps its default preference
for kex in curve25519-sha256 curve25519-sha256@libssh.org ecdh-sha2-nistp256 ecdh-sha2-nistp384 ecdh-sha2-nistp521 diffie-hellman-group-exchange-sha256 diffie-hellman-group14-sha256 diffie-hellman-group14-sha1 diffie-hellman-group-exchange-sha1; do
    start_sshd "kex $kex" $PORT "$ALLHK" "KexAlgorithms $kex" "HostKeyAlgorithms ssh-ed25519" "Ciphers aes128-ctr" "MACs hmac-sha2-256"
    entry "kex $kex" $PORT "$kex" aes128-ctr hmac-sha2-256 ssh-ed25519 "$FP_ED"; PORT=$((PORT+1))
done
for c in aes256-gcm@openssh.com aes128-gcm@openssh.com aes256-ctr aes192-ctr aes128-ctr chacha20-poly1305@openssh.com; do
    start_sshd "cipher $c" $PORT "$ALLHK" "Ciphers $c" "HostKeyAlgorithms ssh-ed25519" "KexAlgorithms curve25519-sha256"
    entry "cipher $c" $PORT curve25519-sha256 "$c" "-" ssh-ed25519 "$FP_ED"; PORT=$((PORT+1))
done
for m in hmac-sha2-256-etm@openssh.com hmac-sha2-512-etm@openssh.com hmac-sha2-256 hmac-sha2-512 hmac-sha1-etm@openssh.com hmac-sha1; do
    start_sshd "mac $m" $PORT "$ALLHK" "Ciphers aes256-ctr" "MACs $m" "HostKeyAlgorithms ssh-ed25519" "KexAlgorithms curve25519-sha256"
    entry "mac $m" $PORT curve25519-sha256 aes256-ctr "$m" ssh-ed25519 "$FP_ED"; PORT=$((PORT+1))
done
declare -A HKFILE=( [ssh-ed25519]=host_ed25519 [ecdsa-sha2-nistp256]=host_ecdsa256 [ecdsa-sha2-nistp384]=host_ecdsa384 [ecdsa-sha2-nistp521]=host_ecdsa521 [rsa-sha2-512]=host_rsa [rsa-sha2-256]=host_rsa [ssh-rsa]=host_rsa )
declare -A HKFP=( [ssh-ed25519]=$FP_ED [ecdsa-sha2-nistp256]=$FP_E256 [ecdsa-sha2-nistp384]=$FP_E384 [ecdsa-sha2-nistp521]=$FP_E521 [rsa-sha2-512]=$FP_RSA [rsa-sha2-256]=$FP_RSA [ssh-rsa]=$FP_RSA )
for h in ssh-ed25519 ecdsa-sha2-nistp256 ecdsa-sha2-nistp384 ecdsa-sha2-nistp521 rsa-sha2-512 rsa-sha2-256 ssh-rsa; do
    start_sshd "hostkey $h" $PORT "HostKey $K/${HKFILE[$h]}" "HostKeyAlgorithms $h" "KexAlgorithms curve25519-sha256"
    entry "hostkey $h" $PORT curve25519-sha256 "*" "*" "$h" "${HKFP[$h]}"; PORT=$((PORT+1))
done
# an ESXi 6.x like server: only the old algorithms
start_sshd legacy $PORT "HostKey $K/host_rsa" "HostKeyAlgorithms ssh-rsa" "KexAlgorithms diffie-hellman-group14-sha1" "Ciphers aes128-ctr" "MACs hmac-sha1" "PubkeyAcceptedAlgorithms +ssh-rsa"
entry "legacy esxi6" $PORT diffie-hellman-group14-sha1 aes128-ctr hmac-sha1 ssh-rsa "$FP_RSA"; LEGACY_PORT=$PORT; PORT=$((PORT+1))
# a server that asks for a key exchange every 128 KiB (server-initiated rekeying)
start_sshd rekey $PORT "$ALLHK" "RekeyLimit 128K" "HostKeyAlgorithms ssh-ed25519"
REKEY_PORT=$PORT; PORT=$((PORT+1))

# ---- servers with unusual behaviour (same account, same home): limits, forced commands, noisy shells, read-only,
#      a tiny disk (tmpfs, real ENOSPC), a missing mv.  Each one is a servers.json key the tests look at ("x.<name>").
cat > $WORK/wrap_err.sh <<'EOW'
#!/bin/sh
echo "Welcome to the noisy host (stderr)" >&2
echo "Last login: yesterday" >&2
exec /bin/sh -c "${SSH_ORIGINAL_COMMAND:-/bin/true}"
EOW
cat > $WORK/wrap_out.sh <<'EOW'
#!/bin/sh
echo "Welcome to the noisy host (stdout, like a .bashrc that echoes)"
exec /bin/sh -c "${SSH_ORIGINAL_COMMAND:-/bin/true}"
EOW
cat > $WORK/wrap_nomv.sh <<'EOW'
#!/bin/sh
case "$SSH_ORIGINAL_COMMAND" in mv\ *) echo "sh: 1: mv: not found" >&2; exit 127;; esac
exec /bin/sh -c "${SSH_ORIGINAL_COMMAND:-/bin/true}"
EOW
cat > $WORK/wrap_slowlogin.sh <<'EOW'
#!/bin/sh
sleep 2
exec /bin/sh -c "${SSH_ORIGINAL_COMMAND:-/bin/true}"
EOW
chmod 755 $WORK/wrap_*.sh
mkdir -p "$WORK/home/small"; chown "$UNAME" "$WORK/home/small"
[ "${VM2B_SERVERS_ONLY:-0}" = 1 ] && { SMALL_OFF=1; rmdir "$WORK/home/small"; }
[ -z "${SMALL_OFF:-}" ] && mount -t tmpfs -o size=1m,mode=0777 tmpfs "$WORK/home/small" 2>/dev/null || echo "note: cannot mount a small tmpfs, the disk-full tests are skipped"
XENTRIES=(); XPORTS=()
xentry() { # name port extra-config... (starts an sshd; adds the scp_<name> server)
    local name=$1 port=$2; shift 2
    start_sshd "x $name" $port "HostKey $K/host_ed25519" "$@"
    XPORTS+=($port)
    XENTRIES+=("\"scp_$name\":{\"type\":\"scp\",\"host\":\"$SH\",\"port\":$port,\"user\":\"$UNAME\",\"password\":\"$UPASS\",\"host_key\":\"$FP_ED\"}")
}
xentry maxsess1 22301 "MaxSessions 1"
xentry forced 22303 "ForceCommand /bin/true"
xentry noise_err 22304 "ForceCommand $WORK/wrap_err.sh"
xentry noise_out 22305 "ForceCommand $WORK/wrap_out.sh"
xentry nomv 22306 "ForceCommand $WORK/wrap_nomv.sh"
xentry slowlogin 22308 "ForceCommand $WORK/wrap_slowlogin.sh"
xentry nopass 22309 "PasswordAuthentication no"
xentry idle 22310 "UnusedConnectionTimeout 3" "ChannelTimeout session=3"
xentry smallgrace 22311 "LoginGraceTime 3"
xentry fewauth 22312 "MaxAuthTries 1"
xentry maxstart 22313 "MaxStartups 2:100:2"
xentry authlog 22315 "LogLevel VERBOSE"       # the log of this server is read by the lock-out guard tests (attempts of the client are counted)
xentry rekey64k 22314 "Ciphers aes128-ctr" "KexAlgorithms diffie-hellman-group14-sha256" "MACs hmac-sha2-256" "RekeyLimit 64K 1"
# ---- paramiko servers (password / keyboard-interactive // fault injection / hostile / corrupting relay) ----
PM_JSON=""
if [ -f "$HERE/tools/testservers/pm_server.py" ]; then
    python3 "$HERE/tools/testservers/pm_server.py" --root "$WORK/pm" --user "$UNAME" --password "$UPASS" --base 22900 --sshd-port $MAIN_PORT --keys "$K" --info "$WORK/pm/info.json" >"$WORK/log/pm.log" 2>&1 &
    PIDS+=($!)
    for i in $(seq 1 50); do [ -f "$WORK/pm/info.json" ] && break; sleep 0.2; done
    [ -f "$WORK/pm/info.json" ] && PM_JSON=$(cat "$WORK/pm/info.json")
fi

# wait for the ports
for p in 22001 $(seq 22100 $((PORT-1))) "${XPORTS[@]}"; do
    for i in $(seq 1 50); do (exec 3<>/dev/tcp/$BIND/$p) 2>/dev/null && break; sleep 0.1; done
done

W() {      # a Linux path as the test program sees it (Wine: Z:<path>; the Windows VM: the UNC path of the shared folder, VM2B_GUEST_MAP)
    if [ -n "${VM2B_GUEST_MAP:-}" ]; then local hp=${VM2B_GUEST_MAP%%=*} gp=${VM2B_GUEST_MAP#*=}; case "$1" in "$hp"/*|"$hp") echo "$gp$(echo "${1#$hp}" | tr / '\\')" | sed 's/\\/\\\\/g'; return;; esac; fi    # backslashes doubled: the value goes into a JSON string
    echo "Z:$1"
}
{
    echo "{"
    echo "\"ssh\":{\"host\":\"$SH\",\"user\":\"$UNAME\",\"password\":\"$UPASS\",\"passphrase\":\"$PASSPH\",\"keys\":\"$(W $K)\",\"root\":\"$(W $WORK/home)\",\"root_unix\":\"$WORK/home\",\"legacy_port\":$LEGACY_PORT,\"rekey_port\":$REKEY_PORT,"
    echo " \"hostkeys\":{\"ed25519\":\"$FP_ED\",\"ecdsa256\":\"$FP_E256\",\"ecdsa384\":\"$FP_E384\",\"ecdsa521\":\"$FP_E521\",\"rsa\":\"$FP_RSA\"}},"
    echo "\"scp\":{\"type\":\"scp\",\"host\":\"$SH\",\"port\":$MAIN_PORT,\"user\":\"$UNAME\",\"password\":\"$UPASS\",\"host_key\":\"$FP_ED\"},"
    echo "\"scp_rekey\":{\"type\":\"scp\",\"host\":\"$SH\",\"port\":$REKEY_PORT,\"user\":\"$UNAME\",\"password\":\"$UPASS\",\"host_key\":\"$FP_ED\"},"
    for e in "${XENTRIES[@]}"; do echo "$e,"; done
    echo "\"x\":{\"small_dir\":\"small\",\"authlog\":\"$(W $WORK/log/sshd-22315.log)\"},"
    [ -n "$PM_JSON" ] && echo "\"pm\":$PM_JSON,"
    printf '"ssh_matrix":['; first=1; for e in "${ENTRIES[@]}"; do [ $first = 1 ] || printf ','; first=0; printf '%s' "$e"; done; echo ']'
    echo "}"
} > "$WORK/servers.json"

EXTRA=()
[ -n "${MINUTES:-}" ] && EXTRA+=(--minutes "$MINUTES")
touch "$WORK/ready"
if [ "${VM2B_SERVERS_ONLY:-0}" = 1 ]; then echo "servers only: $WORK/servers.json"; while true; do sleep 1; done; fi
echo "servers: $WORK/servers.json ($((${#ENTRIES[@]})) matrix entries)"
wine64 "$EXE" --servers "Z:$WORK/servers.json" --only ssh,scp "${EXTRA[@]}" "$@"
RC=$?
exit $RC
