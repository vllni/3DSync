FROM docker.io/library/alpine:3.22
RUN apk add --no-cache vsftpd openssl tini
COPY entrypoint.sh /entrypoint.sh
ENTRYPOINT ["/bin/sh", "/entrypoint.sh"]
