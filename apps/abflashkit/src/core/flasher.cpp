//
// Flasher: the console's commands, and the dev host's stand-in.
//
// 64-bit file offsets on the 32-bit console: a partition is read and written through stdio here
#ifndef _WIN32
#define _FILE_OFFSET_BITS 64
#endif

#include "flasher.h"
#include "core/main.h"
#include "core/services/system.h"

#include <ableem/engine/log.h>
#include <ableem/engine/md5.h>
#include <ableem/engine/zip_archive.h>
#include <ableem/engine/zip_writer.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <thread>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

using namespace std;

const char *const ConsoleFlasher::MiscPartition = "/dev/disk/by-partlabel/MISC";
const char *const ConsoleFlasher::BootPartition = "/dev/disk/by-partlabel/BOOTIMG1";

namespace {
// the backup as both flashers make it: every partition streamed into the zip, then the trailer. The bytes
// are counted over all the partitions together, so one bar covers the whole backup.
bool writeBackup(const vector<LbootBackup::Partition> &partitions, const string &path, const Flasher::Progress &status,
                 const ableem::ByteProgress &bytes) {
    vector<uint64_t> sizes;
    uint64_t total = 0;
    for (const LbootBackup::Partition &partition : partitions) {
        long long size = DirEntry::sizeBySeeking(partition.device);
        sizes.push_back(size > 0 ? static_cast<uint64_t>(size) : 0);
        total += sizes.back();
    }
    bool ok = false;
    {
        ableem::ZipWriter zip;
        ok = zip.open(path);
        uint64_t before = 0; // the bytes of the partitions already in the zip
        for (size_t i = 0; ok && i < partitions.size(); i++) {
            status(_("Zipping:") + " " + partitions[i].entry + "   " + _("Please wait..."));
            ok = zip.addFile(partitions[i].device, partitions[i].entry, [&](uint64_t done, uint64_t) {
                if (bytes)
                    bytes(before + done, total);
            });
            before += sizes[i];
        }
        ok = ok && zip.close();
    }
    ok = ok && LbootBackup::appendSignature(path);
    // a half-written backup must not be taken for one next time: it is what a restore would use
    if (!ok)
        DirEntry::removeFile(path);
    return ok;
}

// the first 32 characters of <kernelDir>/boot.md5 - a bare hash, or md5sum's "<hash>  boot.img"
string expectedKernelMd5(const string &kernelDir) {
    ifstream in(kernelDir + sep + "boot.md5");
    string line;
    getline(in, line);
    return line.size() >= 32 ? line.substr(0, 32) : "";
}

bool kernelIsValid(const string &kernelDir) {
    string image = kernelDir + sep + "boot.img";
    if (!DirEntry::exists(image))
        return false;
    string expected = expectedKernelMd5(kernelDir);
    if (expected.empty())
        return false;
    string actual = ableem::Md5::ofFile(image);
    if (actual != expected) {
        PLOG_WARNING << "Kernel image md5 " << actual << " is not the expected " << expected;
    }
    return actual == expected;
}

bool extractInto(const string &path, const string &dir, const ableem::ByteProgress &bytes) {
    DirEntry::removeDirAndContents(dir);
    DirEntry::createDir(dir);
    return ableem::ZipArchive::extract(path, dir, bytes);
}
} // namespace

//*******************************
// ConsoleFlasher
//*******************************
bool ConsoleFlasher::hasForeignFirmware() {
    return DirEntry::exists("/usr/bin/bleemsync_service") ||
           DirEntry::exists("/etc/systemd/system/project_eris.service");
}

bool ConsoleFlasher::createBackup(const vector<LbootBackup::Partition> &partitions, const string &path,
                                  const Progress &status, const ableem::ByteProgress &bytes) {
    return writeBackup(partitions, path, status, bytes);
}

bool ConsoleFlasher::validateBackup(const string &path) {
    // the firmware's own checker; anything with "fail" in its output is a no
    string result = System::execUnixCommand(("image_verify_tool " + path + " /tmp/validateResult.txt").c_str());
    return result.find("fail") == string::npos;
}

bool ConsoleFlasher::extractBackup(const string &path, const string &dir, const ableem::ByteProgress &bytes) {
    return extractInto(path, dir, bytes);
}

bool ConsoleFlasher::validateKernel(const string &kernelDir) {
    return kernelIsValid(kernelDir);
}

void ConsoleFlasher::setRecoveryMode(bool on, const string &kernelDir) {
    string image = kernelDir + sep + (on ? "recovery-on.img" : "recovery-off.img");
    PLOG_INFO << "Recovery mode " << (on ? "on" : "off");
    System::execUnixCommand(("dd if=" + image + " of=" + MiscPartition).c_str());
}

Flasher::KernelWrite ConsoleFlasher::flashKernel(const string &kernelDir, const ableem::ByteProgress &bytes) {
    PLOG_INFO << "Flashing the kernel image";
    return writeImage(kernelDir + sep + "boot.img", BootPartition, bytes);
}

//*******************************
// ConsoleFlasher::writeImage
//*******************************
// was `dd if=boot.img of=BOOTIMG1`, which reported nothing and whose failure went unnoticed
Flasher::KernelWrite ConsoleFlasher::writeImage(const string &image, const string &device,
                                                const ableem::ByteProgress &bytes) {
    const long long imageSize = DirEntry::fileSize(image);
    const long long deviceSize = DirEntry::sizeBySeeking(device);
    if (imageSize <= 0) {
        PLOG_ERROR << "Kernel image " << image << " is missing or empty";
        return KernelWrite::NothingWritten;
    }
    if (deviceSize < imageSize) {
        PLOG_ERROR << device << " (" << deviceSize << " bytes) cannot hold " << image << " (" << imageSize << ")";
        return KernelWrite::NothingWritten;
    }
    FILE *in = fopen(image.c_str(), "rb");
    if (!in) {
        PLOG_ERROR << "Cannot read " << image;
        return KernelWrite::NothingWritten;
    }
    FILE *out = fopen(device.c_str(), "r+b"); // for update: a device is never truncated, nor created
    if (!out) {
        PLOG_ERROR << "Cannot open " << device << " for writing";
        fclose(in);
        return KernelWrite::NothingWritten;
    }
    const uint64_t total = 2 * static_cast<uint64_t>(imageSize); // the write, then the read-back
    uint64_t done = 0;
    vector<char> buffer(64 * 1024);
    ableem::Md5 written;
    bool ok = true;
    while (ok) {
        size_t got = fread(buffer.data(), 1, buffer.size(), in);
        if (got == 0)
            break;
        written.update(reinterpret_cast<const unsigned char *>(buffer.data()), got);
        if (fwrite(buffer.data(), 1, got, out) != got)
            ok = false;
        done += got;
        if (bytes)
            bytes(done, total);
    }
    if (ferror(in))
        ok = false;
    fclose(in);
    if (fflush(out) != 0)
        ok = false;
#ifndef _WIN32
    // onto the medium, then out of the page cache, so the read-back below reads the flash and not memory
    if (fsync(fileno(out)) != 0)
        ok = false;
    posix_fadvise(fileno(out), 0, 0, POSIX_FADV_DONTNEED);
#endif
    if (fclose(out) != 0)
        ok = false;
    if (!ok) {
        PLOG_ERROR << "Writing " << image << " to " << device << " failed after " << done << " bytes";
        return KernelWrite::Failed;
    }

    FILE *back = fopen(device.c_str(), "rb");
    if (!back) {
        PLOG_ERROR << "Cannot read " << device << " back";
        return KernelWrite::Failed;
    }
    ableem::Md5 readBack;
    uint64_t remaining = static_cast<uint64_t>(imageSize);
    while (remaining > 0) {
        size_t want = remaining < buffer.size() ? static_cast<size_t>(remaining) : buffer.size();
        size_t got = fread(buffer.data(), 1, want, back);
        if (got == 0)
            break;
        readBack.update(reinterpret_cast<const unsigned char *>(buffer.data()), got);
        remaining -= got;
        done += got;
        if (bytes)
            bytes(done, total);
    }
    fclose(back);
    const string expected = written.hexDigest();
    const string actual = readBack.hexDigest();
    if (remaining > 0 || actual != expected) {
        PLOG_ERROR << device << " does not read back as " << image << " (md5 " << actual << ", wrote " << expected
                   << ")";
        return KernelWrite::Failed;
    }
    PLOG_INFO << "Kernel image written and verified, md5 " << actual;
    return KernelWrite::Written;
}

void ConsoleFlasher::installPayload(const string &kernelDir) {
    PLOG_INFO << "Installing the payload";
    System::execUnixCommand(("bash " + kernelDir + sep + "install_payload.sh").c_str());
}

void ConsoleFlasher::sync() {
    System::execUnixCommand("sync");
}

void ConsoleFlasher::reboot() {
    PLOG_INFO << "Rebooting";
    System::execUnixCommand("killall -9 autobleem-gui");
    System::execUnixCommand("systemctl stop weston");
    System::execUnixCommand("systemctl reboot");
}

//*******************************
// FakeFlasher
//*******************************
void FakeFlasher::step(const string &what) {
    PLOG_INFO << "fake flasher: " << what;
    log.push_back(what);
    if (stepDelayMs_ > 0)
        this_thread::sleep_for(chrono::milliseconds(stepDelayMs_));
}

bool FakeFlasher::createBackup(const vector<LbootBackup::Partition> &partitions, const string &path,
                               const Progress &status, const ableem::ByteProgress &bytes) {
    step("backup to " + path);
    if (fakePartitionsDir.empty())
        return writeBackup(partitions, path, status, bytes);
    DirEntry::createDir(DirEntry::getDirNameFromPath(fakePartitionsDir));
    DirEntry::createDir(fakePartitionsDir);
    vector<LbootBackup::Partition> standIns;
    for (const LbootBackup::Partition &partition : partitions) {
        string file = fakePartitionsDir + sep + partition.entry;
        if (DirEntry::fileSize(file) < fakePartitionBytes) {
            // noise, so deflate takes about as long as on a real partition's data
            ofstream out(file, ios::binary);
            string header = "fake " + partition.entry + " standing in for " + partition.device + "\n";
            out << header;
            uint32_t seed = 2463534242u;
            vector<char> chunk(64 * 1024);
            for (long long written = static_cast<long long>(header.size()); written < fakePartitionBytes;
                 written += static_cast<long long>(chunk.size())) {
                for (char &c : chunk) {
                    seed ^= seed << 13;
                    seed ^= seed >> 17;
                    seed ^= seed << 5;
                    c = static_cast<char>(seed & 0xff);
                }
                out.write(chunk.data(), static_cast<streamsize>(chunk.size()));
            }
        }
        standIns.push_back({file, partition.entry});
    }
    return writeBackup(standIns, path, status, bytes);
}

bool FakeFlasher::validateBackup(const string &path) {
    step("validate " + path);
    return true;
}

bool FakeFlasher::extractBackup(const string &path, const string &dir, const ableem::ByteProgress &bytes) {
    step("extract " + path);
    return extractInto(path, dir, bytes);
}

bool FakeFlasher::validateKernel(const string &kernelDir) {
    step("validate kernel in " + kernelDir);
    return kernelIsValid(kernelDir);
}

void FakeFlasher::setRecoveryMode(bool on, const string &) {
    step(string("recovery mode ") + (on ? "on" : "off"));
    recoveryMode = on;
}

Flasher::KernelWrite FakeFlasher::flashKernel(const string &kernelDir, const ableem::ByteProgress &bytes) {
    log.push_back("flash " + kernelDir + sep + "boot.img");
    PLOG_INFO << "fake flasher: " << log.back();
    // the step's delay spread over a pretend write and read-back of the image, so the bar moves
    long long size = DirEntry::fileSize(kernelDir + sep + "boot.img");
    const uint64_t total = 2 * static_cast<uint64_t>(size > 0 ? size : 1);
    const int slices = 20;
    for (int i = 1; i <= slices; i++) {
        if (stepDelayMs_ > 0)
            this_thread::sleep_for(chrono::milliseconds(stepDelayMs_ / slices));
        if (bytes)
            bytes(total * static_cast<uint64_t>(i) / slices, total);
    }
    return kernelWrite;
}

void FakeFlasher::installPayload(const string &kernelDir) {
    step("install payload from " + kernelDir);
}

void FakeFlasher::reboot() {
    step("reboot");
    rebooted = true;
}
