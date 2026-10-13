//
// FactoryRestore: the stock state, and the walk that puts a ROOTFS1 back to it.
//
#include "factory_restore.h"
#include "core/main.h"

#include <ableem/engine/log.h>
#include <ableem/engine/md5.h>

#include <json.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <set>
#include <sstream>

#ifndef _WIN32
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

using namespace std;

const char *const FactoryRestore::StockBootMd5 = "28ce5f6de4981764411393310311b517";
const char *const FactoryRestore::StockRootfsMd5 = "ca710a128b7da4a23ace840c9d16e745";

namespace {
using Entry = FactoryRestore::Entry;

Entry entryFrom(const nlohmann::json &node) {
    Entry entry;
    entry.type = node.value("type", "");
    entry.mode = static_cast<unsigned>(strtoul(node.value("mode", "0").c_str(), nullptr, 8));
    entry.uid = node.value("uid", 0);
    entry.gid = node.value("gid", 0);
    entry.md5 = node.value("md5", "");
    entry.target = node.value("target", "");
    entry.size = node.value("size", static_cast<uint64_t>(0));
    return entry;
}

bool readJson(const string &path, nlohmann::json &json) {
    string text;
    if (!DirEntry::readFile(path, text)) {
        PLOG_ERROR << "Cannot read " << path;
        return false;
    }
    json = nlohmann::json::parse(text, nullptr, false);
    if (json.is_discarded() || !json.is_object()) {
        PLOG_ERROR << path << " is not valid JSON";
        return false;
    }
    return true;
}

string joinPath(const string &root, const string &path) { // root + "/etc/x"
    string base = root;
    while (!base.empty() && base.back() == '/')
        base.pop_back();
    return base + path;
}

#ifndef _WIN32
bool lstatOf(const string &path, struct stat &st) {
    return lstat(path.c_str(), &st) == 0;
}

// a file, link or folder gone with everything in it; a folder that is a mount point of its own (another device)
// is left alone
void removeTree(const string &path, dev_t dev, int &removed) {
    struct stat st;
    if (!lstatOf(path, st))
        return;
    if (S_ISDIR(st.st_mode)) {
        if (st.st_dev != dev)
            return;
        if (DIR *dir = opendir(path.c_str())) {
            vector<string> names;
            while (dirent *item = readdir(dir)) {
                const string name = item->d_name;
                if (name != "." && name != "..")
                    names.push_back(name);
            }
            closedir(dir);
            for (const string &name : names)
                removeTree(path + "/" + name, dev, removed);
        }
        if (rmdir(path.c_str()) == 0)
            removed++;
    } else if (unlink(path.c_str()) == 0) {
        removed++;
    }
}

void setMetadata(const string &path, const Entry &entry, const struct stat &st) {
    if (S_ISLNK(st.st_mode)) {
        if (static_cast<int>(st.st_uid) != entry.uid || static_cast<int>(st.st_gid) != entry.gid)
            if (lchown(path.c_str(), static_cast<uid_t>(entry.uid), static_cast<gid_t>(entry.gid)) != 0) {
            } // best effort: not root on a dev host
        return;
    }
    if (static_cast<int>(st.st_uid) != entry.uid || static_cast<int>(st.st_gid) != entry.gid)
        if (chown(path.c_str(), static_cast<uid_t>(entry.uid), static_cast<gid_t>(entry.gid)) != 0) {
        }
    // chown clears setuid/setgid, so the mode comes after it
    if ((st.st_mode & 07777) != entry.mode || entry.mode & 06000)
        if (chmod(path.c_str(), entry.mode) != 0) {
        }
}

class Scanner {
public:
    Scanner(const string &root, const FactoryRestore::Manifest &manifest, const ableem::ByteProgress &bytes,
            vector<string> &foreign, vector<string> &problems)
        : root_(root), manifest_(manifest), bytes_(bytes), foreign_(foreign), problems_(problems) {
        for (const auto &item : manifest.entries)
            total_ += item.second.size;
    }

    void run() {
        struct stat st;
        if (!lstatOf(root_, st) || !S_ISDIR(st.st_mode)) {
            problems_.push_back("the root " + root_ + " is not a folder");
            return;
        }
        dev_ = st.st_dev;
        scan(root_, "");
        for (const auto &item : manifest_.entries) {
            if (seen_.count(item.first))
                continue;
            bool shadowed = false;
            for (const string &mount : mounts_)
                if (item.first.compare(0, mount.size() + 1, mount + "/") == 0)
                    shadowed = true;
            if (!shadowed)
                problems_.push_back("missing " + item.first);
        }
    }

private:
    void scan(const string &dir, const string &rel) {
        vector<string> names;
        if (DIR *handle = opendir(dir.c_str())) {
            while (dirent *item = readdir(handle)) {
                const string name = item->d_name;
                if (name != "." && name != "..")
                    names.push_back(name);
            }
            closedir(handle);
        } else {
            problems_.push_back("cannot read " + (rel.empty() ? string("/") : rel));
            return;
        }
        sort(names.begin(), names.end());
        for (const string &name : names) {
            const string childRel = rel + "/" + name;
            const string child = dir + "/" + name;
            struct stat st;
            if (!lstatOf(child, st))
                continue;
            auto known = manifest_.entries.find(childRel);
            if (known == manifest_.entries.end()) {
                // a mount point of another filesystem that the stock tree does not list is not ours to delete
                if (!(S_ISDIR(st.st_mode) && st.st_dev != dev_))
                    foreign_.push_back(childRel);
                continue;
            }
            seen_.insert(childRel);
            const Entry &entry = known->second;
            if (entry.type == "dir") {
                if (!S_ISDIR(st.st_mode)) {
                    problems_.push_back("not a folder: " + childRel);
                } else if (st.st_dev != dev_) {
                    mounts_.push_back(childRel); // another filesystem mounted over it: not looked into
                } else {
                    scan(child, childRel);
                }
            } else if (entry.type == "link") {
                char target[4096];
                ssize_t length = S_ISLNK(st.st_mode) ? readlink(child.c_str(), target, sizeof(target) - 1) : -1;
                if (length < 0 || string(target, static_cast<size_t>(length)) != entry.target)
                    problems_.push_back("not the stock link: " + childRel);
            } else if (entry.type == "file") {
                if (!S_ISREG(st.st_mode) || static_cast<uint64_t>(st.st_size) != entry.size) {
                    problems_.push_back("not the stock file: " + childRel);
                } else {
                    const uint64_t before = done_;
                    string md5 = ableem::Md5::ofFile(child, [&](uint64_t got, uint64_t) {
                        if (bytes_)
                            bytes_(before + got, total_);
                    });
                    done_ += entry.size;
                    if (md5 != entry.md5)
                        problems_.push_back("not the stock file: " + childRel);
                }
            }
        }
    }

    string root_;
    const FactoryRestore::Manifest &manifest_;
    const ableem::ByteProgress &bytes_;
    vector<string> &foreign_, &problems_;
    set<string> seen_;
    vector<string> mounts_;
    dev_t dev_ = 0;
    uint64_t total_ = 0, done_ = 0;
};
#endif

// the folder `src` copied to `dst` with everything in it
bool copyTree(const string &src, const string &dst) {
    if (!DirEntry::createDirs(dst))
        return false;
    bool ok = true;
    for (const DirEntry &entry : DirEntry::diru(src)) {
        if (entry.isDir)
            ok = copyTree(src + sep + entry.name, dst + sep + entry.name) && ok;
        else
            ok = DirEntry::copyFile(src + sep + entry.name, dst + sep + entry.name) && ok;
    }
    return ok;
}

string trimmed(const string &text) {
    size_t from = text.find_first_not_of(" \t\r\n");
    if (from == string::npos)
        return "";
    return text.substr(from, text.find_last_not_of(" \t\r\n") - from + 1);
}

// the value of a "Name:   value" line of tune2fs -l
string valueOf(const string &output, const string &name) {
    istringstream lines(output);
    string line;
    while (getline(lines, line)) {
        if (line.compare(0, name.size(), name) == 0 && line.size() > name.size() && line[name.size()] == ':')
            return trimmed(line.substr(name.size() + 1));
    }
    return "";
}
} // namespace

//*******************************
// Manifest / Eris
//*******************************
bool FactoryRestore::Manifest::load(const string &jsonPath) {
    nlohmann::json json;
    if (!readJson(jsonPath, json) || !json.contains("entries") || !json["entries"].is_object())
        return false;
    entries.clear();
    for (auto item = json["entries"].begin(); item != json["entries"].end(); ++item)
        entries[item.key()] = entryFrom(item.value());
    return !entries.empty();
}

bool FactoryRestore::Eris::load(const string &jsonPath) {
    nlohmann::json json;
    if (!readJson(jsonPath, json))
        return false;
    detect = json.value("detect", vector<string>());
    remove = json.value("remove", vector<string>());
    restoreText = json.value("restore_text", map<string, string>());
    restoreOther.clear();
    if (json.contains("restore_other") && json["restore_other"].is_object())
        for (auto item = json["restore_other"].begin(); item != json["restore_other"].end(); ++item)
            restoreOther[item.key()] = entryFrom(item.value());
    return !detect.empty() && !remove.empty();
}

bool FactoryRestore::Eris::installedIn(const string &root) const {
    for (const string &path : detect)
        if (DirEntry::exists(joinPath(root, path)))
            return true;
    return false;
}

//*******************************
// FactoryRestore::validateStock
//*******************************
bool FactoryRestore::validateStock(const string &stockDir, const string &dataDir) {
    const string image = stockDir + sep + "boot.img";
    if (!DirEntry::exists(image) || ableem::Md5::ofFile(image) != StockBootMd5) {
        PLOG_ERROR << image << " is not Sony's kernel image";
        return false;
    }
    Manifest manifest;
    Eris eris;
    return manifest.load(dataDir + sep + "rootfs1_stock_manifest.json") && eris.load(dataDir + sep + "eris_reverse.json");
}

//*******************************
// FactoryRestore::checkTree
//*******************************
void FactoryRestore::checkTree(const string &root, const Manifest &manifest, const ableem::ByteProgress &bytes,
                               vector<string> &foreign, vector<string> &problems) {
#ifndef _WIN32
    Scanner(root, manifest, bytes, foreign, problems).run();
#else
    (void)root;
    (void)manifest;
    (void)bytes;
    (void)foreign;
    problems.push_back("not supported on this host");
#endif
}

//*******************************
// FactoryRestore::restoreEris
//*******************************
FactoryRestore::Result FactoryRestore::restoreEris(const string &root, const Manifest &manifest, const Eris &eris,
                                                   const ableem::ByteProgress &bytes) {
    Result result;
#ifdef _WIN32
    (void)root;
    (void)manifest;
    (void)eris;
    (void)bytes;
    result.problem = "not supported on this host";
    return result;
#else
    struct stat rootStat;
    if (!lstatOf(root, rootStat) || !S_ISDIR(rootStat.st_mode)) {
        result.problem = "the root " + root + " is not a folder";
        return result;
    }
    // 1. what Eris added
    for (const string &path : eris.remove) {
        if (manifest.entries.count(path)) {
            result.problem = "Eris data lists the stock path " + path + " as removable";
            return result;
        }
        removeTree(joinPath(root, path), rootStat.st_dev, result.removed);
    }
    // 2. the files it rewrote, from their stock text, with the manifest's mode and owner
    for (const auto &item : eris.restoreText) {
        auto stock = manifest.entries.find(item.first);
        if (stock == manifest.entries.end()) {
            result.problem = "Eris data restores " + item.first + " that the manifest does not know";
            return result;
        }
        const Entry &entry = stock->second;
        const string path = joinPath(root, item.first);
        DirEntry::createDirs(DirEntry::getDirNameFromPath(path));
        const string temp = path + ".ab-restore";
        {
            ofstream out(temp, ios::binary | ios::trunc);
            out << item.second;
            if (!out) {
                result.problem = "cannot write " + item.first;
                unlink(temp.c_str());
                return result;
            }
        }
        if (rename(temp.c_str(), path.c_str()) != 0) {
            result.problem = "cannot replace " + item.first;
            unlink(temp.c_str());
            return result;
        }
        struct stat st;
        if (lstatOf(path, st))
            setMetadata(path, entry, st);
        result.restored++;
    }
    // 3. what it removed or replaced that is not text
    for (const auto &item : eris.restoreOther) {
        const Entry &entry = item.second;
        if (entry.type != "link" || entry.target.empty()) {
            result.problem = "Eris data restores " + item.first + " as something other than a link";
            return result;
        }
        const string path = joinPath(root, item.first);
        removeTree(path, rootStat.st_dev, result.removed);
        DirEntry::createDirs(DirEntry::getDirNameFromPath(path));
        if (symlink(entry.target.c_str(), path.c_str()) != 0) {
            result.problem = "cannot link " + item.first;
            return result;
        }
        struct stat st;
        if (lstatOf(path, st))
            setMetadata(path, entry, st);
        result.restored++;
    }
    // 4. the whole tree against the stock manifest; foreign paths are deleted only if everything stock checks out
    vector<string> foreign, problems;
    checkTree(root, manifest, bytes, foreign, problems);
    if (!problems.empty()) {
        for (const string &problem : problems)
            PLOG_ERROR << "Not the stock ROOTFS1: " << problem;
        result.problem = to_string(problems.size()) + " stock paths cannot be restored from here, first: " + problems[0];
        return result;
    }
    for (const string &path : foreign) {
        PLOG_INFO << "Deleting " << path << " (not in the stock ROOTFS1)";
        removeTree(joinPath(root, path), rootStat.st_dev, result.removed);
    }
    // 5. modes and owners the manifest has differently (best effort)
    for (const auto &item : manifest.entries) {
        const string path = joinPath(root, item.first);
        struct stat st;
        if (lstatOf(path, st) && st.st_dev == rootStat.st_dev)
            setMetadata(path, item.second, st);
    }
    result.ok = true;
    return result;
#endif
}

//*******************************
// the saves
//*******************************
bool FactoryRestore::backupSaves(const string &dataDir, const string &destDir) {
    if (!DirEntry::createDirs(destDir))
        return false;
    const string pcsx = dataDir + sep + "AppData" + sep + "sony" + sep + "pcsx";
    bool ok = true;
    for (const DirEntry &game : DirEntry::diru_DirsOnly(pcsx)) {
        for (const char *kind : {"memcards", "sstates"}) {
            const string from = pcsx + sep + game.name + sep + ".pcsx" + sep + kind;
            if (DirEntry::isDirectory(from))
                ok = copyTree(from, destDir + sep + game.name + sep + ".pcsx" + sep + kind) && ok;
        }
    }
    return ok;
}

bool FactoryRestore::restoreSaves(const string &srcDir, const string &dataDir) {
    const string pcsx = dataDir + sep + "AppData" + sep + "sony" + sep + "pcsx";
    bool ok = true;
    for (const DirEntry &game : DirEntry::diru_DirsOnly(srcDir)) {
        for (const char *kind : {"memcards", "sstates"}) {
            const string from = srcDir + sep + game.name + sep + ".pcsx" + sep + kind;
            if (DirEntry::isDirectory(from))
                ok = copyTree(from, pcsx + sep + game.name + sep + ".pcsx" + sep + kind) && ok;
        }
    }
    return ok;
}

vector<string> FactoryRestore::foreignFolders(const string &dataDir) {
    vector<string> found;
    for (const DirEntry &entry : DirEntry::diru_DirsOnly(dataDir)) {
        string lower = entry.name;
        transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return tolower(c); });
        if (lower == "autobleem" || lower == "eris" || lower == "bleemsync")
            found.push_back(entry.name);
    }
    sort(found.begin(), found.end());
    return found;
}

//*******************************
// FactoryRestore::mkfsCommand
//*******************************
string FactoryRestore::mkfsCommand(const string &tune2fsOutput, const string &device) {
    const string label = valueOf(tune2fsOutput, "Filesystem volume name");
    const string features = valueOf(tune2fsOutput, "Filesystem features");
    const long blockSize = atol(valueOf(tune2fsOutput, "Block size").c_str());
    const long inodeSize = atol(valueOf(tune2fsOutput, "Inode size").c_str());
    const double blocks = atof(valueOf(tune2fsOutput, "Block count").c_str());
    const double reserved = atof(valueOf(tune2fsOutput, "Reserved block count").c_str());
    if (blockSize <= 0 || inodeSize <= 0 || blocks <= 0 || features.empty())
        return "";
    // "none" first: only the features the factory filesystem had, not mke2fs.conf's defaults
    string list = "none";
    istringstream words(features);
    string word;
    while (words >> word) {
        bool plain = true;
        for (char c : word)
            plain = plain && (isalnum(static_cast<unsigned char>(c)) || c == '_');
        if (plain && word != "needs_recovery")
            list += "," + word;
    }
    char percent[32];
    snprintf(percent, sizeof(percent), "%.2f", reserved * 100.0 / blocks);
    string command = "mkfs.ext4 -F -q -b " + to_string(blockSize) + " -I " + to_string(inodeSize) + " -m " + percent +
                     " -O " + list;
    // a label is a name, not a shell word: only the characters a label has
    bool plainLabel = !label.empty() && label != "<none>" && label.size() <= 16;
    for (char c : label)
        plainLabel = plainLabel && (isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.');
    if (plainLabel)
        command += " -L " + label;
    return command + " " + device;
}

//*******************************
// the names
//*******************************
string FactoryRestore::folderName(long long when) {
    time_t stamp = static_cast<time_t>(when);
    char text[32];
    tm local = {};
    const tm *parts = localtime_r(&stamp, &local); // the console's own date
    if (!parts || strftime(text, sizeof(text), "%Y-%m-%d", parts) == 0)
        return "factory";
    return string("factory-") + text;
}

string FactoryRestore::rootfsName(Rootfs rootfs) {
    switch (rootfs) {
    case Rootfs::Stock:
        return "stock";
    case Rootfs::Eris:
        return "Project Eris";
    case Rootfs::Unknown:
        break;
    }
    return "unknown";
}
