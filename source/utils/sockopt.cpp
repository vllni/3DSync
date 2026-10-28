// ---------------------------------------------------------------------------
// SO_ERROR on the 3DS
// ---------------------------------------------------------------------------
// libctru's getsockopt() hands back the option value exactly as the SOC
// service wrote it.  For SO_ERROR that is the console's own error number —
// negative, in its own numbering (-26 is EINPROGRESS) — not a newlib errno,
// and after a non-blocking connect() the service still reports that
// EINPROGRESS once the connection is up.
//
// libsmb2 connects that way: connect() → EINPROGRESS, poll() for POLLOUT,
// then getsockopt(SO_ERROR) and any non-zero value is a failed connect.  So
// every SMB connection failed with "Socket connect failed with -26", on a
// console that could reach the server.  devkitPro's own libcurl build skips
// the SO_ERROR check on the 3DS for this reason (its curl-8.4.0.patch wraps
// verifyconnect() in #ifndef __3DS__).
//
// The link wraps getsockopt (-Wl,--wrap in the Makefile) for every library
// in the app: SO_ERROR is converted with libctru's own _net_convert_error(),
// and a leftover EINPROGRESS — the connect POLLOUT just reported as done —
// reads as 0.  Any real error (refused, unreachable, timed out) still comes
// through, now as the errno strerror() knows.  Every other option is
// untouched.
// ---------------------------------------------------------------------------

#include <errno.h>
#include <sys/socket.h>

#include <3ds.h>

#include "debug.h"

extern "C" {

// libctru, soc_common.c: a negative SOC result to -errno.
s32 _net_convert_error(s32 sock_retval);

int __real_getsockopt(int sockfd, int level, int optname, void *optval, socklen_t *optlen);

int __wrap_getsockopt(int sockfd, int level, int optname, void *optval, socklen_t *optlen)
{
    int ret = __real_getsockopt(sockfd, level, optname, optval, optlen);
    if (ret != 0 || level != SOL_SOCKET || optname != SO_ERROR || optval == NULL ||
        optlen == NULL || *optlen < sizeof(int))
        return ret;

    int raw = *(int *)optval;
    if (raw >= 0)
        return ret;

    int err = -_net_convert_error(raw);
    if (err == EINPROGRESS)
        err = 0; // left over from connect(); the caller polled it writable
    debugf("socket %d: SO_ERROR %d from the SOC service, read as %d\n", sockfd, raw, err);
    *(int *)optval = err;
    return ret;
}

} // extern "C"
