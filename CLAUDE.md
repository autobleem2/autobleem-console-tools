# AutoBleem console tools - developer context (short map)

`autobleem-console-tools` holds the console's own tools, built on the AutoBleem SDK from the `autobleem-core`
submodule (`ableem_engine`, `ableem`, `ab_core`, `ab_classic`):

- **PSC-Bios** (`apps/pscbios`): an *extension* - a plugin the launcher loads from `Extensions/pscbios/bin/<key>/pscbios.so`.
  Wi-Fi, time zone, Bluetooth, gamepad mapping. Console, Raspberry Pi (32/64-bit) and PC stick; not built on Windows here.
- **ABFlashKit** (`apps/abflashkit`): an *App* (`Apps/abflashkit`) - the console's kernel installer. It writes the
  console's flash. Built for `psc` and `dev` only.

Each app has its own `CLAUDE.md` (`apps/pscbios/CLAUDE.md`, `apps/abflashkit/CLAUDE.md`) - read the one you work in.
This file is the root map they refer to.

## Layout

- `CMakeLists.txt`: the one top level (see Build). `AB_BUILD_TESTS` of the core is forced OFF here.
- `apps/<app>/`: `src/` (`core/` = logic, `screens/`, the app/extension entry), `resources/` (`app.ini` or
  `extension.ini`, `icon.png`, `readme.txt`, `lang/`; PSC-Bios also `DS3.png`, `gamecontrollerdb.txt`), `CMakeLists.txt`.
  `apps/pscbios/third_party/wpa_ctrl/` is hostap's vendored control client; `apps/pscbios/docs/native-backend-plan.md`.
- `tests/`: `CMakeLists.txt` and `tests/apps/` (`test_pscbios_core`, `test_pscbios_bluez` + `fake_bluez_bus.h`,
  `test_pscbios_native`, `test_abflashkit_core`), on core's doctest harness.
- `toolchains/`: `rpi/` (`RPitoolchain.cmake`, `common.cmake`, `sdl2-devkit/` borrowed SDL headers), `rpi64/`, `pcusb/`.
  There is no `toolchains/psc/` here: the console toolchain comes from the build image (below).
- `tools/check_extension.sh`: checks a built plugin exports `ab_extension_abi` and `ab_extension_create` and defines no SDK symbols.
- `.github/workflows/build.yml`: CI. `autobleem-core/`: the submodule.

## Build and test

Each app splits into a `*_core` static library (no screens; what the tests link) and the program:
`pscbios_core` (links `ab_core`) with the plugin `pscbios` built by core's `ab_add_extension()`, which compiles
`pscbios_core`'s sources into the plugin and links no SDK library; `abflashkit_core` and the executable `abflashkit` (links `ab_classic`).

Host build and tests (what CI's `build` job runs, in `ghcr.io/autobleem2/autobleem-build`):

```
git submodule update --init
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure -j4
```

- Tests exist only for `AB_TARGET=dev` (`AB_TOOL_TESTS`, default ON there; OFF for every other target).
- `test_pscbios_native` is Unix only. Libdbus-1 dev files are optional on a host (no Bluetooth transport without them) and required for `psc`.
- Console package (CI `psc` job): configure with `-DCMAKE_TOOLCHAIN_FILE=/opt/ab/toolchains/psc/PSCtoolchainV8.cmake
  -DAB_PSC_TOOLCHAIN=/opt/psc`, `ninja`, then the image's `/opt/ab/tools/check_psc_binary.sh` (glibc 2.24, GLIBCXX 3.4.22, no
  RPATH) and `tools/check_extension.sh` on the plugin. The job packs `console-tools-psc-<version>.tar.gz`
  (`Extensions/pscbios/` + `Apps/abflashkit/`, with `kernel/` fetched from `autobleem2/psc-kernel-payload`'s release - `nightly`, or the tag's).
  The gcc-6 toolchain takes `-march=armv8-a -mfpu=neon-vfpv4`; the root CMakeLists sets the CPU flags before the submodule.
- Pi and PC stick (CI `linux` job): `-DCMAKE_TOOLCHAIN_FILE=toolchains/{rpi/RPitoolchain,rpi64/RPi64toolchain,pcusb/PcUsbToolchain}.cmake`,
  `ninja pscbios` only; output `console-tools-<target>-<version>.tar.gz`. The job checks the plugin's SDK stamp names the target.
- CI is gated by the repository variable `AB_CI_ENABLED`. Tags `v*` attach the tarballs to a release; `develop` pushes replace the
  `nightly` pre-release that autobleem-appliance assembles from; a manual run with `channel=preview` on a feature branch updates `preview`.
- Windows: PSC-Bios is built with the launcher (`-DAB_EXTENSION_DIRS=<this repo>/apps/pscbios` in the launcher's build); see `apps/pscbios/CLAUDE.md`.

## Platforms

`AB_TARGET` is `psc`, `rpi`, `pcusb` or `dev` (anything else is a configure error; empty means `psc` on ARM/aarch64, `dev` elsewhere).
It is passed to the submodule through the cache and defines `AB_PLATFORM_<NAME>`. `rpi64` is `rpi` built for aarch64 by
`RPi64toolchain.cmake` (core's `ab_extension_platform_key()` names the folder `bin/rpi64/`). ABFlashKit is added only for `psc` and `dev`.
Backends of PSC-Bios: `NativeBackend` on the console, `NmBackend` (NetworkManager + BlueZ) on Pi and PC stick, `FakeBackend` on a dev host.
ABFlashKit's `ConsoleFlasher` writes flash only when `AB_DEBUG_HOST` is not defined; a dev host gets `FakeFlasher`.

## The autobleem-core submodule and the SDK ABI

`.gitmodules`: `autobleem-core` -> `https://github.com/autobleem2/autobleem-core.git`; a clone needs `--recurse-submodules`
or `git submodule update --init`. PSC-Bios is loaded by the launcher only when its `AB_SDK_ABI`
(`autobleem-core/src/code/gui/extension.h`) equals the launcher's. CI's `sdk-abi` job compares this repo's
submodule with the one the launcher pins (its `develop`; for a `v*` tag the launcher's same tag if it exists; for a feature branch the
launcher's branch of that name) and fails on a mismatch; the `psc` and `linux` jobs wait for it. So bump this submodule with
every launcher bump:

```
git -C autobleem-core fetch origin
git -C autobleem-core checkout <the sha the launcher's autobleem-core points to>
git add autobleem-core
git commit -m "Bump autobleem-core to <short sha> (AB_SDK_ABI <n>)"
```

Rebuild and run the tests after a bump. The ABI guard (`test_sdk_layout`) lives in core, not here.

## Strings and translations

Every on-screen string is `_("English text")`. Each app has `resources/lang/<Language>.txt` (17 files: English + 16 languages,
same names in both apps), `English text=Translated text` lines under a `#` header. The host loads the tool's `lang/`
over the launcher's table; strings of the shared classic screens come from the launcher's files, an empty value falls through to the launcher's.
A new or changed string goes into every language file of that app in the same commit. `tools/lang_tools.py` (`extract`, `update`,
`validate`) is the launcher's, not in this repo; the commands are in the app docs ("Theme and language").

## Where the rest lives

- **Core** (`autobleem-core/CLAUDE.md`, `autobleem-core/docs/developer-guide.md`): lib_ableem, ab_core, ab_classic, ab_gui,
  "UI styling standards", the SDK ABI rules (`extension.h`), `ab_add_extension` (`cmake/ab_extension.cmake`).
- **Launcher** (`autobleem2/autobleem`): its `CLAUDE.md` and docs - `docs/extensions-plan.md` (how a plugin loads),
  `make_win.sh`, `tools/lang_tools.py`, `tools/make_usb.py`, `tools/ab_drive.py` (the Windows visual tests the app docs describe), the
  Raspberry Pi port notes the toolchain files cite, and the pin of `autobleem-core`.
- The build image (`autobleem2/autobleem-build`): `/opt/ab/toolchains/psc`, `check_psc_binary.sh`, the CI routing workflow.
