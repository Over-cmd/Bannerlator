# proot, built from the Termux fork

These are **Android/bionic** binaries. They do not run inside the Linux runtime — they are what
*creates* it, so they execute on the Android side and travel in the rootfs tarball only because that
is how they reach a device without an app reinstall being able to replace them.

    proot             PRoot, aarch64, bionic — Termux fork v5.1.107.92
    loader            its freestanding loader (PROOT_LOADER)
    libtalloc.so.2    proot links against it; shipped under its SONAME, which is what the loader
                      asks for. A file called libtalloc.so is one it will never find.

Built by `.github/workflows/build-proot.yml` from
<https://github.com/termux/proot> `v5.1.107.92` with the NDK, against Termux's own talloc, with
`RUNPATH $ORIGIN` so libtalloc is found beside proot. The copies here come from that workflow and
are refreshed from its artifact; they are checked in so the rootfs build does not depend on a
previous run.

## Why not `app/src/main/cpp/proot`

That tree is an old snapshot of upstream proot 5.1.0 and **cannot exec anything on a current
Android**: `proot error: execve(...): Bad address`. Bisected on device, the fault is in the binary
and not the loader — our loader paired with a working proot runs fine. Both report version "5.1.0",
because the fork keeps upstream's base version string, which is exactly why the two were assumed to
be the same code for far too long. The fork is thousands of lines ahead in the ptrace and exec core.

## Licence

PRoot is **GPL-2.0-or-later**, compatible with this project's GPL-3.0. libtalloc is
LGPL-3.0-or-later. Corresponding source:

  * <https://github.com/termux/proot> at tag `v5.1.107.92`
  * talloc as packaged by Termux:
    <https://github.com/termux/termux-packages/tree/master/packages/libtalloc>
