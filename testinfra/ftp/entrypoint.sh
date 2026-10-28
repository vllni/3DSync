#!/bin/sh
# One image, three servers: FTP_MODE picks plain (no AUTH TLS offered),
# explicit (AUTH TLS) or implicit (TLS from the first byte). All three share
# the host network, so each listens on its own FTP_LISTEN_PORT.
set -eu
: "${FTP_MODE:?}" "${FTP_LISTEN_PORT:?}" "${HOST_IP:?}" "${PASV_MIN:?}" "${PASV_MAX:?}"
: "${TEST_USER:=3dsync}" "${TEST_PASSWORD:=3dsync}"

if ! id "$TEST_USER" >/dev/null 2>&1; then
    adduser -D -h /srv/ftp "$TEST_USER"
fi
echo "$TEST_USER:$TEST_PASSWORD" | chpasswd
mkdir -p /srv/ftp && chown "$TEST_USER" /srv/ftp

# 3DSync turns off peer verification, but the SAN keeps desktop clients
# that do check from tripping over the address.
if [ ! -f /etc/ssl/ftp/cert.pem ]; then
    mkdir -p /etc/ssl/ftp
    openssl req -x509 -newkey rsa:2048 -nodes -days 3650 -subj "/CN=3dsync-test" \
        -addext "subjectAltName=IP:$HOST_IP,DNS:localhost" \
        -keyout /etc/ssl/ftp/key.pem -out /etc/ssl/ftp/cert.pem 2>/dev/null
fi

# FTP_ANONYMOUS=yes makes it an anonymous-only server: no password asked,
# full write access, and the test user cannot log in at all.
# vsftpd will not chroot an anonymous session into a writable root, so the
# root is read-only and uploads go to pub/ (Path=pub in 3DSync.ini).
if [ "${FTP_ANONYMOUS:-no}" = yes ]; then
    chown root /srv/ftp && chmod 755 /srv/ftp
    mkdir -p /srv/ftp/pub && chown ftp /srv/ftp/pub
    AUTH_CONF="anonymous_enable=YES
local_enable=NO
no_anon_password=YES
anon_root=/srv/ftp
anon_upload_enable=YES
anon_mkdir_write_enable=YES
anon_other_write_enable=YES
anon_umask=022
# Stock vsftpd refuses AUTH TLS from anonymous users (530), which 3DSync's
# default TLS=try runs straight into. FTP_ANON_SSL=no keeps that default.
allow_anon_ssl=$(echo "${FTP_ANON_SSL:-yes}" | tr a-z A-Z)"
else
    AUTH_CONF="anonymous_enable=NO
local_enable=YES
local_umask=022
chroot_local_user=YES"
fi

cat > /etc/vsftpd/vsftpd.conf <<CONF
listen=YES
listen_ipv6=NO
background=NO
seccomp_sandbox=NO
$AUTH_CONF
write_enable=YES
allow_writeable_chroot=YES
use_localtime=NO
pasv_enable=YES
pasv_address=$HOST_IP
pasv_min_port=$PASV_MIN
pasv_max_port=$PASV_MAX
port_enable=YES
connect_from_port_20=NO
xferlog_enable=YES
xferlog_std_format=NO
log_ftp_protocol=YES
vsftpd_log_file=/proc/1/fd/1
CONF

case "$FTP_MODE" in
plain)
    echo "listen_port=$FTP_LISTEN_PORT" >> /etc/vsftpd/vsftpd.conf
    ;;
explicit|implicit)
    cat >> /etc/vsftpd/vsftpd.conf <<CONF
ssl_enable=YES
rsa_cert_file=/etc/ssl/ftp/cert.pem
rsa_private_key_file=/etc/ssl/ftp/key.pem
ssl_ciphers=HIGH
require_ssl_reuse=NO
CONF
    if [ "$FTP_MODE" = implicit ]; then
        printf 'listen_port=%s\nimplicit_ssl=YES\nforce_local_logins_ssl=YES\nforce_local_data_ssl=YES\n' "$FTP_LISTEN_PORT" \
            >> /etc/vsftpd/vsftpd.conf
    else
        # Plaintext stays allowed so TLS=none and TLS=try both have something to hit.
        printf 'listen_port=%s\nforce_local_logins_ssl=NO\nforce_local_data_ssl=NO\n' "$FTP_LISTEN_PORT" \
            >> /etc/vsftpd/vsftpd.conf
    fi
    ;;
*)
    echo "unknown FTP_MODE=$FTP_MODE" >&2
    exit 1
    ;;
esac

# Not as PID 1: vsftpd exits along with the first TLS session there.
exec tini -- vsftpd /etc/vsftpd/vsftpd.conf
