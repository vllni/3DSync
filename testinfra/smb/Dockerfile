FROM docker.io/library/alpine:3.22
RUN apk add --no-cache samba-server samba-common-tools samba-client
COPY smb.conf /etc/samba/smb.conf
COPY entrypoint.sh /entrypoint.sh
ENTRYPOINT ["/bin/sh", "/entrypoint.sh"]
