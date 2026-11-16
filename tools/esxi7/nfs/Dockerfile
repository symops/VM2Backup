# Раунд 4: NFS-сервер стенда (nfs-ganesha, пространство пользователя; ядерного nfsd и пакетов на хосте нет и ставить их нельзя)
FROM debian:trixie-slim
RUN apt-get update && apt-get install -y --no-install-recommends nfs-ganesha nfs-ganesha-vfs rpcbind netbase iproute2 procps && rm -rf /var/lib/apt/lists/*
