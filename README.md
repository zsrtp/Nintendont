### Nintendont (decompgz/decompimizer Fork)

A fork of [Nintendont](https://github.com/FIX94/Nintendont) with added support for [decompgz](https://github.com/zsrtp/decompgz) and (soon) [decompimizer](https://github.com/zsrtp/decompimizer).

### What this fork adds

* **Custom EXI device for settings persistence** — the game can save, load, and delete tpgz settings to the SD card via DMA without going through the memory card subsystem.
* **Status read-back** — the kernel reports actual success/failure of SD operations back to the game, so the UI shows accurate error messages even when the SD card is missing.
* **`f_unlink` support in kernel FatFS** — lowered `_FS_MINIMIZE` from 2 to 0 so the kernel can properly delete files (otherwise it just wipes the files).
* **EXI patching always enabled** — required for the custom EXI device to intercept TPGZ traffic on memory card slot A.
* **Game-callable exit to loader** — exposes a `DoGameExit` entry point at `0x93000008` so the game mod can programmatically return to the Homebrew Channel (e.g. from a menu option) without requiring the button combo.

### Building

Nintendont requires specific older devkitPro toolchain versions. **Newer versions will compile but produce binaries that hang on exit (return to loader).**

Required versions:
* **devkitARM r53-1** (GCC 9.1.0)
* **devkitPPC r35-1** (GCC 8.3.0)
* **libogc 1.8.23-1**

These are no longer available from the official devkitPro package repos. You can download them from the community archive at https://wii.leseratte10.de/devkitPro/:
* [devkitARM r53](https://wii.leseratte10.de/devkitPro/devkitARM/r53%20(2019-06)/) — `-linux`, `-osx`, or `-windows` variant
* [devkitPPC r35](https://wii.leseratte10.de/devkitPro/devkitPPC/r35/) — `-linux`, `-osx`, or `-windows` variant
* [libogc 1.8.23](https://wii.leseratte10.de/devkitPro/libogc/libogc_1.8.23%20(2019-10-02)/) — `-any` (platform-independent)

The `.pkg.tar.xz` files are just compressed tarballs containing an `opt/devkitpro/` directory tree. Remove any existing devkitPro toolchains and extract:

**Linux / macOS:**
```bash
sudo tar xf devkitARM-r53-1-linux.pkg.tar.xz -C /
sudo tar xf devkitPPC-r35-1-linux.pkg.tar.xz -C /
sudo tar xf libogc-1.8.23-1-any.pkg.tar.xz -C /
```
On macOS, replace `-linux` with `-osx` in the filenames.

**Windows (MSYS2):**
```bash
tar xf devkitARM-r53-1-windows.pkg.tar.xz -C /
tar xf devkitPPC-r35-1-windows.pkg.tar.xz -C /
tar xf libogc-1.8.23-1-any.pkg.tar.xz -C /
```

Then build:
```bash
export DEVKITPRO=/opt/devkitpro
export DEVKITPPC=$DEVKITPRO/devkitPPC
export DEVKITARM=$DEVKITPRO/devkitARM
make clean && make
```

The output is `loader/loader.dol`.

### TODO

* Support dynamic file paths for writing (so the game can define them instead of a hardcoded path).
  * Add more protections around file deletions to make sure we don't accidentally wipe the wrong thing after dynamic file paths are supported
