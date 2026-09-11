#!/bin/sh
set -eu

OUT=${OUT:-/out}
DROPBEAR_VERSION=2024.86
OPENSSH_VERSION=9.9p2
DROPBEAR_URL=https://matt.ucc.asn.au/dropbear/releases/dropbear-${DROPBEAR_VERSION}.tar.bz2
OPENSSH_URL=https://cdn.openbsd.org/pub/OpenBSD/OpenSSH/portable/openssh-${OPENSSH_VERSION}.tar.gz
SFTP_PATH=/mnt/SDCARD/App/MiyooFin/tools/dropbear/bin/sftp-server

mkdir -p "$OUT" /tmp/src
cd /tmp/src
curl -fsSLO "$DROPBEAR_URL"
curl -fsSLO "$OPENSSH_URL"
tar -xjf "dropbear-${DROPBEAR_VERSION}.tar.bz2"
tar -xzf "openssh-${OPENSSH_VERSION}.tar.gz"

cd "openssh-${OPENSSH_VERSION}"
CC=arm-linux-gnueabihf-gcc \
LDFLAGS='-static -L/usr/lib/arm-linux-gnueabihf' \
LIBS='-lcrypto -ldl -lpthread' \
./configure --host=arm-linux-gnueabihf --build=x86_64-linux-gnu \
    --without-pam --without-selinux --without-libedit --without-kerberos5 \
    --without-ldns --without-security-key-builtin --disable-strip \
    --with-ssl-dir=/usr/arm-linux-gnueabihf
make -j"$(getconf _NPROCESSORS_ONLN)" sftp-server
install -m 0755 sftp-server "$OUT/sftp-server"

cd "/tmp/src/dropbear-${DROPBEAR_VERSION}"
./configure --host=arm-linux-gnueabihf --build=x86_64-linux-gnu \
    --enable-static --disable-zlib --disable-lastlog --disable-utmp \
    --disable-utmpx --disable-wtmp --disable-wtmpx --disable-loginfunc \
    --disable-shadow --disable-syslog

CFLAGS='-DSFTPSERVER_PATH=\"/mnt/SDCARD/App/MiyooFin/tools/dropbear/bin/sftp-server\" -DDROPBEAR_SFTPSERVER=1' \
make -j"$(getconf _NPROCESSORS_ONLN)" PROGRAMS='dropbear'
install -m 0755 dropbear "$OUT/dropbear"

arm-linux-gnueabihf-readelf -h "$OUT/dropbear" "$OUT/sftp-server" | grep -E 'File|Class|Machine'
arm-linux-gnueabihf-ldd "$OUT/dropbear" 2>/dev/null || true
arm-linux-gnueabihf-ldd "$OUT/sftp-server" 2>/dev/null || true
sha256sum "$OUT/dropbear" "$OUT/sftp-server"
