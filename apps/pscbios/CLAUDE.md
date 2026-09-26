# PSC-Bios - developer context

A hardware configuration extension (since 2026-09-24) and network/controller manager (2026-09-26): a plugin
the launcher loads, `Extensions/pscbios/bin/{key}/pscbios.so` (console, Pi 32/64-bit, PC stick; not Windows),
shipped with every package - it was an App, a program of its own in `Apps/pscbios/`, until 2026-09-24;
ABFlashKit stays an App. The launcher's System menu's *Network & Controllers* item opens it at the `network`
entry through `Extension::runEntry("network")` (SDK ABI 4, `Provides=network` in `extension.ini`); the
Extensions list runs the whole tool from `run()`, which shows `GuiPscBiosMain`, a `GuiFactsPage` of facts -
the time and timezone, the WiFi/ethernet/Bluetooth dongles with their addresses, the pads and whether SDL has
a mapping for each. Where PSC-Bios cannot run - Windows, a console without the kernel or custom libraries, one
built against another `AB_SDK_ABI`, or disabled by the crash guard - the *Network & Controllers* item does not
appear, and *Hardware Information* shows the built-in `GuiHardwareInfo` instead.

`runEntry("network")` opens the **Network & Controllers hub** directly: the same facts page with four
interactive items on its back side (Select/Square/L1/R1) - **Wi-Fi network** (SSID typed or scanned, password,
applied and restarted; the timezone - no driver-mode row, see X6 below), **Bluetooth controllers** (scan, pair,
remove a DualShock 4 or other standard pad through BlueZ over D-Bus - `GuiBtPairing`, 2026-09-22, live state and
battery at the right edge, re-read every 2 s), **DualShock 3 pairing** (USB via sixaxis), and the **controller
mapping wizard** (a pad tested raw, every standard input asked for in turn, the result written to the
`gamecontrollerdb.txt` the launcher loads). The network and Bluetooth items need the AutoBleem kernel on the
console (section 6.2) or system tools (NetworkManager + BlueZ) on a Pi or PC stick; the wizard works everywhere.

Ported on 2026-09-18 from the 2020 standalone tool (a fork of the
old AutoBleem GUI, kept in git history under `psctools/pscbios`) onto `lib_ableem` + `ab_core` +
`ab_classic`; the root CLAUDE.md covers those and the build.

## Platform-specific backends

The tool provides a **backend abstraction** (`ConsoleBackend` interface) for platform differences, chosen once
in `pscbios_extension.cpp`'s `makeBackend()`: **`NativeBackend` on the console** (psc), **`NmBackend` on
Raspberry Pi and the PC stick** (rpi/rpi64/pcusb), and **`FakeBackend` on a dev host** (`AB_DEBUG_HOST`);
Windows has no backend build and the network item is off. `AbnetBackend` (the AutoBleem kernel's `abnet`/
`bluetoothctl` scripts) is gone, superseded by `NativeBackend`.

**No shell script is run for the console or a Pi/PC stick** (since 2026-09-25, docs/native-backend-plan.md
step 3 - the owner's call: native only, no fallback - `resources/bt` and the old `bt` bluetoothctl wrapper are
deleted). `NativeBackend` asks the console itself:

- the network interfaces from `/sys/class/net` under any name (a wireless one has a `wireless`/`phy80211`
  entry; the wired one is ARPHRD_ETHER with a `device`, not `rndis*` - the AutoBleem kernel's USB gadget - nor
  `bnep*`), addresses by `getifaddrs`;
- WiFi through **wpa_supplicant's control socket** (`/var/run/wpa_supplicant/<iface>`); with none running,
  `/etc/wpa_supplicant.conf` written and `dhcpcd -n <iface>` (its `10-wpa_supplicant` hook starts it); a restart is
  TERMINATE + `systemctl restart dhclient`;
- Bluetooth through **BlueZ over D-Bus** (libdbus-1, the system bus) - adapter, discovery, pairing with an agent
  of its own, removal, battery. DualShock 3 is NOT here - it pairs over USB through the sixaxis BlueZ plugin;
- the timezone: `/etc/timezone` (what `settime tz` printed), else `/etc/localtime`'s link; the list from
  `/usr/share/zoneinfo/zone1970.tab` + `zone.tab` (what `timedatectl list-timezones` answered) plus UTC; a change
  is the kernel's `/bin/settime tzone <zone>`, run directly through `System::runAndWait` (no `sh -c`), for a
  zone from that list only.

**Raspberry Pi and PC stick** (`NmBackend`): `nmcli connection show`, `nmcli device wifi list`, `nmcli
device connect <ssid>`, plus `timedatectl list-timezones` and `timedatectl set-timezone`, and - like
`NativeBackend` - **BlueZ over D-Bus through the same `BluezClient`**, never `bluetoothctl` or the deleted
`bt` wrapper. The Wi-Fi screen shows the network it is on and writes no config file (NetworkManager
remembers the connection itself - `keepsWifiSettings()`); there is no driver-mode row here either (X6).
`nmcli device wifi connect` pauses for a second per character typed and is run through `restartNetwork()`
(blocking - nmcli's own call already waits out the attempt, so `beginWifiConnect`/`pumpWifiConnect()` just
report its outcome as an already-finished `WifiConnectWatch`, there being no wpa_supplicant event stream to
follow afterwards on a Pi/PC); the UI waits with the same spinner. Backend errors (no nmcli, the system bus
or bluetoothd down) are logged through `lastError()`/`btLastError()`; the item stays available because they
can be started.

The programs it starts (`dhcpcd`, `systemctl`, `settime`) go through `NativeBackend::CommandRunner`
(`System::runAndWait`, the tests record them). When something does not answer - no WiFi interface,
wpa_supplicant not starting, the system bus or bluetoothd down, no adapter - the call fails at once or after its
timeout and the reason is `lastError()` (WiFi, timezone) / `btLastError()` (Bluetooth), both on the
`ConsoleBackend` interface, always as **a reason for the user through `_()`, then the technical detail in
brackets** - "the controller refused the pairing (org.bluez.Error.AuthenticationFailed: Authentication
Failed)", "wpa_supplicant refused the network (SET_NETWORK psk: FAIL)". `BluezClient::reasonFor()` is what a
D-Bus error means (tested); a reason names no MAC or path - those are the bracket's.

**Nothing freezes a screen** (docs/native-backend-plan.md step 4, 2026-09-26 - the owner's report: the spinner
did not spin while scanning or pairing). Every slow call runs under the standard busy spinner through
`BusyWork` (`src/screens/pscbios_busy.*`: `Gui::beginBusy`, the screen as the dimmed backdrop, its footer
saying "|@O| Cancel"/"|@O| Stop" where the wait can be ended), and the backend calls the screen's **wait hook**
(`ConsoleBackend::setWaitHook`) every few tens of milliseconds while it waits - the D-Bus reply (DbusBluezBus
waits by reading the connection in 40 ms slices, on the main thread, never libdbus's blocking call), the scan's
event, wpa_supplicant's socket appearing, a program run (`System::runAndWait`'s `whileWaiting`). The hook turns
the spinner and reads the pad: Circle stops a Bluetooth scan (what was found is kept), a pairing, an SSID scan
and the following of a connection; a Quit ends any wait and is handed on to the screen's loop. The two long
ones are state machines pumped once a frame: the pairing (`btBeginPair`/`btPumpPair`, whose stage the spinner
says: "Looking for X...", "Pairing X...", "Connecting X...") and a WiFi connection after a write or a restart
(`beginWifiConnect`/`pumpWifiConnect` - a `WifiConnectWatch` fed wpa_supplicant's STATUS every 500 ms, its events
on an attached connection and the interface's address, to Connected or why not: a wrong password
(`reason=WRONG_KEY`), the network not found, an access point refusing, no address from DHCP, a timeout). The
status reads a screen repeats (the main screen every 2 s, the pairing and WiFi screens too) are short:
GetManagedObjects waits 1.5 s at most and a bluetoothd that did not answer is not asked again for 15 s (the
same reason shown meanwhile); STATUS waits 1 s.

Plus two files: `/etc/autobleem/ssid.cfg` (two lines: SSID, password - what the WiFi settings screen keeps;
the driver-mode third line is gone with X6) and `/etc/wpa_supplicant.conf` (read, for the SSID an earlier
setup left there; its `"1"` means none). `/etc/autobleem` is `Env::getPathToKernelConfigDir()`, set by
`EnvironmentSetup::fromRoot()` on the console only; off the console `SsidConfig` keeps `ssid.cfg` next to
the tool. The kernel's overlay brings `settime` and dhcpcd's hook; without it (`kernelInstalled()` false)
the network rows and WiFi settings are off and the pad wizard still works - the 2020 tool exited after
"Custom Firmware Kernel Not Found".

## Layout

```
src/core/       pscbios_core (SDL-free, links ab_core; the tests link it)
  console_backend.*   ConsoleBackend interface (wifiInterface()/ethernetInterface() by what they are, lastError() and
                      btLastError() - the reasons, the wait hook, the pumped pairing and WiFi connection, btBattery,
                      keepsWifiSettings()/currentSsid()) and FakeBackend (a dev host: wlan0 on 192.168.1.23, three
                      networks, Europe/Warsaw, 16 zones; `slow` - the extension's - takes the console's time for
                      every action through the hook: the pairing in three stages, the 8BitDo pad refused, "Neighbour
                      5G" a wrong password, the Xbox pad dropping a few refreshes in; the tests' is instant).
                      `AbnetBackend` is gone (superseded by NativeBackend)
  wifi_connect.*      WifiNetwork (a scan's network: signalText, secured), WpaStatus, wpaStateText (the connection row),
                      WifiConnectWatch - a connection followed to Connected or a WifiFailure, from STATUS, the events
                      and the address, the times the caller's (tested without waiting)
  bt_device_list.*    BtDeviceList - the pairing screen's rows: scan + paired merged, new / discovering... / pairing... /
                      connecting... / paired / connected / dropped (was connected, no longer) / failed, the battery,
                      the error
  native_backend.*    NativeBackend, **the console's** (psc; Unix only - docs/native-backend-plan.md): interfaces from
                      /sys/class/net (wireless = a `wireless`/`phy80211` entry, any name; ethernetInterface() the first
                      ARPHRD_ETHER one with a `device`, rndis*/bnep* left out), IPv4 by getifaddrs, WiFi through
                      WpaCtrlClient, or - with no wpa_supplicant running - wpa_supplicant.conf written and
                      `dhcpcd -n <iface>` (its 10-wpa_supplicant hook starts it); Bluetooth through BluezClient (the
                      bus connected on first use, again after a failure; btLastError() says why when there is none);
                      the timezone from /etc/timezone and the zoneinfo tables, set with `settime tzone`; the kernel
                      is /bin/abnet existing. Every path in NativePaths, the programs through a CommandRunner; the
                      short status timeouts and the BlueZ back-off; the connection watch re-attaches to a restarted
                      wpa_supplicant (its socket's inode)
  nm_backend.*        NmBackend, **Raspberry Pi and the PC stick's** (rpi/rpi64/pcusb): nmcli for the network,
                      timedatectl for the timezone, the same BluezClient NativeBackend uses for Bluetooth - no
                      shell script for pairing, ever. `keepsWifiSettings()` true (NetworkManager remembers the
                      connection, so `writeConfig()` skips `config.save()`); `restartNetwork()` is the blocking
                      nmcli call, `beginWifiConnect`/`pumpWifiConnect()` just report its already-finished outcome.
                      Same `Runner`/`LinesRunner` test-seam style as before the rewrite (`splitTerse`,
                      `parseDevices`, `firstAddress`, `shellQuoted`); takes a `BluezBusFactory` exactly like
                      NativeBackend for its tests
  wpa_ctrl_client.*   WpaCtrlClient: /var/run/wpa_supplicant/<iface> over third_party/wpa_ctrl (hostap 2.10's client,
                      BSD, README.autobleem.txt lists our two changes) - SCAN (waits for the event, 50 ms slices, the
                      hook asked), SCAN_RESULTS, STATUS, the one-network configure, TERMINATE; a timeout on every
                      request. WpaEventMonitor: an ATTACHed connection read without blocking
  bluez_client.*      BluezClient (every host, shared by NativeBackend and NmBackend): the first adapter from
                      GetManagedObjects and its devices (name, paired, trusted, connected, modalias, icon, class,
                      RSSI, Battery1), discovery (BR/EDR filter; someone
                      else's InProgress joined, not stopped), RemoveDevice/Disconnect, the battery (power_supply's
                      sony_controller_battery_<mac> / ps-controller-battery-<mac>, else Battery1), and the pairing as a
                      state machine - beginPair() then pump() (a screen's frame loop) or pair() (blocking): discover
                      until known, Trusted=true, Pair (AlreadyExists fine), wait for Paired, Connect (a failed Connect
                      still counts as paired, lastError() says why). Its own agent for the pairing's duration
                      (NoInputNoOutput, RequestDefaultAgent; yes only to the device being paired, PIN 0000/passkey 0),
                      unregistered after - abbtagent is the default again. Every failure: reasonFor(the D-Bus error),
                      else the step's reason, then the error, in lastError(); refresh(timeoutMs) for a status read;
                      scan(ms, keepGoing) ends early when told
  bluez_bus.h         BluezBus - the D-Bus operations it needs, values already unpacked (DbusValue); the seam the tests
                      script (tests/apps/fake_bluez_bus.h)
  bluez_dbus_bus.*    DbusBluezBus - that over libdbus-1: a private system-bus connection, no main loop, main thread only
                      (a call waits for its reply reading the connection in 40 ms slices, the hook asked between them,
                      its own deadline; pump() = read_write_dispatch for Pair/Connect and the agent's calls). Built
                      where libdbus-1's dev files are (PSCBIOS_HAVE_DBUS): required for the console, optional on a host
  ssid_config.*       SsidConfig - ssid.cfg load/save, the wpa_supplicant.conf fallback, the paths
  game_controller_db.* GameControllerDb - gamecontrollerdb.txt with one mapping replaced under "#AutoBleem"
  pad_mapping.*       PadMapping - the wizard's logic: the 25 standard elements, detectChange(), the stick-half
                      merge (finalElements), the mapping line; the 2020 right-stick bug is fixed here
  network_status.*    NetworkStatus - the facts screen's/hub's facts, one refresh() per RefreshInterval: the dongles by
                      the backend's wifiInterface()/ethernetInterface() (their names are an "Interface details" row),
                      Bluetooth's btStatusText()/btDetails() (Unavailable + the reason when BlueZ does not answer)
src/screens/    the screens, on ab_classic (GuiFactsPage, GuiActionMenu, GuiStringMenu, GuiConfirm, GuiKeyboard,
                 GuiTextPage, GuiAbout)
  gui_pscbios_main.*  the opening screen, a GuiFactsPage (sections: time, timezone, WiFi, ethernet, Bluetooth, the
                      controllers with their mapping and the mapping file). `run()` shows it as the facts screen
                      (Select = WiFi where the kernel/NetworkManager is there, Square = gamepads, Triangle = About,
                      Circle = quit); `runEntry("network")` shows it as the **Network & Controllers hub** instead,
                      with four interactive items in a `GuiActionMenu` on its back side (Select/Square/L1/R1 pick
                      Wi-Fi network, Bluetooth controllers, DualShock 3 pairing, the mapping wizard)
  gui_network_menu.*  the WiFi settings: option rows (label left, value right - a compact panel; no driver-mode row,
                      X6), the Connection row (wpa_state + address, re-read every 2 s), the last failure's row when
                      there is one, the two actions; the scan, the write, the restart, the connection that follows
                      and the timezone under the spinner. `writeConfig()` validates the SSID (+ password unless
                      `console.keepsWifiSettings()`) and skips `config.save()` for a backend that keeps its own
                      settings. gui_ssid_scan_menu.* holds both pickers (the scanned networks with their signal,
                      the timezones)
  gui_bt_pairing.*    the Bluetooth pairing screen: Scan, the devices with their live state and battery at the right
                      edge (re-read every 2 s), the last failure's row; scan and pairing under the spinner, Circle stops
  pscbios_busy.*      BusyWork - the spinner, the pad and the backend's wait hook for the length of one slow call
  gui_gamepad_menu.*  the gamepad section, a compact three-row list; gui_pad_config.* the wizard: the facts, the
                      stage's message and (while mapping) the entries in two columns down the left of the panel,
                      the DualShock picture at the right, the front buttons as RESET/OPEN/POWER chips in the footer
                      (hold Circle 2 s, or keyboard Esc/Backspace/Power, also exit the wizard);
                      pscbios_pages.* the static texts (the About credits with GuiAbout::HeadingMark headings)
src/pscbios.*         PscBios - the running tool's shared part: the ConsoleBackend the screens reach as
                      PscBios::get().console(), valid while its screens show
src/pscbios_extension.cpp  PscBiosExtension (AB_EXTENSION, ABI 4): run() shows the facts screen; runEntry("network")
                      opens the hub; `makeBackend()` picks NativeBackend under `PSCBIOS_NATIVE_BACKEND` on
                      `AB_PLATFORM_PSC`, NmBackend on rpi/rpi64/pcusb, FakeBackend under `AB_DEBUG_HOST`;
                      Env::setAppDir(the extension's folder) for its duration
resources/            what ships in Extensions/pscbios/ next to bin/{key}/pscbios.so: extension.ini (Provides=network,
                      Network=none, no Background), readme.txt, icon.png, DS3.png (the wizard's picture),
                      gamecontrollerdb.txt (the seed), lang/. `bt` (the bluetoothctl wrapper) is deleted - no
                      backend runs it any more
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
  `<build>/extensions/pscbios/`; the host build also runs `tests/apps/test_pscbios_core.cpp` (FakeBackend, NetworkStatus, WifiConnectWatch, BtDeviceList, ssid.cfg, the
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
`GuiPadConfig`, `GuiTextPage`, `GuiAbout`, `GuiKeyboard`, `GuiBtPairing`. The FakeBackend (slow, on a dev
host) answers everything in the time the console takes, so the spinner and every stage can be shot; a
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
