#!/bin/sh
set -eu
: "${TEST_USER:=3dsync}" "${TEST_PASSWORD:=3dsync}" "${SMB_PORT:=1445}"

if ! id "$TEST_USER" >/dev/null 2>&1; then
    adduser -D -H "$TEST_USER"
fi
sed -i "s/valid users = .*/valid users = $TEST_USER/; s/force user = .*/force user = $TEST_USER/; s/smb ports = .*/smb ports = $SMB_PORT/" \
    /etc/samba/smb.conf
mkdir -p /srv/smb/3ds /srv/smb/3ds-encrypted /srv/smb/3ds-public
chown "$TEST_USER" /srv/smb/3ds /srv/smb/3ds-encrypted /srv/smb/3ds-public
printf '%s\n%s\n' "$TEST_PASSWORD" "$TEST_PASSWORD" | smbpasswd -a -s "$TEST_USER" >/dev/null

exec smbd --foreground --no-process-group --debug-stdout
