//
// FlashKitActions: the four things ABFlashKit does, step by step - Flash Kernel (back up, validate, set the
// recovery flag, write the kernel and the payload, clear the flag, reboot), Full backup, Restore Mode (check
// the backup is ours and vanilla, set the recovery flag, reboot so that Sony's recovery restores from it),
// and Back up games (the console's built-in games copied to the stick). The screen is behind FlashUi
// (status lines, a progress bar, the confirmations, the pauses), the console behind Flasher and Led, so a
// test can run them all to the end.
//
#pragma once

#include "flasher.h"
#include "led.h"

#include <ableem/engine/byte_progress.h>

#include <functional>
#include <string>

//******************
// FlashUi
//******************
class FlashUi {
public:
    virtual ~FlashUi() = default;
    virtual void status(const std::string &text) = 0; // a line on the screen
    virtual bool confirm(const std::string &question) = 0;
    virtual void wait(int ms) = 0; // a pause the user is meant to read the status in
    // the bar under the status: done of total, kept across status lines; total 0 = no bar (a step whose
    // length nobody can know - an external command - shows the spinner alone)
    virtual void progress(int /*done*/, int /*total*/) {}
    // runs `job` - one blocking call with no progress of its own (image_verify_tool, the payload script,
    // sync) - while the screen keeps its spinner turning; returns when the job has. A screen runs it on a
    // thread; the default, the tests', runs it right here.
    virtual void runInBackground(const std::function<void()> &job) { job(); }
};

//******************
// FlashKitPaths
//******************
struct FlashKitPaths {
    std::string backup;           // <usb root>/LBOOT.EPB
    std::string kernelDir;        // <app dir>/kernel
    std::string scratchDir;       // where a backup is unpacked for inspection (/tmp/lbootzip on the console)
    std::string validMarker;      // <usb root>/validlboot - its presence skips that inspection
    std::string internalGamesDir; // the console's built-in games, /gaadata
    std::string internalDb;       // <usb root>/System/Databases/internal.db - their titles
    std::string gamesBackupDir;   // <usb root>/Games Backup
};

//******************
// FlashKitActions
//******************
class FlashKitActions {
public:
    enum class Outcome {
        Refused,   // the console is not in a state for it (another firmware, no backup, not our backup)
        Cancelled, // the user said no at a confirmation
        Failed,    // a step failed; nothing irreversible happened, unless the status said otherwise
        Done,      // finished, the tool goes on (a backup)
        Rebooting  // finished with the reboot issued (a flash, a restore)
    };

    FlashKitActions(Flasher &flasher, Led &led, FlashUi &ui, FlashKitPaths paths);

    Outcome flash();
    Outcome fullBackup();
    Outcome restore();
    Outcome backupGames();

    static const char *name(Outcome outcome);
    // how many steps the bar is drawn in: a byte count becomes a fraction of it
    static const int BarSteps = 1000;
    // "700 MB", "1.4 GB"
    static std::string sizeText(uint64_t bytes);

private:
    bool backupExists() const;
    void rebootNow();
    // a bar from 0, fed by byte counts (reported to the screen only when it moves a step); noBar() hides it
    ableem::ByteProgress bar();
    void noBar();

    Flasher &flasher_;
    Led &led_;
    FlashUi &ui_;
    FlashKitPaths paths_;
};
