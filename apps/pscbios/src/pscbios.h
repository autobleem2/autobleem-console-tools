//
// PscBios: what PSC-Bios's screens share while it runs - the console backend they talk to. PSC-Bios is an
// extension of the launcher (Extensions/pscbios/, since 2026-09-24): the launcher's App is the AppBase every
// screen has, and this is the tool's own part, made by PscBiosExtension::run() for as long as its screens
// show.
//
#pragma once

#include "core/console_backend.h"

#include <memory>

//******************
// PscBios
//******************
class PscBios {
public:
    explicit PscBios(std::unique_ptr<ConsoleBackend> console);
    ~PscBios();
    PscBios(const PscBios &) = delete;
    PscBios &operator=(const PscBios &) = delete;
    // the one instance, for the screens - valid while PSC-Bios runs
    static PscBios &get();

    ConsoleBackend &console() { return *console_; }

private:
    static PscBios *instance;
    std::unique_ptr<ConsoleBackend> console_;
};
