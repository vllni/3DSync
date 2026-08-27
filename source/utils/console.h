#ifndef UTILS_CONSOLE_H
#define UTILS_CONSOLE_H

// The CONSOLE_* colour macros come from <3ds.h> on the console.  Everything
// that only needs them for output — the sync engine included — takes them from
// here instead, so the same code compiles and runs in the host test build.
#ifdef __3DS__
#include <3ds.h>
#else
#define CONSOLE_ESC(x)  "\x1b[" #x
#define CONSOLE_RESET   CONSOLE_ESC(0m)
#define CONSOLE_BLACK   CONSOLE_ESC(30m)
#define CONSOLE_RED     CONSOLE_ESC(31;1m)
#define CONSOLE_GREEN   CONSOLE_ESC(32;1m)
#define CONSOLE_YELLOW  CONSOLE_ESC(33;1m)
#define CONSOLE_BLUE    CONSOLE_ESC(34;1m)
#define CONSOLE_MAGENTA CONSOLE_ESC(35;1m)
#define CONSOLE_CYAN    CONSOLE_ESC(36;1m)
#define CONSOLE_WHITE   CONSOLE_ESC(37;1m)
#endif

#endif
