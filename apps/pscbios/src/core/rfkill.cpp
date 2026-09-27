#include "rfkill.h"
#include "core/main.h"

#include <ableem/engine/filesystem.h>

#include <fstream>

using namespace std;
using ableem::DirEntry;
using ableem::sep;

namespace {
string trimmed(string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
        s.pop_back();
    return s;
}

string readTrimmed(const string &path) {
    ifstream in(path);
    string line;
    getline(in, line);
    return trimmed(line);
}

bool writeFile(const string &path, const string &contents) {
    ofstream out(path);
    if (!out)
        return false;
    out << contents;
    return out.good();
}
} // namespace

RfkillUnblock::Result RfkillUnblock::run() {
    message_.clear();
    if (!DirEntry::exists(root))
        return Result::NotNeeded;
    for (const ableem::DirEntry &entry : DirEntry::diru_DirsOnly(root)) {
        const string base = root + sep + entry.name;
        if (readTrimmed(base + sep + "type") != "bluetooth")
            continue;
        if (readTrimmed(base + sep + "hard") == "1") {
            message_ = _("Bluetooth is switched off by a hardware switch");
            return Result::HardBlocked;
        }
        if (readTrimmed(base + sep + "soft") != "1")
            continue; // this one is not soft-blocked - keep looking, in case another entry is
        if (!writeFile(base + sep + "soft", "0")) {
            message_ = _("Bluetooth's software block could not be cleared");
            return Result::ClearFailed;
        }
        return Result::Cleared;
    }
    return Result::NotNeeded;
}
