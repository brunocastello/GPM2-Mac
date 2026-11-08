# Grand Prix Manager 2 for Mac

> [!CAUTION]
> ### 🤖 AI-Generated Code Ahead
> **This entire repository was crafted with the help of AI.**
>
> If you are allergic to neural networks, synthesized functions, or autocomplete on steroids, **this repository is not for you**.
> You are kindly invited to close this page and write your code line-by-line in `ed` or `vim`. Everyone else, enjoy!

A native port of *Grand Prix Manager 2* (MicroProse, 1996) to the Mac. The original Windows
executable, version 1.02B, is statically recompiled to C and runs on a small Win32 runtime, so the
game plays and looks exactly like the original, with some of its bugs fixed. Windows, menus and
dialogs are native to each system.

It builds for:

| System | Build |
|---|---|
| macOS 11 or later (Intel or Apple Silicon) | `Grand Prix Manager 2.app`, universal |
| Mac OS X 10.4 and 10.5 (PowerPC) | `GPM2-ppc.iso` |
| Mac OS 9 (PowerPC, CarbonLib) | `GPM2.dsk` |

This repository has no game files. You need your own copy of *Grand Prix Manager 2* patched to
1.02B: the build reads `GPM2.EXE` 1.02B (SHA-256 `266f2147…3546`), and the app asks for the game
folder the first time it opens.

## Building on a Mac

You need Xcode's command line tools and Python 3 with Capstone (`pip3 install capstone==5.0.6`).

```sh
python3 -m recomp f1 /path/to/GPM2/GPM2.EXE gen/      # executable -> C, in gen/
make -C runtime app                                    # runtime/build/Grand Prix Manager 2.app
```

`make -C runtime app GAME_DIR=/path/to/GPM2` takes the app icon from the game's `GPM.ICO`.

The PowerPC builds (Tiger and Mac OS 9) need cross compilers on Linux; `runtime/Makefile` and
`runtime/host/os9/CMakeLists.txt` show how they are built. No builds are distributed: everything
built from this repository contains code translated from your own copy of the game.

## Layout

- `recomp/`: the recompiler (Python), with the bug-fix hooks in `recomp/hooks.json`.
- `runtime/`: the Win32 runtime in C and the native front ends (`host/macos`, `host/carbon` for
  Tiger, `host/os9`).
- `tools/`: icon and packaging helpers.
- `docs/plans/`: how the port was built, 01-14 in build order (recompiler, Win32 runtime, macOS,
  Tiger and OS 9 front ends, bug hooks). They are working notes, kept as written.
- `docs/bugs/known-bugs.md`: the catalog of known bugs in the original and which ones are fixed.

A network race (Link Play between two machines) was never tested.

*Grand Prix Manager 2* is © MicroProse. This project is not affiliated with MicroProse or its
successors.
