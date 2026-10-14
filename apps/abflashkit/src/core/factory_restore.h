//
// FactoryRestore: what "Restore to factory" knows about the console's stock state - Sony's boot image and ROOTFS1
// (their md5), the stock ROOTFS1 manifest, Project Eris's changes and how to reverse them, the built-in games'
// saves, the mkfs command that recreates USRDATA. Everything here works on plain folders and strings, so the
// tests run it on a small fake tree; ConsoleFlasher points it at the real mounted partitions.
//
#pragma once

#include <ableem/engine/byte_progress.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

class FactoryRestore {
public:
    static const char *const StockBootMd5;   // Sony's BOOTIMG1 (stock/boot.img)
    static const char *const StockRootfsMd5; // Sony's ROOTFS1 partition

    // what the stock ROOTFS1 has at a path (data/rootfs1_stock_manifest.json)
    struct Entry {
        std::string type; // "file", "link" or "dir"
        unsigned mode = 0;
        int uid = 0, gid = 0;
        std::string md5;    // a file
        std::string target; // a link
        uint64_t size = 0;  // a file
    };
    struct Manifest {
        std::map<std::string, Entry> entries; // by absolute path, "/etc/fstab"
        bool load(const std::string &jsonPath);
    };

    // data/eris_reverse.json: what Project Eris adds and rewrites
    struct Eris {
        std::vector<std::string> detect;                // any of them present = Eris is installed
        std::vector<std::string> remove;                // top paths Eris adds
        std::map<std::string, std::string> restoreText; // the stock text of the files it rewrites
        std::map<std::string, Entry> restoreOther;      // what else it removed (the /sbin/blkid link)
        bool load(const std::string &jsonPath);
        bool installedIn(const std::string &root) const;
    };

    enum class Rootfs { Stock, Eris, Unknown }; // Unknown: BleemSync or anything else - cannot be put back from here

    // what step 1 saw; nothing was written to find it out
    struct Findings {
        bool bootIsStock = false;
        std::string bootMd5;
        Rootfs rootfs = Rootfs::Unknown;
        bool bleemSync = false;                   // /usr/bin/bleemsync_service
        std::vector<std::string> foreignFolders;  // autobleem, Eris, BleemSync on USRDATA
        bool recoveryFlag = false;                // MISC already holds the recovery flag
        bool gamesOnUserData = false;             // /gaadata lives on USRDATA's filesystem: a reformat would erase the games
    };

    // how a rootfs restore went
    struct Result {
        bool ok = false;
        std::string problem;   // for the log: what stopped it
        int removed = 0;       // paths deleted (Eris's and foreign ones)
        int restored = 0;      // files and links written back
    };

    // <stockDir>/boot.img is Sony's image (the stock md5) and <dataDir>'s two JSON files load
    static bool validateStock(const std::string &stockDir, const std::string &dataDir);

    // Project Eris reversed in the ROOTFS1 mounted (read-write) at `root`: its "remove" paths deleted, the stock
    // text files and the /sbin/blkid link written back, then the WHOLE tree checked against the manifest. A path
    // the manifest does not know is deleted; a stock path that is missing, of another type or another md5 cannot
    // be restored from here and stops it (ok false, nothing further written - the foreign paths are only deleted
    // once the whole tree has checked out). `bytes` counts the stock files' bytes read for the md5s.
    static Result restoreEris(const std::string &root, const Manifest &manifest, const Eris &eris,
                              const ableem::ByteProgress &bytes);

    // `root` against the manifest, nothing written: the foreign paths (not in the manifest) are listed in
    // `foreign`, a stock path that is missing or differs in `problems`. Metadata (mode, owner) is not compared.
    static void checkTree(const std::string &root, const Manifest &manifest, const ableem::ByteProgress &bytes,
                          std::vector<std::string> &foreign, std::vector<std::string> &problems);

    // the built-in games' saves: <dataDir>/AppData/sony/pcsx/<id>/.pcsx/{memcards,sstates} copied to `destDir`
    // under <id>/ (nothing there to copy is fine), and back
    static bool backupSaves(const std::string &dataDir, const std::string &destDir);
    static bool restoreSaves(const std::string &srcDir, const std::string &dataDir);

    // the names under `dataDir` of another firmware's folders (any case): autobleem, Eris, BleemSync
    static std::vector<std::string> foreignFolders(const std::string &dataDir);

    // the data partition (mounted at `dataMount`) is in use by the running system, so it cannot be unmounted and
    // made again: `procMounts` (the text of /proc/mounts) has an overlay whose upperdir, workdir or lowerdir is
    // on it (relative "data/..." or absolute "/data/..."), another mount of the same device, a mount whose source
    // is a path on it (a bind mount) or a mount below it. Not mounted at all = not in use.
    static bool dataInUse(const std::string &procMounts, const std::string &dataMount);

    // mkfs.ext4 for USRDATA from the output of `tune2fs -l` or `dumpe2fs -h` (the same "Key: value" header
    // lines, with or without the "dumpe2fs 1.4x" first line) of the current filesystem - label, block and inode
    // size, the features (without needs_recovery) and the reserved share - filling the whole partition; "" when
    // the output lacks what is needed
    static std::string mkfsCommand(const std::string &tune2fsOutput, const std::string &device);

    // factory-YYYY-MM-DD for `when` (a time_t); the console has no battery clock, so the date may be old
    static std::string folderName(long long when);

    static std::string rootfsName(Rootfs rootfs);
};
