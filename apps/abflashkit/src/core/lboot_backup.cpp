//
// LbootBackup: the recovery backup's shape and checks.
//
#include "lboot_backup.h"
#include "lboot_signature.h"
#include "core/main.h"

#include <ableem/engine/md5.h>

#include <fstream>

using namespace std;

const char *const LbootBackup::VanillaBootMd5 = "28ce5f6de4981764411393310311b517";
const char *const LbootBackup::VanillaRootfsMd5 = "ca710a128b7da4a23ace840c9d16e745";

//*******************************
// LbootBackup::partitionsFor*
//*******************************
vector<LbootBackup::Partition> LbootBackup::partitionsForFlash() {
    return {{"/dev/disk/by-partlabel/BOOTIMG1", "boot.img"},
            {"/dev/disk/by-partlabel/USRDATA", "userdata.ext4"},
            {"/dev/disk/by-partlabel/TEE1", "tz.img"}};
}

vector<LbootBackup::Partition> LbootBackup::partitionsForFullBackup() {
    return {{"/dev/disk/by-partlabel/BOOTIMG1", "boot.img"},
            {"/dev/disk/by-partlabel/ROOTFS1", "rootfs.ext4"},
            {"/dev/disk/by-partlabel/USRDATA", "userdata.ext4"},
            {"/dev/disk/by-partlabel/TEE1", "tz.img"}};
}

//*******************************
// LbootBackup::appendSignature / isAutoBleemBackup
//*******************************
bool LbootBackup::appendSignature(const string &path) {
    ofstream out(path, ios::binary | ios::app);
    if (!out)
        return false;
    out.write(reinterpret_cast<const char *>(lboot::Signature), lboot::SignatureSize);
    return out.good();
}

bool LbootBackup::isAutoBleemBackup(const string &path) {
    ifstream in(path, ios::binary | ios::ate);
    if (!in || in.tellg() < 9)
        return false;
    in.seekg(-9, ios::end);
    char tail[9];
    in.read(tail, 9);
    return in.gcount() == 9 && string(tail, 9) == "autobleem";
}

//*******************************
// LbootBackup::inspect
//*******************************
LbootBackup::Contents LbootBackup::inspect(const string &extractedDir) {
    return inspect(extractedDir, ableem::ByteProgress());
}

LbootBackup::Contents LbootBackup::inspect(const string &extractedDir, const ableem::ByteProgress &bytes) {
    Contents contents;
    string boot = extractedDir + sep + "boot.img";
    string rootfs = extractedDir + sep + "rootfs.ext4";
    contents.hasBoot = DirEntry::exists(boot);
    contents.hasRootfs = DirEntry::exists(rootfs);
    // one count over both images: the boot image's bytes, then the rootfs's after them
    const uint64_t bootSize = contents.hasBoot ? static_cast<uint64_t>(DirEntry::fileSize(boot)) : 0;
    const uint64_t total = bootSize + (contents.hasRootfs ? static_cast<uint64_t>(DirEntry::fileSize(rootfs)) : 0);
    auto from = [&bytes, total](uint64_t before) -> ableem::ByteProgress {
        if (!bytes)
            return ableem::ByteProgress();
        return [&bytes, before, total](uint64_t done, uint64_t) { bytes(before + done, total); };
    };
    if (contents.hasBoot)
        contents.bootIsVanilla = ableem::Md5::ofFile(boot, from(0)) == VanillaBootMd5;
    if (contents.hasRootfs)
        contents.rootfsIsVanilla = ableem::Md5::ofFile(rootfs, from(bootSize)) == VanillaRootfsMd5;
    return contents;
}
