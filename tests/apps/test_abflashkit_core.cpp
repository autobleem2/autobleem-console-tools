//
// abflashkit_core: the backup's shape and checks, the fake flasher's files, the kernel write, the games
// backup, and the four sequences run to the end against the fake with a scripted screen - progress bars
// included.
//
#include "doctest/doctest.h"

#include "support/temp_dir.h"

#include "core/factory_restore.h"
#include "core/flash_actions.h"
#include "core/flasher.h"
#include "core/games_backup.h"
#include "core/lboot_backup.h"
#include "core/led.h"

#include <ableem/engine/md5.h>
#include <ableem/engine/zip_archive.h>

#include <string>
#include <vector>

#include <cstdlib>
#include <ctime>
#include <unistd.h>

using std::string;
using std::vector;

//*******************************
// a scripted screen
//*******************************
namespace {
struct ScriptedUi : FlashUi {
    vector<string> statuses;
    vector<string> questions;
    vector<bool> answers; // consumed in order; true when it runs out
    int waited = 0;
    void status(const string &text) override { statuses.push_back(text); }
    bool confirm(const string &question) override {
        questions.push_back(question);
        if (answers.empty())
            return true;
        bool answer = answers.front();
        answers.erase(answers.begin());
        return answer;
    }
    void wait(int ms) override { waited += ms; }
    vector<std::pair<int, int>> bars; // every progress() call
    void progress(int done, int total) override { bars.emplace_back(done, total); }
    bool said(const string &piece) const {
        for (const string &s : statuses)
            if (s.find(piece) != string::npos)
                return true;
        return false;
    }
    // a bar that was shown, ran from 0 to full without going back, and was hidden again at the end
    bool barFilled() const {
        size_t start = bars.size();
        for (size_t i = 0; i < bars.size(); i++) {
            if (bars[i] == std::make_pair(0, FlashKitActions::BarSteps)) {
                start = i;
                break;
            }
        }
        if (start == bars.size())
            return false;
        int last = 0;
        for (size_t i = start + 1; i < bars.size() && bars[i].second > 0; i++) {
            if (bars[i].first < last)
                return false;
            last = bars[i].first;
        }
        return last == FlashKitActions::BarSteps && bars.back() == std::make_pair(0, 0);
    }
};

// a tree with fake partitions, a kernel folder whose boot.md5 matches its boot.img and abrootfs.md5 matches
// its abrootfs.tgz, and the paths
struct Bench {
    Bench() : tmp("abflashkit") {
        tmp.writeFile("parts/boot", "BOOT IMAGE BYTES");
        tmp.writeFile("parts/root", "ROOTFS BYTES");
        tmp.writeFile("parts/user", "USERDATA BYTES");
        tmp.writeFile("parts/tee", "TEE BYTES");
        tmp.writeFile("kernel/boot.img", "the new kernel");
        tmp.writeFile("kernel/boot.md5", ableem::Md5::ofString("the new kernel") + "  boot.img\n");
        tmp.writeFile("kernel/abrootfs.tgz", "the new rootfs overlay");
        tmp.writeFile("kernel/abrootfs.md5", ableem::Md5::ofString("the new rootfs overlay") + "  abrootfs.tgz\n");
        backup = tmp.at("LBOOT.EPB");
        kernelDir = tmp.at("kernel");
        scratch = tmp.at("scratch");
        marker = tmp.at("validlboot");
    }
    vector<LbootBackup::Partition> partitions(bool withRootfs) const {
        vector<LbootBackup::Partition> list = {{tmp.at("parts/boot"), "boot.img"}};
        if (withRootfs)
            list.push_back({tmp.at("parts/root"), "rootfs.ext4"});
        list.push_back({tmp.at("parts/user"), "userdata.ext4"});
        list.push_back({tmp.at("parts/tee"), "tz.img"});
        return list;
    }
    FlashKitPaths paths() const {
        FlashKitPaths paths;
        paths.backup = backup;
        paths.kernelDir = kernelDir;
        paths.scratchDir = scratch;
        paths.validMarker = marker;
        paths.internalGamesDir = tmp.at("gaadata");
        paths.internalDb = tmp.at("internal.db"); // absent: the games are titled "Game <id>"
        paths.gamesBackupDir = tmp.at("stick/Games Backup");
        return paths;
    }
    TempDir tmp;
    string backup, kernelDir, scratch, marker;
};
} // namespace

TEST_CASE("LbootBackup: the trailer makes a backup ours, and the vanilla check reads the extracted files") {
    Bench b;
    FakeFlasher flasher(0);
    ScriptedUi ui;
    REQUIRE(flasher.createBackup(b.partitions(true), b.backup, [&](const string &s) { ui.status(s); }, nullptr));
    CHECK(ui.said("boot.img"));
    CHECK(LbootBackup::isAutoBleemBackup(b.backup));
    b.tmp.writeFile("other.zip", "PK not really autobleem"); // ends in "autobleem" but that is the whole point
    CHECK(LbootBackup::isAutoBleemBackup(b.tmp.at("other.zip")));
    b.tmp.writeFile("plain.zip", "PK something else");
    CHECK_FALSE(LbootBackup::isAutoBleemBackup(b.tmp.at("plain.zip")));
    CHECK_FALSE(LbootBackup::isAutoBleemBackup(b.tmp.at("missing.zip")));

    // the zip still reads despite the trailer, and the images inside are not the stock ones
    REQUIRE(flasher.extractBackup(b.backup, b.scratch, nullptr));
    CHECK(b.tmp.readFile("scratch/boot.img") == "BOOT IMAGE BYTES");
    LbootBackup::Contents contents = LbootBackup::inspect(b.scratch);
    CHECK(contents.hasBoot);
    CHECK_FALSE(contents.bootIsVanilla);
    CHECK(contents.hasRootfs);
    CHECK_FALSE(contents.rootfsIsVanilla);
    CHECK_FALSE(contents.isVanilla());

    LbootBackup::Contents empty = LbootBackup::inspect(b.tmp.at("nowhere"));
    CHECK_FALSE(empty.hasBoot);
    CHECK_FALSE(empty.hasRootfs);
}

TEST_CASE("the partition lists: no rootfs for a flash, the lot for a full backup") {
    vector<LbootBackup::Partition> flash = LbootBackup::partitionsForFlash();
    REQUIRE(flash.size() == 3);
    CHECK(flash[0].device == "/dev/disk/by-partlabel/BOOTIMG1");
    CHECK(flash[0].entry == "boot.img");
    vector<LbootBackup::Partition> full = LbootBackup::partitionsForFullBackup();
    REQUIRE(full.size() == 4);
    CHECK(full[1].entry == "rootfs.ext4");
}

TEST_CASE("the kernel check wants boot.img and a matching boot.md5 in either format") {
    Bench b;
    FakeFlasher flasher(0);
    CHECK(flasher.validateKernel(b.kernelDir));
    b.tmp.writeFile("kernel/boot.md5", ableem::Md5::ofString("the new kernel")); // bare hash
    CHECK(flasher.validateKernel(b.kernelDir));
    b.tmp.writeFile("kernel/boot.md5", "00000000000000000000000000000000");
    CHECK_FALSE(flasher.validateKernel(b.kernelDir));
    b.tmp.writeFile("kernel/boot.md5", "");
    CHECK_FALSE(flasher.validateKernel(b.kernelDir));
    CHECK_FALSE(flasher.validateKernel(b.tmp.at("no-kernel")));
}

TEST_CASE("the kernel check also wants abrootfs.tgz and a matching abrootfs.md5 in either format") {
    Bench b;
    FakeFlasher flasher(0);
    CHECK(flasher.validateKernel(b.kernelDir)); // both boot.img and abrootfs.tgz match to start with
    b.tmp.writeFile("kernel/abrootfs.md5", ableem::Md5::ofString("the new rootfs overlay")); // bare hash
    CHECK(flasher.validateKernel(b.kernelDir));
    b.tmp.writeFile("kernel/abrootfs.md5", "00000000000000000000000000000000"); // mismatching
    CHECK_FALSE(flasher.validateKernel(b.kernelDir));
    b.tmp.writeFile("kernel/abrootfs.md5", ""); // missing .md5 (empty file, same as absent)
    CHECK_FALSE(flasher.validateKernel(b.kernelDir));
    ableem::DirEntry::removeFile(b.tmp.at("kernel/abrootfs.md5"));
    CHECK_FALSE(flasher.validateKernel(b.kernelDir));
    b.tmp.writeFile("kernel/abrootfs.md5", ableem::Md5::ofString("the new rootfs overlay") + "  abrootfs.tgz\n");
    ableem::DirEntry::removeFile(b.tmp.at("kernel/abrootfs.tgz")); // the .tgz itself missing
    CHECK_FALSE(flasher.validateKernel(b.kernelDir));
}

TEST_CASE("flash: backup, validate, recovery on, kernel, payload, recovery off, reboot") {
    Bench b;
    FakeFlasher flasher(0);
    NullLed led;
    ScriptedUi ui;
    FlashKitActions actions(flasher, led, ui, b.paths());
    // the backup path is what the flasher zips the "partitions" of; the fake takes the bench's files
    // through the same createBackup, so point the flash list at them by making the backup first
    REQUIRE(flasher.createBackup(b.partitions(false), b.backup, [](const string &) {}, nullptr));
    flasher.log.clear();

    CHECK(actions.flash() == FlashKitActions::Outcome::Rebooting);
    REQUIRE(ui.questions.size() == 1);
    CHECK(ui.questions[0] == "Start flashing ?");
    CHECK(flasher.log == vector<string>{"validate " + b.backup, "validate kernel in " + b.kernelDir, "recovery mode on",
                                        "flash " + b.kernelDir + "/boot.img", "install payload from " + b.kernelDir,
                                        "recovery mode off", "reboot"});
    CHECK(flasher.rebooted);
    CHECK_FALSE(flasher.recoveryMode); // cleared before the reboot: the console boots normally into AutoBleem
    CHECK(led.mode == LedMode::Off);
    CHECK(ui.said("All done"));
    CHECK(ui.barFilled()); // the kernel write's
}

TEST_CASE("flash: a kernel write that fails keeps the recovery flag on, one that never started clears it") {
    Bench b;
    NullLed led;

    // written part way: the console must come back through Sony's recovery, so the flag stays and the
    // payload is not installed
    FakeFlasher broken(0);
    REQUIRE(broken.createBackup(b.partitions(false), b.backup, [](const string &) {}, nullptr));
    broken.log.clear();
    broken.kernelWrite = Flasher::KernelWrite::Failed;
    ScriptedUi ui;
    FlashKitActions actions(broken, led, ui, b.paths());
    CHECK(actions.flash() == FlashKitActions::Outcome::Failed);
    CHECK(broken.recoveryMode);
    CHECK(broken.rebooted);
    CHECK(ui.said("restore itself from LBOOT.EPB"));
    for (const string &step : broken.log)
        CHECK(step.find("install payload") == string::npos);

    // nothing written: the flag goes off again, no payload either
    FakeFlasher refused(0);
    refused.kernelWrite = Flasher::KernelWrite::NothingWritten;
    ScriptedUi ui2;
    FlashKitActions untouched(refused, led, ui2, b.paths());
    CHECK(untouched.flash() == FlashKitActions::Outcome::Failed);
    CHECK_FALSE(refused.recoveryMode);
    CHECK(refused.rebooted);
    CHECK(ui2.said("Nothing was changed"));
    CHECK(refused.log == vector<string>{"validate " + b.backup, "validate kernel in " + b.kernelDir, "recovery mode on",
                                        "flash " + b.kernelDir + "/boot.img", "recovery mode off", "reboot"});
}

TEST_CASE("writeImage: onto a bigger device, read back; a device too small or no image writes nothing") {
    TempDir tmp("write_image");
    string image(200 * 1024, 'k');
    for (size_t i = 0; i < image.size(); i += 5)
        image[i] = static_cast<char>(i % 251);
    tmp.writeFile("boot.img", image);
    tmp.writeFile("device", string(512 * 1024, 'z'));

    uint64_t lastDone = 0, lastTotal = 0;
    CHECK(ConsoleFlasher::writeImage(tmp.at("boot.img"), tmp.at("device"), [&](uint64_t done, uint64_t total) {
              CHECK(done > lastDone);
              lastDone = done;
              lastTotal = total;
          }) == Flasher::KernelWrite::Written);
    CHECK(lastTotal == 2 * image.size()); // the write and the read-back
    CHECK(lastDone == lastTotal);
    string device = tmp.readFile("device");
    REQUIRE(device.size() == 512 * 1024); // not truncated
    CHECK(device.substr(0, image.size()) == image);
    CHECK(device.substr(image.size()) == string(512 * 1024 - image.size(), 'z'));

    tmp.writeFile("small", string(1024, 's'));
    CHECK(ConsoleFlasher::writeImage(tmp.at("boot.img"), tmp.at("small"), nullptr) ==
          Flasher::KernelWrite::NothingWritten);
    CHECK(tmp.readFile("small") == string(1024, 's'));
    CHECK(ConsoleFlasher::writeImage(tmp.at("absent.img"), tmp.at("device"), nullptr) ==
          Flasher::KernelWrite::NothingWritten);
    CHECK(ConsoleFlasher::writeImage(tmp.at("boot.img"), tmp.at("no-device"), nullptr) ==
          Flasher::KernelWrite::NothingWritten);
    CHECK_FALSE(ableem::DirEntry::exists(tmp.at("no-device"))); // never created
}

TEST_CASE("flash refuses another firmware, stops at no, and reboots without writing when the kernel is bad") {
    Bench b;
    struct ForeignFlasher : FakeFlasher {
        ForeignFlasher() : FakeFlasher(0) {}
        bool hasForeignFirmware() override { return true; }
    } foreign;
    NullLed led;
    ScriptedUi ui;
    FlashKitActions refused(foreign, led, ui, b.paths());
    CHECK(refused.flash() == FlashKitActions::Outcome::Refused);
    CHECK(ui.said("not been restored"));
    CHECK(ui.questions.empty());

    FakeFlasher flasher(0);
    ScriptedUi no;
    no.answers = {false};
    FlashKitActions cancelled(flasher, led, no, b.paths());
    CHECK(cancelled.flash() == FlashKitActions::Outcome::Cancelled);
    CHECK(flasher.log.empty());

    b.tmp.writeFile("kernel/boot.md5", "00000000000000000000000000000000");
    REQUIRE(flasher.createBackup(b.partitions(false), b.backup, [](const string &) {}, nullptr));
    flasher.log.clear();
    ScriptedUi ui2;
    FlashKitActions bad(flasher, led, ui2, b.paths());
    CHECK(bad.flash() == FlashKitActions::Outcome::Failed);
    CHECK(ui2.said("Invalid backup or invalid kernel image"));
    CHECK(flasher.rebooted);
    for (const string &step : flasher.log)
        CHECK(step.find("flash ") == string::npos); // nothing written
    CHECK_FALSE(flasher.recoveryMode);
}

TEST_CASE("fullBackup asks before overwriting and writes the four partitions") {
    Bench b;
    FakeFlasher flasher(0);
    NullLed led;
    ScriptedUi ui;
    FlashKitActions actions(flasher, led, ui, b.paths());
    // no backup yet: no question. (The real partition devices do not exist here, so the write fails and
    // the outcome says so - the sequencing is what is under test.)
    CHECK(actions.fullBackup() == FlashKitActions::Outcome::Failed);
    CHECK(ui.questions.empty());
    CHECK(ui.said("Backup failed"));

    REQUIRE(flasher.createBackup(b.partitions(true), b.backup, [](const string &) {}, nullptr));
    ScriptedUi no;
    no.answers = {false};
    FlashKitActions keep(flasher, led, no, b.paths());
    CHECK(keep.fullBackup() == FlashKitActions::Outcome::Cancelled);
    REQUIRE(no.questions.size() == 1);
    CHECK(no.questions[0] == "Overwrite the existing backup?");
    CHECK(LbootBackup::isAutoBleemBackup(b.backup)); // untouched
}

TEST_CASE("restore: no backup, not ours, a modified one asks twice, the marker skips the inspection") {
    Bench b;
    FakeFlasher flasher(0);
    NullLed led;

    ScriptedUi none;
    FlashKitActions noBackup(flasher, led, none, b.paths());
    CHECK(noBackup.restore() == FlashKitActions::Outcome::Refused);
    CHECK(none.said("No backup found"));

    b.tmp.writeFile("LBOOT.EPB", "someone else's file");
    ScriptedUi foreign;
    FlashKitActions notOurs(flasher, led, foreign, b.paths());
    CHECK(notOurs.restore() == FlashKitActions::Outcome::Refused);
    CHECK(foreign.said("Non AutoBleem Backup"));

    // ours, without a rootfs and with a non-stock kernel: the SOFTBRICK question, the 1.0a question, then
    // the reboot question
    REQUIRE(flasher.createBackup(b.partitions(false), b.backup, [](const string &) {}, nullptr));
    flasher.log.clear();
    ScriptedUi yes;
    FlashKitActions modified(flasher, led, yes, b.paths());
    CHECK(modified.restore() == FlashKitActions::Outcome::Rebooting);
    REQUIRE(yes.questions.size() == 3);
    CHECK(yes.questions[0].find("SOFTBRICK") != string::npos);
    CHECK(yes.questions[1].find("rootfs1") != string::npos);
    CHECK(yes.questions[2] == "Set recovery mode and reboot now?");
    CHECK(flasher.recoveryMode);
    CHECK(flasher.rebooted);
    CHECK(yes.said("Recovery Mode On"));

    // a no at the first question ends it
    FakeFlasher flasher2(0);
    ScriptedUi no;
    no.answers = {false};
    FlashKitActions declined(flasher2, led, no, b.paths());
    CHECK(declined.restore() == FlashKitActions::Outcome::Cancelled);
    CHECK_FALSE(flasher2.rebooted);
    CHECK(no.said("Recovery interrupted"));

    // the marker: straight to the reboot question
    b.tmp.writeFile("validlboot", "");
    FakeFlasher flasher3(0);
    ScriptedUi marked;
    FlashKitActions trusted(flasher3, led, marked, b.paths());
    CHECK(trusted.restore() == FlashKitActions::Outcome::Rebooting);
    REQUIRE(marked.questions.size() == 1);
    CHECK(marked.questions[0] == "Set recovery mode and reboot now?");
    for (const string &step : flasher3.log)
        CHECK(step.find("extract") == string::npos);
}

TEST_CASE("Led names and the null led") {
    NullLed led;
    led.setMode(LedMode::BlinkGreen);
    CHECK(led.mode == LedMode::BlinkGreen);
    CHECK(string(Led::name(LedMode::BlinkGreen)) == "blink green");
    CHECK(string(Led::name(LedMode::Off)) == "off");
}

TEST_CASE("SysfsLed writes the brightness files it is pointed at and stops its thread") {
    TempDir tmp("leds");
    tmp.makeSubDir("red");
    tmp.makeSubDir("green");
    {
        SysfsLed led(tmp.path());
        led.setMode(LedMode::Orange);
        CHECK(tmp.readFile("red/brightness") == "1");
        CHECK(tmp.readFile("green/brightness") == "1");
        led.setMode(LedMode::Red);
        CHECK(tmp.readFile("green/brightness") == "0");
    } // the destructor joins the blinker
    CHECK(true);
}

TEST_CASE("the bars: a full backup fills one over all the partitions, a restore one over unpack and check") {
    Bench b;
    NullLed led;
    FakeFlasher flasher(0);
    flasher.fakePartitionsDir = b.tmp.at("fake-partitions");
    flasher.fakePartitionBytes = 300 * 1024; // several of miniz's 64 KB reads each
    ScriptedUi ui;
    FlashKitActions actions(flasher, led, ui, b.paths());
    CHECK(actions.fullBackup() == FlashKitActions::Outcome::Done);
    CHECK(ui.said("Backup complete"));
    CHECK(ui.barFilled());
    // one line per partition, the bar carried across them rather than restarted
    CHECK(ui.said("boot.img"));
    CHECK(ui.said("tz.img"));
    int restarts = 0;
    for (const auto &bar : ui.bars)
        if (bar == std::make_pair(0, FlashKitActions::BarSteps))
            restarts++;
    CHECK(restarts == 1);

    ScriptedUi restoring;
    FlashKitActions restore(flasher, led, restoring, b.paths());
    CHECK(restore.restore() == FlashKitActions::Outcome::Rebooting);
    CHECK(restoring.barFilled());
}

//*******************************
// the games backup
//*******************************
namespace {
// /gaadata as the console has it: numbered game folders, and a databases folder that is not a game
void makeGaadata(TempDir &tmp) {
    tmp.writeFile("gaadata/1/SCUS-94900.bin", string(150 * 1024, 'a'));
    tmp.writeFile("gaadata/1/SCUS-94900.cue", "FILE \"SCUS-94900.bin\" BINARY");
    tmp.writeFile("gaadata/1/pcsx.cfg", "Bios = SET");
    tmp.writeFile("gaadata/10/SLUS-00594.bin", string(90 * 1024, 'b'));
    tmp.writeFile("gaadata/10/extra/manual.txt", "a sub-folder comes along");
    tmp.writeFile("gaadata/2/SCES-00001.bin", "c");
    tmp.writeFile("gaadata/databases/regional.db", "not a game");
    tmp.makeSubDir("gaadata/7"); // empty: not a game
}
} // namespace

TEST_CASE("GamesBackup: folder names every filesystem takes") {
    CHECK(GamesBackup::folderName("Final Fantasy VII") == "Final Fantasy VII");
    CHECK(GamesBackup::folderName("Metal Gear Solid: VR Missions") == "Metal Gear Solid - VR Missions");
    CHECK(GamesBackup::folderName("What?/Why*") == "What Why");
    CHECK(GamesBackup::folderName("Trailing dots...") == "Trailing dots");
    CHECK(GamesBackup::folderName("  <>  ") == "Game");
    CHECK(GamesBackup::folderName("R4: Ridge Racer Type 4") == "R4 - Ridge Racer Type 4");
}

TEST_CASE("GamesBackup: finds the numbered folders in id order, copies them, skips what is already there") {
    TempDir tmp("games_backup");
    makeGaadata(tmp);
    const string dest = tmp.at("stick/Games Backup");
    vector<GamesBackup::Game> games = GamesBackup::find(tmp.at("gaadata"), tmp.at("no-internal.db"), dest);
    REQUIRE(games.size() == 3);
    CHECK(games[0].id == 1);
    CHECK(games[1].id == 2);
    CHECK(games[2].id == 10);
    CHECK(games[0].title == "Game 1"); // no database to name it
    CHECK(games[0].dest == dest + "/Game 1");
    CHECK(games[0].files.size() == 3);
    CHECK(games[0].bytes == 150 * 1024 + 28 + 10);
    CHECK(games[2].files.size() == 2); // the sub-folder's file too

    const uint64_t all = GamesBackup::bytesToCopy(games);
    CHECK(all == games[0].bytes + games[1].bytes + games[2].bytes);
    vector<string> started;
    uint64_t last = 0, lastTotal = 0;
    REQUIRE(GamesBackup::copy(
        games, [&](const GamesBackup::Game &game, size_t, size_t) { started.push_back(game.title); },
        [&](uint64_t done, uint64_t total) {
            CHECK(done >= last);
            last = done;
            lastTotal = total;
        }));
    CHECK(started == vector<string>{"Game 1", "Game 2", "Game 10"});
    CHECK(lastTotal == all);
    CHECK(last == all);
    CHECK(tmp.readFile("stick/Games Backup/Game 1/SCUS-94900.bin") == string(150 * 1024, 'a'));
    CHECK(tmp.readFile("stick/Games Backup/Game 10/extra/manual.txt") == "a sub-folder comes along");
    CHECK_FALSE(ableem::DirEntry::exists(tmp.at("stick/Games Backup/Game 1/SCUS-94900.bin.part")));
    CHECK(GamesBackup::bytesToCopy(games) == 0);

    // a copy cut short (smaller than the source) is done again, the rest left as it is
    tmp.writeFile("stick/Games Backup/Game 10/SLUS-00594.bin", "short");
    CHECK(GamesBackup::bytesToCopy(games) == 90 * 1024);
    REQUIRE(GamesBackup::copy(games, nullptr, nullptr));
    CHECK(tmp.readFile("stick/Games Backup/Game 10/SLUS-00594.bin") == string(90 * 1024, 'b'));

    CHECK(GamesBackup::find(tmp.at("no-gaadata"), "", dest).empty());
}

TEST_CASE("backupGames: copies with a bar, says so the second time, refuses a console without games") {
    Bench b;
    makeGaadata(b.tmp);
    NullLed led;
    FakeFlasher flasher(0);
    ScriptedUi ui;
    FlashKitActions actions(flasher, led, ui, b.paths());
    CHECK(actions.backupGames() == FlashKitActions::Outcome::Done);
    CHECK(ui.said("Copying: Game 1   (1/3)"));
    CHECK(ui.said("Copying: Game 10   (3/3)"));
    CHECK(ui.said("Games backed up"));
    CHECK(ui.barFilled());
    CHECK(b.tmp.readFile("stick/Games Backup/Game 2/SCES-00001.bin") == "c");
    CHECK(led.mode == LedMode::Green);

    ScriptedUi again;
    FlashKitActions second(flasher, led, again, b.paths());
    CHECK(second.backupGames() == FlashKitActions::Outcome::Done);
    CHECK(again.said("already backed up"));
    CHECK(again.bars.empty());

    Bench empty;
    ScriptedUi none;
    FlashKitActions nothing(flasher, led, none, empty.paths());
    CHECK(nothing.backupGames() == FlashKitActions::Outcome::Refused);
    CHECK(none.said("No built-in games"));
}

TEST_CASE("sizeText") {
    CHECK(FlashKitActions::sizeText(0) == "0 MB");
    CHECK(FlashKitActions::sizeText(700ull * 1024 * 1024) == "700 MB");
    CHECK(FlashKitActions::sizeText(1536ull * 1024 * 1024) == "1.5 GB");
}

//*******************************
// Restore to factory
//*******************************
namespace {
// a small stock tree and the manifest + Eris data that describe it; `eris` makes the tree what Project Eris
// leaves: its files added, the text files rewritten, /sbin/blkid gone, a foreign file written live
struct FactoryBench {
    explicit FactoryBench(bool eris = true) : tmp("factory") {
        tmp.writeFile("root/etc/fstab", "stock fstab");
        tmp.writeFile("root/etc/group", eris ? "eris group" : "stock group\n");
        tmp.writeFile("root/bin/busybox.nosuid", "BUSYBOX");
        tmp.makeSubDir("root/sbin");
        tmp.makeSubDir("root/tmp");
        tmp.writeFile("data/rootfs1_stock_manifest.json",
                      R"({"entries": {
  "/etc": {"type":"dir","mode":"0755","uid":0,"gid":0},
  "/etc/fstab": {"type":"file","mode":"0644","uid":0,"gid":0,"size":11,"md5":")" +
                          ableem::Md5::ofString("stock fstab") + R"("},
  "/etc/group": {"type":"file","mode":"0644","uid":0,"gid":0,"size":12,"md5":")" +
                          ableem::Md5::ofString("stock group\n") + R"("},
  "/bin": {"type":"dir","mode":"0755","uid":0,"gid":0},
  "/bin/busybox.nosuid": {"type":"file","mode":"0755","uid":0,"gid":0,"size":7,"md5":")" +
                          ableem::Md5::ofString("BUSYBOX") + R"("},
  "/sbin": {"type":"dir","mode":"0755","uid":0,"gid":0},
  "/sbin/blkid": {"type":"link","mode":"0777","uid":0,"gid":0,"target":"/bin/busybox.nosuid"},
  "/tmp": {"type":"dir","mode":"01777","uid":0,"gid":0}}})");
        tmp.writeFile("data/eris_reverse.json", R"({
  "detect": ["/usr/bin/project_eris_service"],
  "remove": ["/opt", "/usr/bin/project_eris_service", "/etc/eris.conf"],
  "restore_text": {"/etc/group": "stock group\n"},
  "restore_other": {"/sbin/blkid": {"type":"link","mode":"0777","uid":0,"gid":0,"target":"/bin/busybox.nosuid"}}})");
        if (eris) {
            tmp.writeFile("root/opt/eris/loader", "ERIS");
            tmp.writeFile("root/usr/bin/project_eris_service", "ERIS SERVICE");
            tmp.writeFile("root/etc/eris.conf", "x");
            tmp.writeFile("root/var/log/live.log", "written live"); // not in the manifest
        } else {
            symlink("/bin/busybox.nosuid", tmp.at("root/sbin/blkid").c_str());
        }
        root = tmp.at("root");
        data = tmp.at("data");
    }
    bool loaded() { return manifest.load(data + "/rootfs1_stock_manifest.json") && erisData.load(data + "/eris_reverse.json"); }
    TempDir tmp;
    string root, data;
    FactoryRestore::Manifest manifest;
    FactoryRestore::Eris erisData;
};

string linkTarget(const string &path) {
    char target[256];
    ssize_t n = readlink(path.c_str(), target, sizeof(target) - 1);
    return n < 0 ? "" : string(target, static_cast<size_t>(n));
}
} // namespace

TEST_CASE("the checked-in stock data: Sony's boot image, a manifest and Eris data that agree") {
    const string app = AB_FLASHKIT_DIR;
    CHECK(FactoryRestore::validateStock(app + "/stock", app + "/data"));
    CHECK(ableem::Md5::ofFile(app + "/stock/boot.img") == FactoryRestore::StockBootMd5);
    FactoryRestore::Manifest manifest;
    FactoryRestore::Eris eris;
    REQUIRE(manifest.load(app + "/data/rootfs1_stock_manifest.json"));
    REQUIRE(eris.load(app + "/data/eris_reverse.json"));
    CHECK(manifest.entries.size() > 2000);
    CHECK(manifest.entries.at("/etc/fstab").type == "file");
    CHECK(manifest.entries.at("/sbin/blkid").target == "/bin/busybox.nosuid");
    CHECK(manifest.entries.at("/etc/passwd").mode == 0644);
    CHECK(manifest.entries.at("/bin/busybox.suid").mode == 04755);
    // what Eris adds is not stock; what it rewrites is
    for (const string &path : eris.remove)
        CHECK(manifest.entries.count(path) == 0);
    for (const auto &item : eris.restoreText)
        CHECK(manifest.entries.count(item.first) == 1);
    CHECK(eris.restoreText.size() == 7);
    CHECK(eris.restoreOther.count("/sbin/blkid") == 1);

    // a different boot image, or data files that are not there, are not stock
    TempDir tmp("stock_bad");
    tmp.writeFile("stock/boot.img", "not Sony's");
    CHECK_FALSE(FactoryRestore::validateStock(tmp.at("stock"), app + "/data"));
    CHECK_FALSE(FactoryRestore::validateStock(app + "/stock", tmp.at("nothing")));
}

TEST_CASE("restoreEris: Eris removed, the text back, the link back, foreign paths deleted, the tree equals the manifest") {
    FactoryBench b;
    REQUIRE(b.loaded());
    CHECK(b.erisData.installedIn(b.root));
    uint64_t last = 0;
    FactoryRestore::Result result = FactoryRestore::restoreEris(b.root, b.manifest, b.erisData,
                                                                [&](uint64_t done, uint64_t) { last = done; });
    INFO(result.problem);
    REQUIRE(result.ok);
    CHECK(last > 0);
    CHECK_FALSE(b.erisData.installedIn(b.root));
    CHECK_FALSE(ableem::DirEntry::exists(b.tmp.at("root/opt")));
    CHECK_FALSE(ableem::DirEntry::exists(b.tmp.at("root/etc/eris.conf")));
    CHECK_FALSE(ableem::DirEntry::exists(b.tmp.at("root/var"))); // the live log and its folder
    CHECK(b.tmp.readFile("root/etc/group") == "stock group\n");
    CHECK(linkTarget(b.tmp.at("root/sbin/blkid")) == "/bin/busybox.nosuid");
    CHECK(b.tmp.readFile("root/etc/fstab") == "stock fstab");
    CHECK_FALSE(ableem::DirEntry::exists(b.tmp.at("root/etc/group.ab-restore")));
    vector<string> foreign, problems;
    FactoryRestore::checkTree(b.root, b.manifest, nullptr, foreign, problems);
    CHECK(foreign.empty());
    CHECK(problems.empty());
}

TEST_CASE("restoreEris: a stock file that differs or is missing stops it, and no foreign path is deleted") {
    {
        FactoryBench b;
        REQUIRE(b.loaded());
        b.tmp.writeFile("root/bin/busybox.nosuid", "PATCHED"); // not Sony's, and nothing here can put it back
        FactoryRestore::Result result = FactoryRestore::restoreEris(b.root, b.manifest, b.erisData, nullptr);
        CHECK_FALSE(result.ok);
        CHECK(result.problem.find("/bin/busybox.nosuid") != string::npos);
        CHECK(ableem::DirEntry::exists(b.tmp.at("root/var/log/live.log"))); // nothing further was touched
    }
    {
        FactoryBench b;
        REQUIRE(b.loaded());
        ableem::DirEntry::removeFile(b.tmp.at("root/etc/fstab"));
        FactoryRestore::Result result = FactoryRestore::restoreEris(b.root, b.manifest, b.erisData, nullptr);
        CHECK_FALSE(result.ok);
        CHECK(result.problem.find("/etc/fstab") != string::npos);
        CHECK(ableem::DirEntry::exists(b.tmp.at("root/var/log/live.log")));
    }
    {
        // a stock link pointing elsewhere, and a stock folder that is a file
        FactoryBench b(false);
        REQUIRE(b.loaded());
        unlink(b.tmp.at("root/sbin/blkid").c_str());
        symlink("/bin/other", b.tmp.at("root/sbin/blkid").c_str());
        vector<string> foreign, problems;
        FactoryRestore::checkTree(b.root, b.manifest, nullptr, foreign, problems);
        // restoreEris puts the blkid link back before checking, so only an unrelated link would stop it
        CHECK(problems.size() == 1);
        CHECK(FactoryRestore::restoreEris(b.root, b.manifest, b.erisData, nullptr).ok);
    }
}

TEST_CASE("checkTree: a stock tree has nothing foreign, a stray file is listed, a mount point is not looked into") {
    FactoryBench b(false);
    REQUIRE(b.loaded());
    vector<string> foreign, problems;
    FactoryRestore::checkTree(b.root, b.manifest, nullptr, foreign, problems);
    CHECK(foreign.empty());
    CHECK(problems.empty());
    b.tmp.writeFile("root/etc/stray", "x");
    b.tmp.writeFile("root/tmp/session", "written live"); // in /tmp: not in the manifest either
    FactoryRestore::checkTree(b.root, b.manifest, nullptr, foreign, problems);
    CHECK(foreign == vector<string>{"/etc/stray", "/tmp/session"});
    CHECK(problems.empty());
    // a root that is not there
    foreign.clear();
    FactoryRestore::checkTree(b.tmp.at("nowhere"), b.manifest, nullptr, foreign, problems);
    CHECK_FALSE(problems.empty());
}

TEST_CASE("the saves: <data>/AppData/sony/pcsx/<id>/.pcsx/{memcards,sstates} copied away and back") {
    TempDir tmp("saves");
    tmp.writeFile("data/AppData/sony/pcsx/3/.pcsx/memcards/card1.mcd", "CARD 3");
    tmp.writeFile("data/AppData/sony/pcsx/3/.pcsx/sstates/slot.0", "STATE 3");
    tmp.writeFile("data/AppData/sony/pcsx/3/.pcsx/cheats/not-a-save", "x");
    tmp.writeFile("data/AppData/sony/pcsx/7/.pcsx/memcards/card1.mcd", "CARD 7");
    tmp.writeFile("data/autobleem/config.ini", "x");
    CHECK(FactoryRestore::backupSaves(tmp.at("data"), tmp.at("stick/factory-1")));
    CHECK(tmp.readFile("stick/factory-1/3/.pcsx/memcards/card1.mcd") == "CARD 3");
    CHECK(tmp.readFile("stick/factory-1/3/.pcsx/sstates/slot.0") == "STATE 3");
    CHECK(tmp.readFile("stick/factory-1/7/.pcsx/memcards/card1.mcd") == "CARD 7");
    CHECK_FALSE(ableem::DirEntry::exists(tmp.at("stick/factory-1/3/.pcsx/cheats")));
    CHECK(FactoryRestore::backupSaves(tmp.at("no-data"), tmp.at("stick/factory-2"))); // nothing to copy is fine
    CHECK(ableem::DirEntry::isDirectory(tmp.at("stick/factory-2")));

    // onto a new, empty data partition
    CHECK(FactoryRestore::restoreSaves(tmp.at("stick/factory-1"), tmp.at("newdata")));
    CHECK(tmp.readFile("newdata/AppData/sony/pcsx/3/.pcsx/sstates/slot.0") == "STATE 3");
    CHECK(tmp.readFile("newdata/AppData/sony/pcsx/7/.pcsx/memcards/card1.mcd") == "CARD 7");
}

TEST_CASE("foreign folders on USRDATA, whatever their case") {
    TempDir tmp("foreign");
    tmp.makeSubDir("data/AutoBleem");
    tmp.makeSubDir("data/Eris");
    tmp.makeSubDir("data/bleemsync");
    tmp.makeSubDir("data/AppData");
    tmp.writeFile("data/eris.txt", "a file is not a folder");
    CHECK(FactoryRestore::foreignFolders(tmp.at("data")) == vector<string>{"AutoBleem", "Eris", "bleemsync"});
    CHECK(FactoryRestore::foreignFolders(tmp.at("nowhere")).empty());
}

TEST_CASE("mkfsCommand: the factory label, block and inode size, features and reserve, from tune2fs -l") {
    const string info =
        "tune2fs 1.43.4 (31-Jan-2017)\n"
        "Filesystem volume name:   userdata\n"
        "Filesystem features:      has_journal ext_attr resize_inode dir_index filetype needs_recovery extent "
        "flex_bg sparse_super large_file huge_file dir_nlink extra_isize\n"
        "Block count:              1000000\n"
        "Reserved block count:     50000\n"
        "Block size:               4096\n"
        "Inode size:               256\n";
    const string command = FactoryRestore::mkfsCommand(info, "/dev/disk/by-partlabel/USRDATA");
    CHECK(command == "mkfs.ext4 -F -q -b 4096 -I 256 -m 5.00 -O none,has_journal,ext_attr,resize_inode,dir_index,"
                     "filetype,extent,flex_bg,sparse_super,large_file,huge_file,dir_nlink,extra_isize -L userdata "
                     "/dev/disk/by-partlabel/USRDATA");
    // no label, an unusable label, no output at all
    string none = info;
    none.replace(none.find("userdata"), 8, "<none>");
    CHECK(FactoryRestore::mkfsCommand(none, "/dev/x").find("-L") == string::npos);
    string odd = info;
    odd.replace(odd.find("userdata"), 8, "a;b");
    CHECK(FactoryRestore::mkfsCommand(odd, "/dev/x").find("-L") == string::npos);
    CHECK(FactoryRestore::mkfsCommand("", "/dev/x").empty());
    CHECK(FactoryRestore::mkfsCommand("tune2fs: Bad magic number in super-block\n", "/dev/x").empty());
}

TEST_CASE("the backup folder's name is the date") {
    // the console's local date: the zone is pinned so the host's does not matter
    setenv("TZ", "UTC", 1);
    tzset();
    CHECK(FactoryRestore::folderName(0) == "factory-1970-01-01");
    CHECK(FactoryRestore::folderName(1760443200) == "factory-2025-10-14"); // noon
}

namespace {
struct FactoryRun {
    explicit FactoryRun(bool eris = true) : fb(eris) {
        // the bench's usual pieces plus the factory ones
        FlashKitPaths paths;
        paths.backup = fb.tmp.at("stick/LBOOT.EPB");
        paths.kernelDir = fb.tmp.at("kernel");
        paths.stockDir = fb.tmp.at("stock");
        paths.dataDir = fb.data;
        paths.backupsDir = fb.tmp.at("stick/System/Backups");
        this->paths = paths;
        flasher.findings.bootIsStock = false;
        flasher.findings.rootfs = eris ? FactoryRestore::Rootfs::Eris : FactoryRestore::Rootfs::Stock;
        flasher.findings.foreignFolders = {"AutoBleem", "Eris"};
        flasher.fakeRootfsDir = fb.root;
        flasher.fakeDataDir = fb.tmp.at("usrdata");
        // a backup the action makes itself reads stand-ins, never the host's /dev/disk/by-partlabel
        flasher.fakePartitionsDir = fb.tmp.at("fake-partitions");
        flasher.fakePartitionBytes = 64 * 1024;
        fb.tmp.writeFile("usrdata/AppData/sony/pcsx/5/.pcsx/memcards/card1.mcd", "CARD 5");
        fb.tmp.writeFile("stock/boot.img", "STOCK KERNEL");
        fb.tmp.writeFile("parts/boot", "BOOT");
        fb.tmp.writeFile("parts/user", "USER");
        fb.tmp.writeFile("parts/tee", "TEE");
        fb.tmp.makeSubDir("stick"); // the stick is mounted: LBOOT.EPB is written into an existing folder
        FakeFlasher maker(0);
        maker.createBackup({{fb.tmp.at("parts/boot"), "boot.img"}, {fb.tmp.at("parts/user"), "userdata.ext4"},
                            {fb.tmp.at("parts/tee"), "tz.img"}},
                           paths.backup, [](const string &) {}, nullptr);
    }
    FactoryBench fb;
    FlashKitPaths paths;
    FakeFlasher flasher{0};
    NullLed led;
    ScriptedUi ui;
    FlashKitActions::Outcome run() {
        FlashKitActions actions(flasher, led, ui, paths);
        return actions.restoreFactory();
    }
    bool wrote() const {
        for (const string &step : flasher.log)
            if (step.rfind("recovery mode on", 0) == 0 || step.rfind("flash ", 0) == 0 ||
                step.rfind("restore rootfs", 0) == 0 || step == "reset user data")
                return true;
        return false;
    }
};
} // namespace

TEST_CASE("restoreFactory on Project Eris: inspect, saves, kernel, rootfs, data partition, saves back, reboot") {
    FactoryRun r;
    CHECK(r.run() == FlashKitActions::Outcome::Rebooting);
    // the confirmation shows the findings and what will be done
    REQUIRE(r.ui.questions.size() == 1);
    CHECK(r.ui.questions[0].find("Project Eris") != string::npos);
    CHECK(r.ui.questions[0].find("AutoBleem Eris") != string::npos);
    CHECK(r.ui.questions[0].find("not stock") != string::npos);
    // the saves went to the stick under the date, then back, in the order of the plan
    const vector<string> &log = r.flasher.log;
    REQUIRE(log.size() == 10);
    CHECK(log[0] == "validate stock in " + r.paths.stockDir);
    CHECK(log[1] == "inspect factory state");
    CHECK(log[2].rfind("backup saves to " + r.paths.backupsDir + "/factory-", 0) == 0);
    CHECK(log[3] == "recovery mode on");
    CHECK(log[4] == "flash " + r.paths.stockDir + "/boot.img");
    CHECK(log[5] == "restore rootfs (Project Eris)");
    CHECK(log[6] == "reset user data");
    CHECK(log[7].rfind("restore saves from " + r.paths.backupsDir + "/factory-", 0) == 0);
    CHECK(log[8] == "recovery mode off");
    CHECK(log[9] == "reboot");
    CHECK_FALSE(r.flasher.recoveryMode);
    CHECK(r.flasher.rebooted);
    // the real tree code ran on the fake root, and the saves really travelled
    CHECK(r.fb.tmp.readFile("root/etc/group") == "stock group\n");
    CHECK_FALSE(ableem::DirEntry::exists(r.fb.tmp.at("root/opt")));
    CHECK(r.ui.said("All done"));
    const string day = FactoryRestore::folderName(static_cast<long long>(time(nullptr)));
    CHECK(r.fb.tmp.readFile("stick/System/Backups/" + day + "/5/.pcsx/memcards/card1.mcd") == "CARD 5");
}

TEST_CASE("restoreFactory on a stock ROOTFS1 leaves it alone, and makes the backup when there is none") {
    FactoryRun r(false);
    ableem::DirEntry::removeFile(r.paths.backup);
    CHECK(r.run() == FlashKitActions::Outcome::Rebooting);
    CHECK(r.ui.questions[0].find("System files: stock") != string::npos);
    CHECK(ableem::DirEntry::exists(r.paths.backup));
    CHECK(LbootBackup::isAutoBleemBackup(r.paths.backup));
    bool backedUp = false;
    for (const string &step : r.flasher.log) {
        backedUp = backedUp || step == "backup to " + r.paths.backup;
        CHECK(step.find("restore rootfs (Project Eris)") == string::npos);
    }
    CHECK(backedUp);
    CHECK(r.fb.tmp.readFile("root/etc/fstab") == "stock fstab"); // untouched

    // an existing LBOOT.EPB is kept as it is
    FactoryRun again(false);
    const long long size = ableem::DirEntry::fileSize(again.paths.backup);
    CHECK(again.run() == FlashKitActions::Outcome::Rebooting);
    CHECK(ableem::DirEntry::fileSize(again.paths.backup) == size);
    for (const string &step : again.flasher.log)
        CHECK(step.find("backup to") != 0);
}

TEST_CASE("restoreFactory refuses BleemSync, an unknown rootfs, games on USRDATA and a tool without the stock files") {
    {
        FactoryRun r;
        r.flasher.findings.rootfs = FactoryRestore::Rootfs::Unknown;
        CHECK(r.run() == FlashKitActions::Outcome::Refused);
        CHECK(r.ui.said("stock LBOOT.EPB"));
        CHECK(r.ui.questions.empty());
        CHECK_FALSE(r.wrote());
    }
    {
        FactoryRun r;
        r.flasher.findings.gamesOnUserData = true;
        CHECK(r.run() == FlashKitActions::Outcome::Refused);
        CHECK(r.ui.said("built-in games"));
        CHECK_FALSE(r.wrote());
    }
    {
        FactoryRun r;
        r.flasher.stockValid = false;
        CHECK(r.run() == FlashKitActions::Outcome::Refused);
        CHECK(r.ui.said("missing from this tool"));
        CHECK(r.flasher.log == vector<string>{"validate stock in " + r.paths.stockDir});
    }
}

TEST_CASE("restoreFactory: Circle at the question writes nothing; a saves backup that fails stops before the kernel") {
    {
        FactoryRun r;
        r.ui.answers = {false};
        CHECK(r.run() == FlashKitActions::Outcome::Cancelled);
        CHECK_FALSE(r.wrote());
        for (const string &step : r.flasher.log)
            CHECK(step.find("backup") == string::npos);
    }
    {
        FactoryRun r;
        r.flasher.savesOk = false;
        CHECK(r.run() == FlashKitActions::Outcome::Failed);
        CHECK(r.ui.said("Nothing was changed"));
        CHECK_FALSE(r.wrote());
        CHECK_FALSE(r.flasher.rebooted);
    }
}

TEST_CASE("restoreFactory: an LBOOT.EPB backup that cannot be written stops before anything is written") {
    FactoryRun r(false);
    ableem::DirEntry::removeFile(r.paths.backup);
    r.paths.backup = r.fb.tmp.at("no-such-folder/LBOOT.EPB"); // the stick is not there to write to
    CHECK(r.run() == FlashKitActions::Outcome::Failed);
    CHECK(r.ui.said("Nothing was changed"));
    CHECK_FALSE(r.wrote());
    CHECK_FALSE(r.flasher.rebooted);
    CHECK_FALSE(r.flasher.recoveryMode);
    for (const string &step : r.flasher.log)
        CHECK(step.find("backup saves") == string::npos);
}

TEST_CASE("restoreFactory: the stock path it cannot restore stops it with the Restore Mode advice, nothing further written") {
    FactoryRun r;
    r.fb.tmp.writeFile("root/bin/busybox.nosuid", "PATCHED");
    CHECK(r.run() == FlashKitActions::Outcome::Failed);
    CHECK(r.ui.said("Restore Mode with your own stock LBOOT.EPB"));
    const vector<string> &log = r.flasher.log;
    CHECK(log.back() == "recovery mode off"); // the flag is cleared, the console is not sent into recovery
    for (const string &step : log) {
        CHECK(step != "reset user data");
        CHECK(step.find("restore saves") == string::npos);
    }
    CHECK_FALSE(r.flasher.rebooted);
    CHECK_FALSE(r.flasher.recoveryMode);
    CHECK(ableem::DirEntry::exists(r.fb.tmp.at("root/var/log/live.log"))); // no foreign path deleted
    CHECK(r.fb.tmp.readFile("usrdata/AppData/sony/pcsx/5/.pcsx/memcards/card1.mcd") == "CARD 5");
}

TEST_CASE("restoreFactory: a kernel write that fails, and a data partition that cannot be made") {
    {
        FactoryRun r;
        r.flasher.kernelWrite = Flasher::KernelWrite::NothingWritten;
        CHECK(r.run() == FlashKitActions::Outcome::Failed);
        CHECK_FALSE(r.flasher.recoveryMode);
        CHECK(r.ui.said("Nothing was changed"));
        for (const string &step : r.flasher.log)
            CHECK(step.find("restore rootfs") == string::npos);
    }
    {
        FactoryRun r;
        r.flasher.kernelWrite = Flasher::KernelWrite::Failed;
        CHECK(r.run() == FlashKitActions::Outcome::Failed);
        CHECK(r.flasher.recoveryMode); // the flag stays: Sony's recovery restores the backup
        CHECK(r.flasher.rebooted);
        for (const string &step : r.flasher.log)
            CHECK(step.find("restore rootfs") == string::npos);
    }
    {
        FactoryRun r;
        r.flasher.userDataReset = Flasher::UserDataReset::Untouched; // could not be unmounted
        CHECK(r.run() == FlashKitActions::Outcome::Failed);
        CHECK_FALSE(r.flasher.recoveryMode);
        CHECK_FALSE(r.flasher.rebooted);
        for (const string &step : r.flasher.log)
            CHECK(step.find("restore saves") == string::npos);
    }
    {
        FactoryRun r;
        r.flasher.userDataReset = Flasher::UserDataReset::Failed; // mkfs failed: the filesystem is gone
        CHECK(r.run() == FlashKitActions::Outcome::Failed);
        CHECK(r.flasher.recoveryMode);
        CHECK(r.flasher.rebooted);
        CHECK(r.ui.said("restore itself from LBOOT.EPB"));
    }
}

//*******************************
// the data partition in use, and dumpe2fs
//*******************************
namespace {
// the console's /proc/mounts under the AutoBleem 2020 init: the root is an overlay whose layers are on /data
const char *const OverlayMounts =
    "rootfs / overlay rw,relatime,lowerdir=newroot,upperdir=data/autobleem/rootfs,workdir=data/autobleem/workdir 0 0\n"
    "proc /proc proc rw,relatime 0 0\n"
    "/dev/mmcblk0p10 /data ext4 rw,noatime,nodiratime,commit=1,data=ordered 0 0\n"
    "rootfs /var/lib/bluetooth overlay rw,relatime,lowerdir=newroot,upperdir=data/autobleem/rootfs,"
    "workdir=data/autobleem/workdir 0 0\n";
// the new init: the upper layer in RAM, /data read-only and used by nothing
const char *const FreeMounts =
    "rootfs / overlay rw,relatime,lowerdir=newroot,upperdir=/ramdisk/upper,workdir=/ramdisk/work 0 0\n"
    "proc /proc proc rw,relatime 0 0\n"
    "/dev/mmcblk0p10 /data ext4 ro,noatime,nodiratime,data=ordered 0 0\n"
    "/dev/sda1 /media vfat rw,relatime 0 0\n";
} // namespace

TEST_CASE("mkfsCommand reads dumpe2fs -h too, with its version line first") {
    const string info =
        "dumpe2fs 1.43.4 (31-Jan-2017)\n"
        "Filesystem volume name:   userdata\n"
        "Last mounted on:          /data\n"
        "Filesystem features:      has_journal ext_attr resize_inode dir_index filetype needs_recovery extent "
        "flex_bg sparse_super large_file huge_file dir_nlink extra_isize\n"
        "Reserved block count:     50000\n"
        "Block count:              1000000\n"
        "Block size:               4096\n"
        "Inode size:\t          256\n"
        "Journal features:         journal_incompat_revoke\n";
    CHECK(FactoryRestore::mkfsCommand(info, "/dev/x") ==
          "mkfs.ext4 -F -q -b 4096 -I 256 -m 5.00 -O none,has_journal,ext_attr,resize_inode,dir_index,filetype,"
          "extent,flex_bg,sparse_super,large_file,huge_file,dir_nlink,extra_isize -L userdata /dev/x");
}

TEST_CASE("dataInUse: the overlay layers on /data, another mount of it, a bind of it, a mount below it") {
    CHECK(FactoryRestore::dataInUse(OverlayMounts, "/data"));
    CHECK_FALSE(FactoryRestore::dataInUse(FreeMounts, "/data"));
    CHECK_FALSE(FactoryRestore::dataInUse("", "/data"));
    // absolute layer paths, in any of the three options
    CHECK(FactoryRestore::dataInUse("/dev/p10 /data ext4 rw 0 0\nrootfs / overlay rw,upperdir=/data/x,workdir=/tmp/w 0 0\n",
                                    "/data"));
    CHECK(FactoryRestore::dataInUse("/dev/p10 /data ext4 rw 0 0\nrootfs / overlay rw,workdir=/data/w,upperdir=/tmp/u 0 0\n",
                                    "/data"));
    CHECK(FactoryRestore::dataInUse("/dev/p10 /data ext4 rw 0 0\nrootfs / overlay rw,lowerdir=/ro:/data/lower 0 0\n",
                                    "/data"));
    // a folder that only starts with the same letters is not /data
    CHECK_FALSE(FactoryRestore::dataInUse("/dev/p10 /data ext4 rw 0 0\nrootfs / overlay rw,upperdir=/database/u 0 0\n",
                                          "/data"));
    CHECK(FactoryRestore::dataInUse("/dev/p10 /data ext4 rw 0 0\n/dev/p10 /mnt/again ext4 rw 0 0\n", "/data"));
    CHECK(FactoryRestore::dataInUse("/dev/p10 /data ext4 rw 0 0\n/data/games /gaadata none rw,bind 0 0\n", "/data"));
    CHECK(FactoryRestore::dataInUse("/dev/p10 /data ext4 rw 0 0\n/dev/sdb1 /data/usb vfat rw 0 0\n", "/data"));
}

TEST_CASE("ConsoleFlasher::userDataErasable follows the mounts file") {
    TempDir tmp("mounts");
    ConsoleFlasher flasher;
    flasher.procMounts = tmp.at("mounts-busy");
    tmp.writeFile("mounts-busy", OverlayMounts);
    CHECK_FALSE(flasher.userDataErasable());
    flasher.procMounts = tmp.at("mounts-free");
    tmp.writeFile("mounts-free", FreeMounts);
    CHECK(flasher.userDataErasable());
    flasher.procMounts = tmp.at("no-such-file"); // cannot tell: not erased
    CHECK_FALSE(flasher.userDataErasable());
}

TEST_CASE("restoreFactory refuses while the data partition is in use: nothing written, no backup, no flag") {
    FactoryRun r;
    r.flasher.dataErasable = false; // the overlay of the console's /proc/mounts
    CHECK(r.run() == FlashKitActions::Outcome::Refused);
    CHECK(r.ui.said("The data partition is in use by the running system"));
    CHECK(r.ui.questions.empty());
    CHECK_FALSE(r.wrote());
    CHECK_FALSE(r.flasher.recoveryMode);
    CHECK_FALSE(r.flasher.rebooted);
    for (const string &step : r.flasher.log) {
        CHECK(step.find("backup") == string::npos);
        CHECK(step.find("flash") == string::npos);
        CHECK(step.find("recovery mode") == string::npos);
    }
}

TEST_CASE("restoreFactory with a free data partition (the new init) runs through") {
    FactoryRun r;
    r.flasher.dataErasable = true;
    CHECK(r.run() == FlashKitActions::Outcome::Rebooting);
    CHECK_FALSE(r.ui.said("in use by the running system"));
    CHECK(r.flasher.rebooted);
}

TEST_CASE("restoreFactory in the half-done state: Sony's kernel and stock files, only the data partition is erased") {
    FactoryRun r(false);
    r.flasher.findings.bootIsStock = true;
    CHECK(r.run() == FlashKitActions::Outcome::Rebooting);
    const vector<string> &log = r.flasher.log;
    REQUIRE(log.size() == 8);
    CHECK(log[0] == "validate stock in " + r.paths.stockDir);
    CHECK(log[1] == "inspect factory state");
    CHECK(log[2].rfind("backup saves to ", 0) == 0);
    CHECK(log[3] == "recovery mode on");
    CHECK(log[4] == "reset user data");
    CHECK(log[5].rfind("restore saves from ", 0) == 0);
    CHECK(log[6] == "recovery mode off");
    CHECK(log[7] == "reboot");
    for (const string &step : log)
        CHECK(step.find("flash ") == string::npos);
    // and while it is still in use, it is refused as well
    FactoryRun busy(false);
    busy.flasher.findings.bootIsStock = true;
    busy.flasher.dataErasable = false;
    CHECK(busy.run() == FlashKitActions::Outcome::Refused);
    CHECK_FALSE(busy.wrote());
}
