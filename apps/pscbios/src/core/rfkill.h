//
// RfkillUnblock: clears a soft rfkill block on Bluetooth before a backend's btUp() tries to power the
// adapter on. TOOLS-10 (BUG-10, a Pi 400): rfkill can soft-block hci0 (systemd restoring
// /var/lib/systemd/rfkill's saved state at boot is one way it happens) and BlueZ then reports the adapter's
// PowerState as "off-blocked" - Adapter1.SetProperty("Powered", true) fails every time, no matter how often
// it is asked, until the block is cleared. A HARD block (a physical switch, `hard` == 1 under sysfs, always
// read-only) is never touched here - only reported, through message().
//
// The sysfs root is a plain field, not a constructor argument that would ripple through both backends'
// constructors - a test (or a caller with an unusual mount) sets root directly, exactly like NativePaths'
// members. Its default is the real /sys/class/rfkill; a tree with no such directory at all (a console kernel
// whose Bluetooth dongle has no rfkill entry - the common case, not a fault) is a silent Result::NotNeeded,
// never an error.
//
#pragma once

#include <string>

//******************
// RfkillUnblock
//******************
class RfkillUnblock {
public:
    std::string root = "/sys/class/rfkill"; // replaced by a temp tree in the tests

    enum class Result {
        NotNeeded,   // no rfkill directory at all, or no "bluetooth"-type entry there, or one already
                     // unblocked - nothing to do, not an error
        Cleared,     // a soft-blocked bluetooth entry was found and its soft block cleared (wrote "0")
        HardBlocked, // a bluetooth entry is hard-blocked - left alone; message() explains
        ClearFailed, // a soft-blocked entry could not be cleared (no permission, read-only fs, ...);
                     // message() explains
    };

    // Looks at every entry under root whose "type" file reads "bluetooth". The first one that is
    // hard-blocked is reported and left alone (Result::HardBlocked) without looking at its soft block. The
    // first one that is soft-blocked (and not hard-blocked) has 0 written to its "soft" file.
    Result run();

    std::string message() const { return message_; } // set for HardBlocked/ClearFailed, "" otherwise

private:
    std::string message_;
};
