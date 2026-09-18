# proot, prebuilt

These are **Android/bionic** binaries. They do not run inside the Linux runtime — they are what
*creates* it, so they execute on the Android side and are only carried in the rootfs tarball
because that is a convenient way to deliver them.

    proot             PRoot 5.1.0, aarch64, bionic
    loader            its freestanding loader (PROOT_LOADER)
    libtalloc.so.2    proot links against it, so it travels with it

## Why they are prebuilt rather than built here

Our own build of proot (`app/src/main/cpp/proot`) produces a binary that cannot exec anything:

    proot error: execve("/usr/bin/echo"): Bad address

Bisected on device, the fault is in the binary and not the loader — our loader paired with a working
proot runs fine. The clearest lead is that a working proot reports

    built-in accelerators: process_vm = yes, seccomp_filter = yes

and ours reports no accelerators at all, which matters because `process_vm` is how proot writes the
loader into the traced process. Until that is understood, shipping a proot that works beats shipping
one we built, and this directory is the honest way to say so.

## Licence

PRoot is **GPL-2.0-or-later** (`Copyright (C) 2015 STMicroelectronics`), which is compatible with
this project's GPL-3.0. libtalloc is LGPL-3.0-or-later. Both are redistributed unmodified.

Corresponding source:

  * PRoot — https://github.com/proot-me/proot (v5.1.0), as packaged by Termux:
    https://github.com/termux/termux-packages/tree/master/packages/proot
  * talloc — https://gitlab.com/samba-team/samba/-/tree/master/lib/talloc, as packaged by Termux:
    https://github.com/termux/termux-packages/tree/master/packages/libtalloc

Replacing these with our own build is tracked as the open work; nothing else depends on them being
prebuilt.
