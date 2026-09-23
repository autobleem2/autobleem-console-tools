//
// FlashKitActions: the sequences.
//
#include "flash_actions.h"
#include "games_backup.h"
#include "core/main.h"
#include "core/services/system.h"

#include <ableem/engine/log.h>
#include <ableem/engine/zip_archive.h>

#include <cstdio>
#include <memory>
#include <utility>

using namespace std;

const int FlashKitActions::BarSteps; // C++14: the in-class value needs a definition once it is bound to a reference

//*******************************
// FlashKitActions::FlashKitActions
//*******************************
FlashKitActions::FlashKitActions(Flasher &flasher, Led &led, FlashUi &ui, FlashKitPaths paths)
    : flasher_(flasher), led_(led), ui_(ui), paths_(std::move(paths)) {}

const char *FlashKitActions::name(Outcome outcome) {
    switch (outcome) {
    case Outcome::Refused:
        return "refused";
    case Outcome::Cancelled:
        return "cancelled";
    case Outcome::Failed:
        return "failed";
    case Outcome::Done:
        return "done";
    case Outcome::Rebooting:
        return "rebooting";
    }
    return "?";
}

string FlashKitActions::sizeText(uint64_t bytes) {
    const double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
    char text[32];
    if (mb < 1024.0)
        snprintf(text, sizeof(text), "%.0f MB", mb);
    else
        snprintf(text, sizeof(text), "%.1f GB", mb / 1024.0);
    return text;
}

bool FlashKitActions::backupExists() const {
    return DirEntry::exists(paths_.backup);
}

// the LED off, a moment for sysfs to take it, then the reboot
void FlashKitActions::rebootNow() {
    led_.setMode(LedMode::Off);
    ui_.wait(100);
    flasher_.reboot();
}

//*******************************
// FlashKitActions::bar / noBar
//*******************************
ableem::ByteProgress FlashKitActions::bar() {
    ui_.progress(0, BarSteps);
    auto shown = make_shared<int>(0);
    return [this, shown](uint64_t done, uint64_t total) {
        if (total == 0)
            return;
        const int step = static_cast<int>((done < total ? done : total) * BarSteps / total);
        if (step != *shown) {
            *shown = step;
            ui_.progress(step, BarSteps);
        }
    };
}

void FlashKitActions::noBar() {
    ui_.progress(0, 0);
}

//*******************************
// FlashKitActions::flash
//*******************************
FlashKitActions::Outcome FlashKitActions::flash() {
    if (flasher_.hasForeignFirmware()) {
        ui_.status(_("This PSC has not been restored to original condition. Unable to flash."));
        ui_.wait(4000);
        return Outcome::Refused;
    }
    if (!ui_.confirm(_("Start flashing ?")))
        return Outcome::Cancelled;

    led_.setMode(LedMode::BlinkGreen);
    // an existing backup is kept: it is the one the recovery would restore, made before any flash
    if (!backupExists()) {
        ui_.status(_("Creating backup...."));
        if (!flasher_.createBackup(
                LbootBackup::partitionsForFlash(), paths_.backup, [&](const string &text) { ui_.status(text); },
                bar())) {
            noBar();
            ui_.status(_("Invalid backup or invalid kernel image"));
            ui_.wait(3000);
            led_.setMode(LedMode::Green);
            return Outcome::Failed;
        }
    }
    noBar();
    ui_.runInBackground([&] { flasher_.sync(); });

    ui_.status(_("Validating backup...   Please wait..."));
    bool ok = false;
    ui_.runInBackground(
        [&] { ok = flasher_.validateBackup(paths_.backup) && flasher_.validateKernel(paths_.kernelDir); });
    if (!ok) {
        ui_.status(_("Invalid backup or invalid kernel image"));
        // the 2020 tool rebooted either way; nothing has been written, the console comes back as it was
        ui_.wait(3000);
        rebootNow();
        return Outcome::Failed;
    }

    led_.setMode(LedMode::Red);
    ui_.wait(2000);
    ui_.status(_("Setting recovery mode: on"));
    flasher_.setRecoveryMode(true, paths_.kernelDir);
    ui_.status(_("Flashing KERNEL IMAGE"));
    Flasher::KernelWrite written = flasher_.flashKernel(paths_.kernelDir, bar());
    noBar();
    if (written == Flasher::KernelWrite::NothingWritten) {
        // the boot partition is untouched: the flag goes off again and the console restarts as it was
        flasher_.setRecoveryMode(false, paths_.kernelDir);
        led_.setMode(LedMode::Green);
        ui_.status(_("The kernel image could not be written. Nothing was changed."));
        ui_.wait(4000);
        rebootNow();
        return Outcome::Failed;
    }
    if (written == Flasher::KernelWrite::Failed) {
        // a boot partition half written: the flag stays on, so the restart goes into Sony's recovery, which
        // restores the partitions from the backup just made - the one way this console boots again
        ui_.status(_("Writing the kernel failed. The console will restore itself from LBOOT.EPB when it restarts."));
        ui_.wait(6000);
        rebootNow();
        return Outcome::Failed;
    }
    ui_.status(_("Updating payload"));
    ui_.runInBackground([&] { flasher_.installPayload(paths_.kernelDir); });
    led_.setMode(LedMode::Green);
    ui_.status(_("Setting recovery mode: off"));
    flasher_.setRecoveryMode(false, paths_.kernelDir);
    ui_.wait(2000);
    ui_.status(_("All done - when the screen goes black replace power cord"));
    ui_.wait(3000);
    rebootNow();
    return Outcome::Rebooting;
}

//*******************************
// FlashKitActions::fullBackup
//*******************************
FlashKitActions::Outcome FlashKitActions::fullBackup() {
    // new since the port: the previous backup is what a restore would use, so overwriting it is asked
    if (backupExists() && !ui_.confirm(_("Overwrite the existing backup?")))
        return Outcome::Cancelled;
    led_.setMode(LedMode::BlinkGreen);
    ui_.status(_("Creating backup...."));
    if (backupExists())
        DirEntry::removeFile(paths_.backup);
    bool ok = flasher_.createBackup(
        LbootBackup::partitionsForFullBackup(), paths_.backup, [&](const string &text) { ui_.status(text); }, bar());
    noBar();
    ui_.runInBackground([&] { flasher_.sync(); });
    led_.setMode(LedMode::Green);
    ui_.status(ok ? _("Backup complete") : _("Backup failed"));
    ui_.wait(2000);
    return ok ? Outcome::Done : Outcome::Failed;
}

//*******************************
// FlashKitActions::restore
//*******************************
FlashKitActions::Outcome FlashKitActions::restore() {
    if (!backupExists()) {
        ui_.status(_("No backup found - Recovery locked"));
        ui_.wait(3000);
        return Outcome::Refused;
    }
    if (!LbootBackup::isAutoBleemBackup(paths_.backup)) {
        ui_.status(_("Non AutoBleem Backup found. Please use valid AB backup"));
        ui_.wait(3000);
        return Outcome::Refused;
    }
    // the marker file skips the inspection (a backup already known to be good)
    if (!DirEntry::exists(paths_.validMarker)) {
        ui_.status(_("Checking LBOOT for vanilla kernel. Decompressing..."));
        // one bar over the unpacking and the md5 of the two images after it, both sized from the zip's
        // directory before anything is read
        uint64_t unpacked = 0, hashed = 0;
        vector<ableem::ZipEntry> entries;
        ableem::ZipArchive::listEntries(paths_.backup, entries);
        for (const ableem::ZipEntry &entry : entries) {
            unpacked += entry.size;
            if (entry.name == "boot.img" || entry.name == "rootfs.ext4")
                hashed += entry.size;
        }
        const uint64_t whole = unpacked + hashed;
        ableem::ByteProgress shown = bar();
        bool extracted = flasher_.extractBackup(paths_.backup, paths_.scratchDir,
                                                [&](uint64_t done, uint64_t) { shown(done, whole); });
        if (!extracted) {
            noBar();
            ui_.status(_("Recovery interrupted"));
            ui_.wait(3000);
            return Outcome::Failed;
        }
        LbootBackup::Contents contents =
            LbootBackup::inspect(paths_.scratchDir, [&](uint64_t done, uint64_t) { shown(unpacked + done, whole); });
        noBar();
        if (contents.hasBoot) {
            PLOG_INFO << "backup: boot " << (contents.bootIsVanilla ? "vanilla" : "modified") << ", rootfs "
                      << (contents.hasRootfs ? (contents.rootfsIsVanilla ? "vanilla" : "modified") : "absent");
            if (!contents.isVanilla() &&
                !ui_.confirm(_(
                    "The LBOOT does not contain CLEAN PSC firmware. You may SOFTBRICK your console. Are you sure?"))) {
                ui_.status(_("Recovery interrupted"));
                ui_.wait(3000);
                return Outcome::Cancelled;
            }
            if (!contents.hasRootfs &&
                !ui_.confirm(_("The LBOOT does not contain rootfs1. This may be ABFK1.0a backup. Are you sure?"))) {
                ui_.status(_("Recovery interrupted"));
                ui_.wait(3000);
                return Outcome::Cancelled;
            }
        }
    }
    // new since the port: the reboot into recovery is the point of no return, so it is asked
    if (!ui_.confirm(_("Set recovery mode and reboot now?"))) {
        ui_.status(_("Recovery interrupted"));
        ui_.wait(3000);
        return Outcome::Cancelled;
    }
    led_.setMode(LedMode::Red);
    flasher_.setRecoveryMode(true, paths_.kernelDir);
    ui_.status(_("Recovery Mode On"));
    ui_.wait(3000);
    rebootNow();
    return Outcome::Rebooting;
}

//*******************************
// FlashKitActions::backupGames
//*******************************
FlashKitActions::Outcome FlashKitActions::backupGames() {
    vector<GamesBackup::Game> games =
        GamesBackup::find(paths_.internalGamesDir, paths_.internalDb, paths_.gamesBackupDir);
    if (games.empty()) {
        ui_.status(_("No built-in games found on this console"));
        ui_.wait(3000);
        return Outcome::Refused;
    }
    const uint64_t needed = GamesBackup::bytesToCopy(games);
    if (needed == 0) {
        ui_.status(_("The games are already backed up"));
        ui_.wait(3000);
        return Outcome::Done;
    }
    // room for the copies and a little over; a stick whose free space cannot be read is tried anyway
    uint64_t freeBytes = 0, totalBytes = 0;
    const string stickRoot = DirEntry::getDirNameFromPath(paths_.gamesBackupDir);
    if (System::diskSpace(stickRoot, freeBytes, totalBytes) && freeBytes < needed + 16 * 1024 * 1024) {
        ui_.status(_("Not enough space on the stick") + " - " + sizeText(needed) + " " + _("needed") + ", " +
                   sizeText(freeBytes) + " " + _("free"));
        ui_.wait(4000);
        return Outcome::Refused;
    }

    PLOG_INFO << "Backing up " << games.size() << " games, " << needed << " bytes, to " << paths_.gamesBackupDir;
    led_.setMode(LedMode::BlinkGreen);
    ableem::ByteProgress shown = bar();
    bool ok = GamesBackup::copy(
        games,
        [&](const GamesBackup::Game &game, size_t index, size_t count) {
            ui_.status(_("Copying:") + " " + game.title + "   (" + to_string(index + 1) + "/" + to_string(count) + ")");
        },
        shown);
    noBar();
    ui_.runInBackground([&] { flasher_.sync(); });
    led_.setMode(LedMode::Green);
    ui_.status(ok ? _("Games backed up to the Games Backup folder") : _("Games backup failed"));
    ui_.wait(2500);
    return ok ? Outcome::Done : Outcome::Failed;
}
