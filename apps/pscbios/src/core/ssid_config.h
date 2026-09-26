//
// SsidConfig: the WiFi credentials file the AutoBleem kernel's abnet reads - /etc/autobleem/ssid.cfg, two
// lines: the SSID, the password. With no file the SSID may still be found in /etc/wpa_supplicant.conf (what
// an earlier setup wrote there). A file from before X6 (2026-09-26) may carry a third line, the driver mode
// ("wext"/"nl80211") pscbios no longer sets or shows - load() reads and discards it; save() writes two lines.
//
#pragma once

#include <string>

//******************
// SsidConfig
//******************
class SsidConfig {
public:
    std::string ssid;
    std::string password;

    // reads `path`; false (and the defaults kept) when it is not there
    bool load(const std::string &path);
    // writes the two lines; refuses (false) an empty SSID or password, as the tool always has
    bool save(const std::string &path) const;

    // the ssid= of a wpa_supplicant.conf; "" when there is none, or when it is "1" (what an old setup
    // wrote when it had no ssid.cfg)
    static std::string ssidFromWpaSupplicant(const std::string &path);

    // where they are on the console: <kernel config dir>/ssid.cfg and /etc/wpa_supplicant.conf; on a
    // machine without the kernel config dir, next to the tool (so a dev host round-trips them)
    static std::string defaultPath();
    static std::string wpaSupplicantPath();
};
