//
// abflashkit_core: the backup's shape and checks, the fake flasher's files, the kernel write, the games
// backup, and the four sequences run to the end against the fake with a scripted screen - progress bars
// included.
//
#include "doctest/doctest.h"

#include "support/temp_dir.h"

#include "core/flash_actions.h"
#include "core/flasher.h"
#include "core/games_backup.h"
#include "core/lboot_backup.h"
#include "core/led.h"

#include <ableem/engine/md5.h>
#include <ableem/engine/zip_archive.h>

#include <string>
#include <vector>

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

// a tree with fake partitions, a kernel folder whose boot.md5 matches its boot.img, and the paths
struct Bench {
    Bench() : tmp("abflashkit") {
        tmp.writeFile("parts/boot", "BOOT IMAGE BYTES");
        tmp.writeFile("parts/root", "ROOTFS BYTES");
        tmp.writeFile("parts/user", "USERDATA BYTES");
        tmp.writeFile("parts/tee", "TEE BYTES");
        tmp.writeFile("kernel/boot.img", "the new kernel");
        tmp.writeFile("kernel/boot.md5", ableem::Md5::ofString("the new kernel") + "  boot.img\n");
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
