# QuickFIX (third-party)

MiniOEMS uses [QuickFIX](https://github.com/quickfix/quickfix) for the FIX session layer. QuickFIX isn't vendored. `scripts/build_quickfix.sh` fetches one pinned upstream commit, applies `mingw.patch`, and builds a static library into `build/_deps/quickfix-install`.

| | |
|---|---|
| Upstream commit | `386ce46e917ae494ab6e90b1be90fd421cdbe3f9` |
| Build options | static library, no SSL, no examples |
| Licence | QuickFIX Software License, Version 1.0 (BSD-style). `mingw.patch` is a modification of QuickFIX source. |

## Why there's a patch

QuickFIX recognises Windows by checking `_MSC_VER`, which only Microsoft's compiler defines. MinGW-w64 builds Windows programs with GCC, so it fell through to the POSIX code (`arpa/inet.h`, `poll.h`, `fcntl`), which doesn't exist on Windows. The patch makes MinGW use QuickFIX's existing Windows code paths: Winsock sockets and `_beginthreadex` threads.

The patch changes nothing on Linux or macOS, because GCC and Clang there don't define `__MINGW32__`.

One detail: QuickFIX's Windows thread code spells the calling convention `_stdcall`, which is Microsoft's spelling. MinGW only accepts that in GNU mode, and this project compiles in strict `-std=c++20`, so the patch uses `__stdcall`, which both compilers accept. QuickFIX itself built fine either way; the problem only showed up when strict-mode code included QuickFIX's headers.

## Verification (Windows 11, MSYS2 MinGW-w64 GCC 16.1)

- **QuickFIX acceptance tests:** 467 of 467 passed. These are scripted FIX sessions covering FIX 4.0 to 5.0 SP2: logon, sequence-number gaps, resend requests, PossDup handling and malformed messages.
- **QuickFIX unit tests:** all pass except 3 checks in `UtilityTests`. Those checks create sockets before any test has called `socket_init()`, and on Windows that means before Winsock's `WSAStartup()`. They fail only because of test order. A standalone check showed the same calls succeed once `socket_init()` has run.
