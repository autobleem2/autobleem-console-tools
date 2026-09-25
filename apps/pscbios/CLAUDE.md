# PSC-Bios - developer context

The console's hardware configuration tool, **an extension of the launcher** since 2026-09-24 (the owner's
call: a plugin the launcher loads, `Extensions/pscbios/bin/psc/pscbios.so`, shipped with the console package;
it was an App - a program of its own in `Apps/pscbios/` - until then; ABFlashKit stays an App). The launcher's
*Hardware Information* item runs it (`SystemMenuAction::HardwareInfo` -> `app.extensions().run("pscbios")`),
and so does the Extensions list; where it cannot run - a Pi or a PC (it is built for the console only), a
console without it, one built against another `AB_SDK_ABI`, or disabled by the crash guard - the item shows
the built-in `GuiHardwareInfo` instead. One screen of facts - the time and timezone, the WiFi/ethernet/Bluetooth
dongles with their addresses, the pads and whether SDL has a mapping for each - and three things it can
do: **WiFi settings** (SSID typed or picked from a scan, password, driver mode, written for the kernel and
the network restarted; the timezone), the **gamepad mapping wizard** (a pad tested raw, every standard
input asked for in turn, the result written to the `gamecontrollerdb.txt` the launcher loads), a DualShock 3
USB-pairing page, and an interactive **Bluetooth pairing screen** (`GuiBtPairing`, 2026-09-22) - scan, pick,
pair/remove a DualShock 4 or other standard Bluetooth gamepad via a shipped `bt` bluetoothctl wrapper, so it
works on the original flashed kernel too (not just a freshly built one).
Ported on 2026-09-18 from the 2020 standalone tool (a fork of the
old AutoBleem GUI, kept in git history under `psctools/pscbios`) onto `lib_ableem` + `ab_core` +
`ab_classic`; the root CLAUDE.md covers those and the build.

## What the console provides

Everything console-specific goes through two scripts the **AutoBleem kernel** ships in `/bin` - they are
not in this repository:

- `/bin/abnet list_ifaces | wlan_on | is_up <iface> | show_ip <iface> | scan | configure "<ssid>" "<pw>"
  | driver_mode <wext|nl80211> | restart | bt_up | bt_name`
- `bluetoothctl` (and a running `bluetoothd`) for the Bluetooth pairing screen (`GuiBtPairing`). PSC-Bios
  ships its own `bt` wrapper (`resources/bt`, run from `Env::getAppDir()`) that drives bluetoothctl:
  `bt scan | paired | pair "<mac>" | remove "<mac>"` - `scan`/`paired` print `<mac> <name>` per line,
  `pair`/`remove` print `ok`. **This deliberately does NOT use a new abnet subcommand**, so pairing works on
  the ORIGINAL flashed kernel too (its overlay already carries bluez 5.50 + bluetoothd + the sixaxis
  plugin), not only on a freshly built one. Only `bt_up`/`bt_name` (the adapter facts) go through abnet, and
  those already exist. DualShock 3 is NOT here - it pairs over USB through the sixaxis BlueZ plugin.
- `/bin/settime tz` (the current zone) and `settime tzone "<zone>"`; the zone list is `timedatectl list-timezones`.

Plus two files: `/etc/autobleem/ssid.cfg` (three lines: SSID, password, driver mode - what `abnet
configure` is fed from) and `/etc/wpa_supplicant.conf` (read only, for the SSID an earlier setup left
there; its `"1"` means none). `/etc/autobleem` is `Env::getPathToKernelConfigDir()`, set by
`EnvironmentSetup::fromRoot()` on the console only; off the console `SsidConfig` keeps `ssid.cfg` next to
the tool. Without the kernel (`/bin/abnet` missing) the network rows and WiFi settings are off and the
pad wizard still works - the 2020 tool exited after "Custom Firmware Kernel Not Found".

## Layout

```
src/core/       pscbios_core (SDL-free, links ab_core; the tests link it)
  console_backend.*   ConsoleBackend interface; AbnetBackend (the popens above, through System::execUnixCommand[Lines])
                      and FakeBackend (a dev host: wlan0 on 192.168.1.23, three SSIDs, Europe/Warsaw, 16 zones)
  native_backend.*    NativeBackend (Unix only, not used yet - docs/native-backend-plan.md): interfaces from
                      /sys/class/net (wireless = a `wireless`/`phy80211` entry, any name), IPv4 by getifaddrs, WiFi
                      through WpaCtrlClient, or - with no wpa_supplicant running - wpa_supplicant.conf written and
                      `dhcpcd -n <iface>` (its 10-wpa_supplicant hook starts it); Bluetooth/timezone delegated to an
                      AbnetBackend until step 2. Every path in NativePaths, the programs through a CommandRunner
  wpa_ctrl_client.*   WpaCtrlClient: /var/run/wpa_supplicant/<iface> over third_party/wpa_ctrl (hostap 2.10's client,
                      BSD, README.autobleem.txt lists our two changes) - SCAN (waits for the event), SCAN_RESULTS,
                      STATUS, the one-network configure, TERMINATE; a timeout on every request
  ssid_config.*       SsidConfig - ssid.cfg load/save, the wpa_supplicant.conf fallback, the paths
  game_controller_db.* GameControllerDb - gamecontrollerdb.txt with one mapping replaced under "#AutoBleem"
  pad_mapping.*       PadMapping - the wizard's logic: the 25 standard elements, detectChange(), the stick-half
                      merge (finalElements), the mapping line; the 2020 right-stick bug is fixed here
  network_status.*    NetworkStatus - the main screen's facts, one refresh() per RefreshInterval
src/screens/    the screens, on ab_classic (GuiFactsPage, GuiStringMenu, GuiConfirm, GuiKeyboard, GuiTextPage, GuiAbout)
  gui_pscbios_main.*  the opening screen, a GuiFactsPage (sections: time, WiFi, ethernet, Bluetooth, the controllers
                      with their mapping and the mapping file); Select = WiFi (kernel only), Square = gamepads,
                      Triangle = About, Circle = quit
  gui_network_menu.*  the WiFi settings: seven option rows (label left, value right - a compact panel), the two
                      actions among them; the restart's two messages are the busy spinner over the panel.
                      gui_ssid_scan_menu.* holds both pickers (SSID scan, timezone)
  gui_gamepad_menu.*  the gamepad section, a compact three-row list; gui_pad_config.* the wizard: the facts, the
                      stage's message and (while mapping) the entries in two columns down the left of the panel,
                      the DualShock picture at the right, the front buttons as RESET/OPEN/POWER chips in the footer;
                      pscbios_pages.* the static texts (the About credits with GuiAbout::HeadingMark headings)
src/pscbios.*         PscBios - the running tool's shared part: the ConsoleBackend the screens reach as
                      PscBios::get().console(), valid while its screens show
src/pscbios_extension.cpp  PscBiosExtension (AB_EXTENSION): run() = the backend choice (FakeBackend under
                      AB_DEBUG_HOST), Env::setAppDir(the extension's folder) for its duration, the main screen
resources/            what ships in Extensions/pscbios/ next to bin/psc/pscbios.so: extension.ini (Network=none,
                      no Background), readme.txt, icon.png, DS3.png (the wizard's picture), bt (the bluetoothctl
                      wrapper), gamecontrollerdb.txt (the seed), lang/
```

It runs **inside the launcher**: the launcher's `App` is the `AppBase` every screen has (config, theme,
language, audio), its window and its pads; nothing of the SDK is in the plugin (`tools/check_extension.sh`
checks the exports - `ab_add_extension` builds it with the SDK's headers only, and pscbios_core's sources are
compiled into it rather than linked, since that library links ab_core). `Env::getAppDir()` - where `DS3.png`,
`bt` and, off the console, `ssid.cfg` are read - is the extension's folder while `run()` runs and is put back
after. Its log lines are the launcher's (`System/Logs/autobleem.log`, tagged `[pscbios]`).

**Its `AB_SDK_ABI` must be the launcher's**, or the launcher refuses to load it (and Hardware Information
falls back to the built-in screen): the CI's first step compares this repository's `autobleem-core` with the
launcher's (its develop; for a v* tag its master) and fails when they differ - bump the submodule with every
ABI bump in core.

## Theme and language

The tool draws with the launcher's theme and language - it is the launcher's process. The host loads the
extension's `lang/<Language>.txt` over the launcher's table when the extension is loaded: the shared classic
screens' strings (the keyboard's included) come from the launcher's file, the tool's own from its file (an
empty value there falls through to the launcher's).
`resources/lang/` is Key=Value; `English.txt` is generated from these sources:

```
python tools/lang_tools.py --src-dir apps/pscbios/src --lang-dir apps/pscbios/resources/lang extract
python tools/lang_tools.py --src-dir apps/pscbios/src --lang-dir apps/pscbios/resources/lang update
```
`make_win.sh` runs `validate` on it. Five translations came over from the 2020 tool (Italiano, Polski,
Portuguese_BR, Slovak, Spanish); about half their strings changed with the port and are empty.

## The wizard

`GuiPadConfig` opens one joystick **raw** (`ableem::Joystick`, after `Input::flushPads()` let go of the
pads; `probePads()` takes them back after) and shows its axes/buttons/hats as numbers plus the DS3 picture
lit by the mapped view. It is driven by the console's **front buttons** - they arrive as keyboard scancodes:
Power (`Key::Sleep`, Escape on a keyboard) cancels/exits, Reset (`Key::Reset`, or Start = Space with the
keyboard as pad) picks the next pad, Open (`Key::Open`, or Return) starts mapping / skips an input / saves.
`Input::setPowerKeyAsKey(true)` for the screen's duration is what turns the power button into a key. The
mapping: `initialState` is the pad at rest; every frame `PadMapping::detectChange()` names the raw input
that moved; the screen waits for it to be released (`anythingHeld`) and moves on. At the end
`finalElements()` merges the stick halves, the line is given to SDL (`Input::addMapping`) for a test, and
Open once more asks for a name and writes it with `GameControllerDb` to `Input::currentMappingPath()` -
the file `probePads()` loaded, i.e. the kernel's `/etc/autobleem/gamecontrollerdb.txt` when it exists,
else the main GUI's `gamecontrollerdb.txt` - which the launcher loads at its next start (`Env::padMappingFiles()`).

## Build, run, test

- **This repository** (Linux: the host build and the console): `ab_add_extension` without a HOST builds
  `<build>/extensions/pscbios/`; the host build also runs `tests/apps/test_pscbios_core.cpp` (and, on Linux,
  `test_pscbios_native.cpp`: a fake wpa_supplicant on a Unix datagram socket in a temp dir, a fake sysfs). The CI's psc
  job checks the plugin (`check_psc_binary.sh`, `check_extension.sh`) and packs the folder into
  `console-tools-psc-<v>.tar.gz` as `Extensions/pscbios/`, next to `Apps/abflashkit/`; autobleem-appliance
  extracts that tarball onto the stick. Not built for the Pi. Not UPX-packed (a shared library).
- **Windows** needs the launcher's import library, so there it is built **with the launcher**:
  `cmake ... -DAB_EXTENSION_DIRS=<this repo>/apps/pscbios` in the launcher's build, then its
  `tools/make_usb.py usb` puts `build_win/extensions/pscbios/` on the dev stick.

Visual test on Windows: the launcher's `tools/ab_drive.py start`, then the System menu's Hardware Information
(`down l2; press r2; up l2; wait_screen GuiSystemMenu`, four `press down`, `press x`) - the screen names are
`GuiPscBiosMain`, `GuiNetworkMenu`, `GuiSsidScanMenu`, `GuiTimezoneSelect`, `GuiGamepadMenu`,
`GuiPadConfig`, `GuiTextPage`, `GuiAbout`, `GuiKeyboard`. The FakeBackend answers everything; a
`configure`/`restart`/`setTimezone` is logged and reflected in what it then reports. The wizard needs a
real pad (the keyboard-as-pad is not a joystick) - it shows "NO GAME CONTROLLERS OPENED" without one.

## Behaviour changes vs the 2020 tool

- No exit without the kernel; the pads can still be tested and mapped.
- The network facts are refreshed every 2 s by the clock, not every 75 frames (8 popens a frame-count).
- Titles are decorated once (`-=...=-`, not twice); the main screen returns instead of recursing.
- The right-stick merge checked the left stick's values (`gui_padconfig.cpp:132-139`); fixed, tested.
- The mapping is saved to the file the launcher loads (`Input::currentMappingPath()`), never to the
  tool's own folder, and the launcher actually loads it now (it never called `loadMappings` before).
- Dead code gone: `bluetool`, the BT menu, `cfgprocessor`, `DebugTimer`, the theme/sony copies.
- The shared look (2026-09-21, the root CLAUDE.md's "UI styling standards"): the facts page, compact
  option lists with the values at the right edge, chips for the front buttons, Back everywhere leaving
  loses nothing; the gamepad menu's copy of the controller list is gone (the opening screen has it).
