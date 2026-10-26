#!/bin/sh
# VM2Backup Windows test VM: run-os.sh x86|w10|w11   (container vm2b-win, ONE VM at a time; see docs/WINDOWS-TESTS.md)
#   x86 : Windows 10 x86 (WIN10X86.iso) - plain qemu inside the dockurr/windows image (dockur has no 32-bit drivers)
#   w10 : Windows 10 x64 ru business (dockur)      w11 : Windows 11 5in1 25H2 (dockur)
# Disk in .winvm/storage-<os> (removed by destroy.sh); exchange: .winvm/shared/{jobs,results,pkg}; web console http://127.0.0.1:8008
# (dockur only), VNC 127.0.0.1:5908; container port 18443 -> guest 8443 (x86).  Returns at once; the install takes 30-60 min
# (wait-ready.sh [seconds] waits until the executor is ready).
cd "$(dirname "$0")" || exit 1
W=$PWD
ISOD=/home/symo/Downloads/2222222/ISO
case "$1" in
  x86) ;;
  w10) ISO="ru-ru_windows_10_business_editions_version_22h2_updated_oct_2023_x64_dvd_6072cf93.iso"; RAM=4G ;;
  w11) ISO="Windows_11_5in1_25H2_26200_9445_2026_09_x64.iso"; RAM=4G ;;
  *) echo "usage: run-os.sh x86|w10|w11"; exit 1 ;;
esac
if docker ps -a --format '{{.Names}}' | grep -qx vm2b-win; then echo "container vm2b-win exists: run destroy.sh first"; exit 1; fi
FREE=$(df -BG --output=avail /home | tail -1 | tr -dc 0-9)
[ "$FREE" -ge 30 ] || { echo "only ${FREE} GB free on /home"; exit 1; }
rm -rf "$W/shared/results" "$W/shared/jobs"; mkdir -p "$W/storage-$1" "$W/shared/jobs/done" "$W/shared/results" "$W/shared/pkg"
if [ "$1" = x86 ]; then
  (cd x86/answer && genisoimage -quiet -J -r -V ANSWER -o ../answer.iso .) || exit 1
  docker run -d --name vm2b-win --device /dev/kvm --stop-timeout 60 -p 127.0.0.1:5908:5900 -p 127.0.0.1:18443:18443 -p 127.0.0.1:18444:18444 \
    -v "$W/x86:/x:ro" -v "$W/shared:/shared" -v "$W/storage-x86:/s" -v "$ISOD/WIN10X86.iso:/boot.iso:ro" \
    --entrypoint bash dockurr/windows /x/qemu-run.sh
else
  docker run -d --name vm2b-win -e RAM_SIZE=$RAM -e CPU_CORES=4 -e DISK_SIZE=32G \
    --device=/dev/kvm --device=/dev/net/tun --cap-add NET_ADMIN --stop-timeout 120 \
    -p 127.0.0.1:8008:8006 -p 127.0.0.1:5908:5900 \
    -v "$W/storage-$1:/storage" -v "$W/shared:/shared" -v "$W/oem:/oem" -v "$ISOD/$ISO:/boot.iso:ro" dockurr/windows
fi
echo "$1" > "$W/current-os"
date +%s > "$W/start-time"
