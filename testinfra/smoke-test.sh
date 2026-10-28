#!/bin/sh
# Checks each test server answers the way 3DSync expects, from this machine.
# Usage: ./smoke-test.sh   (reads .env; run after `podman compose up -d`)
set -u
cd "$(dirname "$0")"
[ -f .env ] && . ./.env
H=${HOST_IP:?set HOST_IP in .env}
U="${TEST_USER:-3dsync}:${TEST_PASSWORD:-3dsync}"
FTP=${FTP_PORT:-2121} FTPS=${FTPS_IMPLICIT_PORT:-2990} PLAIN=${FTP_PLAIN_PORT:-2122}
ANON=${FTP_ANON_PORT:-2123} ANON_STOCK=${FTP_ANON_STOCK_PORT:-2124}
DAV=${WEBDAV_HTTP_PORT:-8080} DAVS=${WEBDAV_HTTPS_PORT:-8443}

failed=0
check() { # name, command...
    name=$1; shift
    if out=$("$@" 2>&1); then echo "ok    $name"; else echo "FAIL  $name"; echo "$out" | sed 's/^/      /'; failed=1; fi
}
refused() { ! "$@"; }
status() { # expected-code, curl args...
    want=$1; shift
    got=$(curl -sk -o /dev/null -w '%{http_code}' -u "$U" "$@")
    [ "$got" = "$want" ] || { echo "HTTP $got, wanted $want"; return 1; }
}

tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
echo "3dsync smoke $(date)" > "$tmp/f.txt"
c="curl -sSk -u $U"

check "ftp TLS=require upload"     $c --ssl-reqd --ftp-create-dirs -T "$tmp/f.txt" "ftp://$H:$FTP/smoke/f.txt"
check "ftp TLS=require MDTM"       $c --ssl-reqd -Q "MDTM smoke/f.txt" -o /dev/null "ftp://$H:$FTP/"
check "ftp TLS=none list"          $c "ftp://$H:$FTP/smoke/"
check "ftp active mode list"       $c --ssl-reqd -P - "ftp://$H:$FTP/smoke/"
check "ftp delete"                 $c --ssl-reqd -Q "DELE smoke/f.txt" -o /dev/null "ftp://$H:$FTP/"
check "ftps implicit upload"       $c -T "$tmp/f.txt" "ftps://$H:$FTPS/f.txt"
check "ftps implicit download"     $c -o /dev/null "ftps://$H:$FTPS/f.txt"
check "ftp plain TLS=try upload"   $c --ssl -T "$tmp/f.txt" "ftp://$H:$PLAIN/f.txt"
check "ftp plain TLS=require refused" refused $c --ssl-reqd "ftp://$H:$PLAIN/"

# Anonymous: 3DSync logs in as "anonymous" with an empty password when the
# User= line is missing. Uploads go to pub/, the root is read-only.
a="curl -sSk -u anonymous:"
for tls in "" --ssl --ssl-reqd; do
    check "ftp anon ${tls:-TLS=none} upload"   $a $tls --ftp-create-dirs -T "$tmp/f.txt" "ftp://$H:$ANON/pub/smoke/f.txt"
    check "ftp anon ${tls:-TLS=none} rename"   $a $tls -Q "RNFR pub/smoke/f.txt" -Q "RNTO pub/smoke/g.txt" -o /dev/null "ftp://$H:$ANON/"
    check "ftp anon ${tls:-TLS=none} delete"   $a $tls -Q "DELE pub/smoke/g.txt" -o /dev/null "ftp://$H:$ANON/"
done
check "ftp anon active mode list"  $a --ssl-reqd -P - "ftp://$H:$ANON/pub/"
check "ftp anon refuses test user" refused $c "ftp://$H:$ANON/"
check "ftp refuses anonymous"      refused $a "ftp://$H:$FTP/"
# Stock vsftpd: plaintext works, and AUTH TLS is accepted but the login
# after it is refused, so TLS=try does not get in.
check "ftp anon stock TLS=none upload"   $a --ftp-create-dirs -T "$tmp/f.txt" "ftp://$H:$ANON_STOCK/pub/smoke/f.txt"
check "ftp anon stock TLS=none delete"   $a -Q "DELE pub/smoke/f.txt" -o /dev/null "ftp://$H:$ANON_STOCK/"
check "ftp anon stock TLS=try refused"   refused $a --ssl "ftp://$H:$ANON_STOCK/pub/"

for base in "http://$H:$DAV/dav" "https://$H:$DAVS/dav" "https://$H:$DAVS/dav-nodepth"; do
    check "$base MKCOL"            status 201 -X MKCOL "$base/smoke/"
    check "$base PUT"              status 201 -T "$tmp/f.txt" "$base/smoke/f.txt"
    check "$base PROPFIND depth 1" status 207 -X PROPFIND -H "Depth: 1" "$base/"
    check "$base ETag"             sh -c "curl -sSkI -u '$U' '$base/smoke/f.txt' | grep -qi '^etag:'"
    case $base in
    *nodepth) check "$base PROPFIND infinity refused" status 403 -X PROPFIND -H "Depth: infinity" "$base/" ;;
    *)        check "$base PROPFIND infinity"         status 207 -X PROPFIND -H "Depth: infinity" "$base/" ;;
    esac
    check "$base bad password"     status 401 -u nobody:wrong "$base/"
    check "$base cleanup"          status 204 -X DELETE "$base/smoke/"
done

for base in "http://$H:$DAV/dav-anon" "https://$H:$DAVS/dav-anon"; do
    check "$base MKCOL, no login"  status 201 -u : -X MKCOL "$base/smoke/"
    check "$base PUT, no login"    status 201 -u : -T "$tmp/f.txt" "$base/smoke/f.txt"
    check "$base PROPFIND infinity, no login" status 207 -u : -X PROPFIND -H "Depth: infinity" "$base/"
    check "$base cleanup"          status 204 -u : -X DELETE "$base/smoke/"
done

# No SMB2 client on a stock host, so smbclient runs inside the container,
# which shares the host network.
for share in 3ds 3ds-encrypted; do
    check "smb //$H/$share SMB3 put/ls/rm" podman compose exec -T smb \
        smbclient "//$H/$share" -p "${SMB_PORT:-1445}" -U "${TEST_USER:-3dsync}%${TEST_PASSWORD:-3dsync}" -m SMB3 \
        -c "put /etc/hostname smoke.txt; ls smoke.txt; rm smoke.txt"
done
check "smb //$H/3ds-public as guest put/ls/rm" podman compose exec -T smb \
    smbclient "//$H/3ds-public" -p "${SMB_PORT:-1445}" -N -m SMB3 \
    -c "put /etc/hostname smoke.txt; ls smoke.txt; rm smoke.txt"
check "smb //$H/3ds refuses guest" refused podman compose exec -T smb \
    smbclient "//$H/3ds" -p "${SMB_PORT:-1445}" -N -m SMB3 -c "ls"
check "smb SMB1 refused" refused podman compose exec -T smb \
    smbclient -L "//$H" -p "${SMB_PORT:-1445}" -U "${TEST_USER:-3dsync}%${TEST_PASSWORD:-3dsync}" -m NT1 --option='client min protocol=NT1'

exit $failed
