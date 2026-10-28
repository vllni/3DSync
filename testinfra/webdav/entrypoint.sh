#!/bin/sh
set -eu
: "${HOST_IP:?}" "${WEBDAV_HTTP_PORT:=8080}" "${WEBDAV_HTTPS_PORT:=8443}"
export WEBDAV_HTTP_PORT WEBDAV_HTTPS_PORT
: "${TEST_USER:=3dsync}" "${TEST_PASSWORD:=3dsync}"

htpasswd -bc /usr/local/apache2/conf/htpasswd "$TEST_USER" "$TEST_PASSWORD" 2>/dev/null
mkdir -p /srv/webdav/dav /srv/webdav/dav-nodepth /srv/webdav/dav-anon /var/lib/dav
chown daemon:daemon /srv/webdav/dav /srv/webdav/dav-nodepth /srv/webdav/dav-anon /var/lib/dav

if [ ! -f /etc/ssl/webdav/cert.pem ]; then
    mkdir -p /etc/ssl/webdav
    openssl req -x509 -newkey rsa:2048 -nodes -days 3650 -subj "/CN=3dsync-test" \
        -addext "subjectAltName=IP:$HOST_IP,DNS:localhost" \
        -keyout /etc/ssl/webdav/key.pem -out /etc/ssl/webdav/cert.pem 2>/dev/null
fi

exec httpd-foreground
