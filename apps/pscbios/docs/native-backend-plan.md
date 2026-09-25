# PSC-Bios without shell scripts - the native backend (plan, 2026-09-25)

## Why

Today every console fact and action goes through a shell: `/bin/abnet` (the kernel's script) and PSC-Bios's own
`bt` wrapper around `bluetoothctl`, their text output parsed back. That cost a night on the console:

- `bt pair` wrote its commands into `bluetoothctl` all at once, so it quit before a pad answered; a later
  version skipped the whole pairing when its log could not be written (a read-only stick) - and the screen
  showed nothing either way, because a script can only say "ok" or nothing;
- `abnet` looks for `wlan0` only, and knows nothing about why a network did not come up;
- no live state: the pairing screen cannot show connected / dropped / battery.

## What replaces it

A `ConsoleBackend`, **`NativeBackend`** (`src/core/native_backend.*`), replacing `AbnetBackend`; `FakeBackend`
stays for the dev host. The screens do not
change until step 4.

### Bluetooth - BlueZ over D-Bus (`src/core/bluez_client.*`)

- `org.bluez.Adapter1`: `Powered`, `StartDiscovery`/`StopDiscovery` (with a class-of-device filter for
  gamepads), `RemoveDevice`; the objects under `/org/bluez/hci0` for the device list.
- `org.bluez.Device1`: `Pair` (asynchronous), `Trusted`, `Connect`, and the properties `Paired`,
  `Connected`, `Name`, `Modalias`, `Icon` - read live, so the screen can show connected/dropped.
- **An agent of PSC-Bios's own for the time it pairs**: registered (`NoInputNoOutput`) and made default
  before `Pair`, answering `RequestConfirmation`/`RequestAuthorization`/`AuthorizeService` with yes **only for
  the device being paired**, unregistered afterwards - abbtagent (the payload's cable-pairing agent) is BlueZ's
  default again. The D-Bus calls are pumped by PSC-Bios while it waits, with a timeout.
- The battery: `/sys/class/power_supply/sony_controller_battery_<mac>` (what hid-sony/hid-playstation report),
  or `org.bluez.Battery1` where BlueZ has one.
- A real error for every failure (the D-Bus error name and message), shown on the screen and logged.

### WiFi - wpa_supplicant's control socket (`src/core/wpa_ctrl_client.*`)

- hostap's `wpa_ctrl.c` vendored (BSD-3) - the documented client of `/var/run/wpa_supplicant/<iface>`.
- The interfaces from `/sys/class/net/*/wireless` (any name, not only `wlan0`), addresses with `getifaddrs`.
- `SCAN` + `SCAN_RESULTS` (SSIDs with signal strength), `STATUS` (state, SSID, IP), and the configuration:
  `ADD_NETWORK`/`SET_NETWORK ssid|psk`/`ENABLE_NETWORK`/`SAVE_CONFIG` - wpa_supplicant writes
  `/etc/wpa_supplicant.conf` itself.
- With no wpa_supplicant running yet (no configuration at all): PSC-Bios writes the minimal file
  (`ctrl_interface`, `update_config=1`) and has dhcpcd take the interface again, whose `10-wpa_supplicant` hook
  starts it (the payload since f4d1e5c; the 2020 overlay always had the hook).
- Driver mode (nl80211/wext): the dhcpcd.conf variant chosen as today, as a file copy.

### Unchanged

The timezone (`settime`), the pad mapping wizard, `ssid.cfg`.

## Decisions (the owner, 2026-09-25)

- **D-Bus through libdbus, headers in the build image**: `libdbus-1-dev` joins the console sysroot of
  autobleem-build (the psc stage); PSC-Bios links `libdbus-1.so.3`, which every console has (stock included).
- **Native only**: `AbnetBackend` and the `bt` wrapper are removed with step 3 - no shell fallback. A console
  whose system bus or BlueZ does not answer shows the network and Bluetooth rows as unavailable, with the reason.

## Steps (one commit each, tests with each, as the launcher's rule)

1. `WpaCtrlClient` + the WiFi half of `NativeBackend`; tests against a fake control socket in a temp dir.
2. `BluezClient` (the D-Bus route chosen above) + the Bluetooth half; the logic (device list, the pairing
   state machine, the agent's yes/no) behind an interface, tested with a fake.
3. `pscbios_extension.cpp` uses `NativeBackend` on the console; `AbnetBackend`, the `bt` wrapper and their
   tests are removed.
4. No more frozen screens: scans and pairing run asynchronously (D-Bus replies and wpa_supplicant events pumped
   in the screen's loop) under the standard busy spinner (`Gui::beginBusy`/`tickBusy`) - today the screen
   draws a static message and blocks in a shell script, so the spinner never turns (the owner, 2026-09-25).
   The pairing screen: live state (pairing... / paired / connected / dropped, battery), the error text; the
   WiFi screen: signal strength, the connection state, the reason a connection failed.
5. On the console over SSH: DS4, DS3 (cable, abbtagent), a WiFi join from nothing, a wrong password.

## Progress

- Step 1 done (2026-09-25): `WpaCtrlClient`, `NativeBackend`'s WiFi half (Bluetooth and the timezone delegated
  to an `AbnetBackend` for now), `tests/apps/test_pscbios_native.cpp`. Two choices of its own: a scan with no
  wpa_supplicant running starts it the same way (the minimal file when there is none, `dhcpcd -n <iface>`), so a
  console with nothing configured yet can pick its network from a list; `restartNetwork` is abnet's (TERMINATE
  instead of `killall`, then `systemctl restart dhclient`). To check on the console: `dhcpcd -n` against the
  running `dhcpcd -B` starting wpa_supplicant through the hook, and whether the console has a `netdev` group
  (`GROUP=` is written only when it does).
- Step 2 done (2026-09-25): `BluezClient` (`src/core/bluez_client.*`) over a `BluezBus` interface, `DbusBluezBus`
  (libdbus-1, `src/core/bluez_dbus_bus.*`) the console's transport, `NativeBackend`'s Bluetooth half through it;
  `tests/apps/test_pscbios_bluez.cpp` (every host, a scripted BlueZ in `tests/apps/fake_bluez_bus.h`) and the
  Bluetooth cases in `test_pscbios_native.cpp`. Choices of its own: the first adapter by path (not `hci0` by name);
  pairing is a state machine (`beginPair`/`pump`) for step 4's screens, `pair()` its blocking form for today's
  `btPair`; a Connect that fails after a successful pairing still counts as paired (the old script's rule - the
  pad connects itself on its PS button) with the reason in `lastError()`; the agent is registered for the whole
  pairing (discovery included) but says yes only to that device, and answers `RequestPinCode` with 0000 and
  `RequestPasskey` with 0; `btUp()` powers an adapter that is off on once per backend; discovery that someone else
  started (InProgress) is joined and left running. The libdbus layer is built only where the dev files are - the
  build image has them in both the psc sysroot and the native stage, so the CI builds it everywhere it builds
  PSC-Bios. To check on the console (step 5): `abbtagent` becomes the default agent again after our
  UnregisterAgent (BlueZ keeps the earlier defaults in a queue - to be seen on 5.50); a DS4 pairs with Trusted set first and no
  AuthorizeService reaching our agent; `SetDiscoveryFilter` accepted by the console's bluetoothd; the battery file
  names hid-sony uses on the 4.4 kernel; the system bus's policy lets root export an agent and call `org.bluez`.
- Step 3 done (2026-09-25): `PscBiosExtension` makes a `NativeBackend` wherever it is built (not Windows) and not
  `AB_DEBUG_HOST`; `AbnetBackend`, `resources/bt` and their tests are gone. What was still delegated went native:
  `kernelInstalled()` is `/bin/abnet` existing (the same check, the file only looked at); the timezone is
  `/etc/timezone` (all `settime tz` did was cat it) else `/etc/localtime`'s link, the list the zoneinfo tables
  (`zone1970.tab` + `zone.tab`, each zone only while its file is there, plus UTC - `timedatectl list-timezones`
  without timedatectl), and a change `settime tzone <zone>` through the `CommandRunner` (no `sh -c`), for a zone
  of that list only (settime puts it into a path unchecked). `ConsoleBackend` gained `wifiInterface()`,
  `ethernetInterface()` (the first ARPHRD_ETHER interface with a `device`, not wireless, not the kernel's `rndis*`
  gadget nor `bnep*`), `lastError()` and `btLastError()`, and lost `wlanOn()`; `NetworkStatus` and the WiFi screen
  ask for the interfaces by what they are. Minimal surfacing until step 4: Bluetooth "Unavailable" with the reason
  as the main screen's details row (no adapter stays "Not found"), the pairing screen's reason under its two
  info rows, and a 3 s message with the reason after a failed SSID scan, WiFi write, restart, timezone change,
  Bluetooth scan, pairing or removal. The fixed reasons at the top of those paths are translated; the D-Bus /
  wpa_supplicant text after them is shown as it is. To check on the console (step 5): a stick that had the
  old extension - the installer replaces `Extensions/pscbios/` whole, so no `bt` is left behind; `/bin/settime`
  runs through its `#!/bin/bash` when exec'd directly (it has to keep its x bit); `/etc/timezone` exists on the
  flashed overlay (`reference/abrootfs.manifest.txt` says so) and matches `/etc/localtime`; the zone list is as
  long as timedatectl's was; a console without a WiFi dongle, with a USB ethernet dongle (its name), and with
  bluetoothd stopped (`systemctl stop bluetooth`: the main screen says Unavailable and why, nothing hangs).
