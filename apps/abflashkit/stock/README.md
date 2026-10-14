# Stock PlayStation Classic kernel image

`boot.img` is the console's original boot partition image, exactly as Sony shipped it (md5
`28ce5f6de4981764411393310311b517`). It is a U-Boot FIT image ("Yocto aud Baseline/4.4/aiv8167-rockman-emmc") that
holds only a Linux kernel (lz4) and its flattened device tree. There is no ramdisk. ABFlashKit's "Restore to factory"
writes it back to BOOTIMG1 (AUTOBLEEM-32).

Licence: the Linux kernel and its device tree are GPL-2.0. The corresponding source is Sony's Linux source release,
which is the first commit of https://github.com/autobleem2/psc-kernel (33772ce, "Initial commit"). This binary was
built by Sony with their own kernel configuration; it is shipped unmodified.

`../data/rootfs1_stock_manifest.json` lists every path of the stock ROOTFS1 (md5
`ca710a128b7da4a23ace840c9d16e745`) with its type, mode and md5 or link target. It holds hashes only, no file
contents. `../data/eris_reverse.json` describes what Project Eris changes in ROOTFS1: the paths it adds, the stock
text of the seven configuration files it rewrites, and the `/sbin/blkid` link it removes.
