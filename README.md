### Nintendont (decompgz/decompimizer Fork)

A fork of [Nintendont](https://github.com/FIX94/Nintendont) with added support for [decompgz](https://github.com/zsrtp/decompgz) and (soon) [decompimizer](https://github.com/zsrtp/decompimizer).

### What this fork adds

* **Custom EXI device for settings persistence** — the game can save, load, and delete tpgz settings to the SD card via DMA without going through the memory card subsystem.
* **Status read-back** — the kernel reports actual success/failure of SD operations back to the game, so the UI shows accurate error messages even when the SD card is missing.
* **`f_unlink` support in kernel FatFS** — lowered `_FS_MINIMIZE` from 2 to 0 so the kernel can properly delete files (otherwise it just wipes the files).
* **EXI patching always enabled** — required for the custom EXI device to intercept TPGZ traffic on memory card slot A.

### How it works

tpgz sends DMA transfers on EXI channel 0 (memory card slot A) with a `"GZ"` magic prefix. The Nintendont kernel intercepts these before normal memory card handling and routes them to a custom handler (`kernel/TPGZ.c`) that performs file I/O on the SD card at `/saves/tpgzcfg.bin`.

### Building

```
make -C kernel/
make -C loader/
```

Requires devkitARM and devkitPPC from devkitPro.

### TODO

* Support dynamic file paths for writing (so the game can define them instead of a hardcoded path).
  * Add more protections around file deletions to make sure we don't accidentally wipe the wrong thing after dynamic file paths are supported
