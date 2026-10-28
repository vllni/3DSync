FROM docker.io/library/httpd:2.4-alpine
# DavLockDB needs the DBM driver apr-util defaults to, packaged separately.
RUN apk add --no-cache openssl apr-util-dbm_gdbm
COPY httpd.conf /usr/local/apache2/conf/httpd.conf
COPY entrypoint.sh /entrypoint.sh
ENTRYPOINT ["/bin/sh", "/entrypoint.sh"]
