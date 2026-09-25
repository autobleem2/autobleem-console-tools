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
pair/remove a DualShock 4 or other standard Bluetooth gamepad through BlueZ over D-Bus, so it works on the
original flashed kernel too (its overlay has bluez 5.50 + bluetoothd), not only a freshly built one.
Ported on 2026-09-18 from the 2020 standalone tool (a fork of the
old AutoBleem GUI, kept in git history under `psctools/pscbios`) onto `lib_ableem` + `ab_core` +
`ab_classic`; the root CLAUDE.md covers those and the build.

## What the console provides

**No shell script is run** (since 2026-09-25, docs/native-backend-plan.md step 3 - the owner's call: native
only, no fallback). `NativeBackend` asks the console itself:

- the network interfaces from `/sys/class/net` under any name (a wireless one has a `wireless`/`phy80211`
  entry; the wired one is ARPHRD_ETHER with a `device`, not `rndis*` - the AutoBleem kernel's USB gadget - nor
  `bnep*`), addresses by `getifaddrs`;
- WiFi through **wpa_supplicant's control socket** (`/var/run/wpa_supplicant/<iface>`); with none running,
  `/etc/wpa_supplicant.conf` written and `dhcpcd -n <iface>` (its `10-wpa_supplicant` hook starts it); the
  driver mode is `/etc/autobleem/dhcpcd.conf.<wext|nl80211>` copied over `/etc/dhcpcd.conf`; a restart is
  TERMINATE + `systemctl restart dhclient`;
- Bluetooth through **BlueZ over D-Bus** (libdbus-1, the system bus) - adapter, discovery, pairing with an agent
  of its own, removal, battery. DualShock 3 is NOT here - it pairs over USB through the sixaxis BlueZ plugin;
- the timezone: `/etc/timezone` (what `settime tz` printed), else `/etc/localtime`'s link; the list from
  `/usr/share/zoneinfo/zone1970.tab` + `zone.tab` (what `timedatectl list-timezones` answered) plus UTC; a change
  is the kernel's `/bin/settime tzone <zone>`, run directly through `System::runAndWait` (no `sh -c`), for a
  zone from that list only.

The programs it starts (`dhcpcd`, `systemctl`, `settime`) go through `NativeBackend::CommandRunner`
(`System::runAndWait`, the tests record them). When something does not answer - no WiFi interface,
wpa_supplicant not starting, the system bus or bluetoothd down, no adapter - the call fails at once or after its
timeout and the reason is `lastError()` (WiFi, timezone) / `btLastError()` (Bluetooth), both on the
`ConsoleBackend` interface: the main screen shows Bluetooth as "Unavailable" with the reason as its details
row, the SSID scan, the WiFi write/restart, the timezone and the pairing screen show the reason under their
message (`drawText`, 3 s - step 4 makes that live). The fixed reasons NativeBackend and the two clients produce
at the top of those paths go through `_()`; the D-Bus/wpa_supplicant error text after them is shown as it is.

Plus two files: `/etc/autobleem/ssid.cfg` (three lines: SSID, password, driver mode - what the WiFi settings
screen keeps) and `/etc/wpa_supplicant.conf` (read, for the SSID an earlier setup left there; its `"1"` means
none). `/etc/autobleem` is `Env::getPathToKernelConfigDir()`, set by `EnvironmentSetup::fromRoot()` on the
console only; off the console `SsidConfig` keeps `ssid.cfg` next to the tool. Without the AutoBleem kernel
(`kernelInstalled()`: `/bin/abnet` exists - only looked at, as the 2020 tool did; the kernel's overlay brings
settime, dhcpcd's hook and the driver variants too) the network rows and WiFi settings are off and the pad
wizard still works - the 2020 tool exited after "Custom Firmware Kernel Not Found".

## Layout

```
src/core/       pscbios_core (SDL-free, links ab_core; the tests link it)
  console_backend.*   ConsoleBackend interface (wifiInterface()/ethernetInterface() by what they are, lastError() and
                      btLastError() - the reasons) and FakeBackend (a dev host: wlan0 on 192.168.1.23, three SSIDs,
                      Europe/Warsaw, 16 zones)
  native_backend.*    NativeBackend, the console's (Unix only - docs/native-backend-plan.md): interfaces from
                      /sys/class/net (wireless = a `wireless`/`phy80211` entry, any name; ethernetInterface() the first
                      ARPHRD_ETHER one with a `device`, rndis*/bnep* left out), IPv4 by getifaddrs, WiFi through
                      WpaCtrlClient, or - with no wpa_supplicant running - wpa_supplicant.conf written and
                      `dhcpcd -n <iface>` (its 10-wpa_supplicant hook starts it); Bluetooth through BluezClient (the
                      bus connected on first use, again after a failure; btLastError() says why when there is none);
                      the timezone from /etc/timezone and the zoneinfo tables, set with `settime tzone`; the kernel
                      is /bin/abnet existing. Every path in NativePaths, the programs through a CommandRunner
  wpa_ctrl_client.*   WpaCtrlClient: /var/run/wpa_supplicant/<iface> over third_party/wpa_ctrl (hostap 2.10's client,
                      BSD, README.autobleem.txt lists our two changes) - SCAN (waits for the event), SCAN_RESULTS,
                      STATUS, the one-network configure, TERMINATE; a timeout on every request
  bluez_client.*      BluezClient (every host): the first adapter from GetManagedObjects and its devices (name, paired,
                      trusted, connected, modalias, icon, class, RSSI, Battery1), discovery (BR/EDR filter; someone
                      else's InProgress joined, not stopped), RemoveDevice/Disconnect, the battery (power_supply's
                      sony_controller_battery_<mac> / ps-controller-battery-<mac>, else Battery1), and the pairing as a
                      state machine - beginPair() then pump() (a screen's frame loop) or pair() (blocking): discover
                      until known, Trusted=true, Pair (AlreadyExists fine), wait for Paired, Connect (a failed Connect
                      still counts as paired, lastError() says why). Its own agent for the pairing's duration
                      (NoInputNoOutput, RequestDefaultAgent; yes only to the device being paired, PIN 0000/passkey 0),
                      unregistered after - abbtagent is the default again. Every failure: the D-Bus error in lastError()
  bluez_bus.h         BluezBus - the D-Bus operations it needs, values already unpacked (DbusValue); the seam the tests
                      script (tests/apps/fake_bluez_bus.h)
  bluez_dbus_bus.*    DbusBluezBus - that over libdbus-1: a private system-bus connection, no main loop (blocking calls
                      with a timeout, pump() = read_write_dispatch for Pair/Connect and the agent's calls). Built where
                      libdbus-1's dev files are (PSCBIOS_HAVE_DBUS): required for the console, optional on a host
  ssid_config.*       SsidConfig - ssid.cfg load/save, the wpa_supplicant.conf fallback, the paths
  game_controller_db.* GameControllerDb - gamecontrollerdb.txt with one mapping replaced under "#AutoBleem"
  pad_mapping.*       PadMapping - the wizard's logic: the 25 standard elements, detectChange(), the stick-half
                      merge (finalElements), the mapping line; the 2020 right-stick bug is fixed here
  network_status.*    NetworkStatus - the main screen's facts, one refresh() per RefreshInterval: the dongles by
                      the backend's wifiInterface()/ethernetInterface() (their names are an "Interface details" row),
                      Bluetooth's btStatusText()/btDetails() (Unavailable + the reason when BlueZ does not answer)
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
src/pscbios_extension.cpp  PscBiosExtension (AB_EXTENSION): run() = the backend choice (NativeBackend where it is
                      built - PSCBIOS_NATIVE_BACKEND, not Windows - FakeBackend under AB_DEBUG_HOST),
                      Env::setAppDir(the extension's folder) for its duration, the main screen
resources/            what ships in Extensions/pscbios/ next to bin/psc/pscbios.so: extension.ini (Network=none,
                      no Background), readme.txt, icon.png, DS3.png (the wizard's picture), gamecontrollerdb.txt
                      (the seed), lang/
```

It runs **inside the launcher**: the launcher's `App` is the `AppBase` every screen has (config, theme,
language, audio), its window and its pads; nothing of the SDK is in the plugin (`tools/check_extension.sh`
checks the exports - `ab_add_extension` builds it with the SDK's headers only, and pscbios_core's sources are
compiled into it rather than linked, since that library links ab_core). `Env::getAppDir()` - where `DS3.png`
and, off the console, `ssid.cfg` are read - is the extension's folder while `run()` runs and is put back
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
Portuguese_BR, Slovak, Spanish); about half their strings changed with the port and are empty. Every language
file has every key since 2026-09-25 (the pairing screen's strings were added then); `AutoBleem`/`Version` are
empty on purpose - the launcher's file has them.

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
  `<build>/extensions/pscbios/`; the host build also runs `tests/apps/test_pscbios_core.cpp` (FakeBackend, NetworkStatus, ssid.cfg, the
  mapping), `test_pscbios_bluez.cpp` (BluezClient over a scripted BlueZ, every host) and, on Linux,
  `test_pscbios_native.cpp` (a fake wpa_supplicant on a Unix datagram socket in a temp dir, a fake sysfs, the
  Bluetooth half over the scripted BlueZ, the timezone over a fake zoneinfo tree). The plugin links
  `libdbus-1.so.3` (the console's own, stock included; the build image's psc sysroot has the dev files). The CI's psc
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
