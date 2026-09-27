# ABFlashKit - developer context

The console's kernel installer, an App in `Apps/abflashkit` the launcher starts from its Apps set. Four
actions in one menu: **Flash Kernel** (a recovery backup of the console's partitions to the USB
stick's `LBOOT.EPB` if there is none yet, the backup and the kernel image validated, the bootloader's
recovery flag set, `kernel/boot.img` written to the boot partition and read back, the AutoBleem rootfs
overlay unpacked onto the data partition by `kernel/install_payload.sh`, the flag cleared, a reboot),
**Full backup** (all four partitions, rootfs included, to `LBOOT.EPB`), **Restore Mode** (checks the
backup is ours and is the stock firmware, sets the recovery flag and reboots - Sony's own recovery then
restores the console from the file on the stick) and **Back up games** (2026-09-24: the built-in games in
`/gaadata/<id>/` copied to `<usb root>/Games Backup/<title>/`, see below). Ported on 2026-09-18 from the 2020 standalone tool (a fork of the old AutoBleem
GUI, in git history under `psctools/abflashkit`) onto `lib_ableem` + `ab_core` + `ab_classic`; the root
CLAUDE.md covers those and the build.

**This tool writes to the console's flash.** Everything that does is in `ConsoleFlasher`, compiled on every
host but constructed only when `AB_DEBUG_HOST` is not defined; a dev host gets `FakeFlasher`, which touches
no device. Keep it that way.

## What the console provides

- `/dev/disk/by-partlabel/{BOOTIMG1,ROOTFS1,USRDATA,TEE1}` - read as files into the backup (a block
  device's size comes from seeking to its end, `DirEntry::sizeBySeeking`). `MISC` is written with `dd`
  (the recovery flag from `kernel/recovery-{on,off}.img`, 16 bytes). `BOOTIMG1` is written in-process since
  2026-09-24 (`ConsoleFlasher::writeImage`, was `dd`): opened for update (never truncated or created),
  refused before the first byte when the image is missing or bigger than the partition, `fsync`ed, dropped
  from the page cache (`posix_fadvise`) and read back against the md5 of what was written.
- `/gaadata/<id>/` (`Env::getPathToInternalGamesDir()`) - the built-in games, read only; their titles
  come from the stick's `System/Databases/internal.db`, the launcher's copy of the console's database.
- `image_verify_tool` - the firmware's own checker for an `LBOOT.EPB`; "fail" in its output is a no.
- `kernel/install_payload.sh` (a console script in the payload, run with `bash`), `sync`, `killall -9
  autobleem-gui`, `systemctl stop weston`, `systemctl reboot`.
- `/sys/class/leds/{red,green}/brightness` - the power LED, the flasher's progress indicator (green idle,
  blinking green backing up, red while the flag is set and the kernel written, off just before the reboot).
- The backup lives at `<usb root>/LBOOT.EPB`; `<usb root>/validlboot`, when present, skips the restore's
  inspection of it. `/tmp/lbootzip` is where a backup is unpacked for that inspection.
- `/usr/bin/bleemsync_service` or `/etc/systemd/system/project_eris.service` present = another custom
  firmware: a flash is refused ("This PSC has not been restored to original condition").

## The backup format

`LbootBackup`: a plain zip (deflate, no zip64 - the recovery reads plain zip, which is why `ZipWriter`
gives miniz each entry's size up front) of the partitions as `boot.img`, `rootfs.ext4` (full backup
only), `userdata.ext4`, `tz.img`, followed by the 4096-byte trailer in `lboot_signature.h` - "RAWCAW", a
signature block Sony's recovery accepts (madmonkey's), padded with "autobleem" lines. **The trailer is byte
for byte the 2020 tool's; never regenerate it.** Its last nine bytes ("autobleem") are how
`isAutoBleemBackup()` tells our backup from someone else's. `inspect()` md5s an unpacked backup against the
stock firmware's `boot.img` (`28ce5f6d…`) and `rootfs.ext4` (`ca710a12…`): a modified one, or one
without a rootfs (an ABFK 1.0a backup), gets a warning question before a restore.

## Progress (2026-09-24)

Every step that can measure itself draws a bar under the spinner, in bytes: the backup (one bar over all
the partitions, not one per partition), the kernel write and its read-back (one bar, twice the image's
size), the restore's unpacking plus the md5 of `boot.img`/`rootfs.ext4` after it (one bar, both sized from
the zip's directory up front), and the games backup (one bar over every file still to copy). The bytes come
from autobleem-core's `ableem::ByteProgress` overloads (`ZipWriter::addFile`, `ZipArchive::extract`,
`Md5::ofFile`, `DirEntry::copy`); `FlashKitActions::bar()` turns them into `BarSteps` (1000) and reports a
step only when it moves, and `GuiFlashKitMain::progress` draws at most one every 40 ms - a frame waits for
the display, so drawing all 1000 made a 2 s check take 26 s. A step that cannot measure itself -
`image_verify_tool`, `install_payload.sh`, `sync` - goes through `FlashUi::runInBackground`: on a thread,
with the spinner turning and no bar (the 2020 tool froze the screen for all of them).

**A kernel write that fails** is now told apart (`Flasher::KernelWrite`): nothing written -> the flag is
cleared and the console restarts as it was; written part way or not reading back -> the flag **stays on**
and the payload is not installed, so the restart goes into Sony's recovery, which restores the partitions
from the backup just made. The 2020 tool ignored `dd`'s result and cleared the flag either way.

## Back up games (2026-09-24)

`GamesBackup` (`core/games_backup.*`): every numbered folder under the internal games dir is a game (empty
ones and `databases/` are not), copied whole, sub-folders included, to `Games Backup/<title>` - the title
FAT-safe (`folderName`: no `<>:"/\|?*`, a colon becomes " -", no trailing dots/spaces), `"<title> (<id>)"`
when two share one, `"Game <id>"` when internal.db does not know the id. A file is copied through
`<name>.part` and renamed once whole; a file already there at the source's size is skipped, so a second run
says "already backed up" and an interrupted one resumes. The free space is checked first (`System::
diskSpace`, 16 MB of slack). Nothing on the console is written. The copies sit outside `Games/` on purpose:
the launcher still shows the originals from `/gaadata`; moving a folder into `Games/` makes it a USB game.
On a dev host `main.cpp` points the internal games dir at `<app dir>/fake/gaadata` with three 16 MB games.

The vendored miniz needed one patch for this: its backward scan for the zip end record went wrong when
the record was within 4 KB of the file start (a small zip plus the trailer) - marked `AutoBleem:` in
`lib_ableem/third_party/miniz/miniz.c`, regression test in `tests/core/test_zip_writer.cpp`.

## Layout

```
src/core/       abflashkit_core (SDL-free, links ab_core; the tests link it)
  lboot_signature.h   the trailer bytes
  lboot_backup.*      LbootBackup - the partition lists, the trailer, isAutoBleemBackup, inspect
  flasher.*           Flasher interface; ConsoleFlasher (the commands above, through System::execUnixCommand,
                      the zip through ableem::ZipWriter, md5 through ableem::Md5, the kernel through
                      writeImage) and FakeFlasher (every step logged and delayed 1.5 s; a backup written for
                      real from 8 MB stand-in files under <app dir>/fake/partitions; validation always passes;
                      kernelWrite says how the flash went; the reboot only sets a flag)
  games_backup.*      GamesBackup - the built-in games found, named and copied to the stick
  led.*               Led interface; SysfsLed (a std::thread blinker, joined by the destructor - the 2020
                      tool forked one it never reaped) and NullLed (logs)
  flash_actions.*     FlashKitActions - flash()/fullBackup()/restore()/backupGames() step by step, through a
                      FlashUi (status line, progress bar, confirm, wait, runInBackground) so the tests run them
                      to the end against the fakes; FlashKitPaths is every path they use
src/screens/gui_flashkit_main.*  the one screen: a GuiActionMenu of the four actions and About (the shared GuiAbout with the tool's credits; name + a line of what it
                      does, the version at the header's right), and the FlashUi: each status line is the busy
                      spinner's message over the dimmed menu (Gui::beginBusy) with the bar under it, a question
                      a GuiConfirm, a pause or a background job keeps the spinner turning; the last status stays
                      1.5 s before the menu is back
src/abflashkit_app.*  AbFlashKit : AppBase - holds the Flasher and the Led, knows the paths
src/main.cpp          EnvironmentSetup::forTool, the flasher/led choice, logs
resources/            app.ini, run.sh, readme.txt, icon.png, lang/ (Polski from the 2020 tool)
```
The `kernel/` payload (`boot.img` 6.8 MB, `abrootfs.tgz` 22 MB, the recovery images, `install_payload.sh`,
the md5 files) is fetched from `autobleem2/psc-kernel-payload`'s releases and unpacked straight into the
staged package's `Apps/abflashkit/kernel/` at package time by `.github/workflows/build.yml`'s `kernel-payload`
job: a v* tag build takes the payload released under the same tag; any other build takes the payload's
rolling `nightly` pre-release. A missing release or asset fails the job — there is no fallback to the old
2020 payload files.
`.github/workflows/build.yml`'s Package step lays `resources/`, then the fresh binary, then the fetched
kernel/ into `stage/Apps/abflashkit/`, and refuses a package whose `kernel/` is incomplete or whose
`boot.img` does not match `boot.md5`, or whose `abrootfs.tgz` does not match `abrootfs.md5` (TOOLS-3,
2026-09-27 - checked from the start until then it was not). `boot.md5`/`abrootfs.md5` may each be a bare
hash or `md5sum`'s `<hash>  <name>`. `Flasher::validateKernel()` (the tool's own readiness check, run before
a flash) checks both pairs the same way, in-process (`fileMatchesMd5` in `flasher.cpp`) - `install_payload.sh`
untars `abrootfs.tgz` onto the data partition, so a corrupt download used to be unpacked with nothing to say
so. **There is no `payload/Apps/abflashkit/` any more** (TOOLS-3, 2026-09-27, in two steps): first the
checked-in `abflashkit` binary and `readme.txt` went (dead weight - `resources/` has its own `readme.txt`
and CI always lays a freshly built binary over whatever `payload/` had), then the rest of it - `app.ini`,
`icon.png`, `run.sh`, `lang/` (all duplicated by `resources/`) and the 2020 `kernel/` files (`boot.img`,
`boot.md5`, `abrootfs.tgz`, `abrootfs.md5`, `install_payload.sh`, `recovery-{on,off}.img`, replaced wholesale
by the fetched release tarball, never read by CI - its own comment on the `kernel-payload` job said so
already). Proven with a byte-for-byte diff of the Package step's staged tree built both ways (the old
`payload/Apps/abflashkit/` copied in first, and without it) against a real `nightly` kernel-payload tarball:
identical file list, identical md5 of every file. `payload/` itself is gone with it - `abflashkit` was the
only thing under it.

## Theme and language

Drawn with the main GUI's theme through `AppBase` (the 2020 tool had its own `background.jpg`, button PNGs
and `zrnic.ttf` - gone). The language comes from the main GUI's `config.ini`; the tool's `lang/` is loaded
over the main file (`Lang::loadMore`). Regenerate `English.txt` with
`python tools/lang_tools.py --src-dir apps/abflashkit/src --lang-dir apps/abflashkit/resources/lang extract`
(then `update`); `make_win.sh` validates it.

## Build, run, test

Part of the root build: the host build (`cmake --build`, `AB_TARGET=dev`, the default) builds `abflashkit.exe`
and runs `tests/apps/test_abflashkit_core.cpp` (the four sequences with a scripted `FlashUi` that also checks
each bar fills, the backup round trip, the kernel check and write, the games backup, the LED). The console
build is CI's `.github/workflows/build.yml` `psc` job (TOOLS-3, 2026-09-27 - `make_psc.sh` never existed in
this repository; that sentence was stale): it builds `abflashkit`, checks it against the console's
glibc/GLIBCXX (`tools/check_psc_binary.sh`), and its Package step stages `resources/`, then the fresh binary,
then the fetched `kernel/` payload into `Apps/abflashkit/` (see "Layout" above). Not built for the Pi.

Visual test on Windows: this used to be `python tools/make_usb.py usb` (`autobleem2/AutoBleem2`, the launcher
repository) staging `usb/Apps/abflashkit/`, but since ABFlashKit moved to this repository (2026-09-23) that
script's `TOOLS = ['abflashkit']` step looks for `apps/abflashkit/resources` inside *its own* tree, which no
longer exists there, and silently skips staging it - a plain `python tools/make_usb.py usb` no longer
populates `usb/Apps/abflashkit/` (TOOLS-3, 2026-09-27 - found stale, not yet fixed on that side). Until the
launcher's script is updated for the split, stage it by hand: copy this repository's
`apps/abflashkit/resources/` and a Windows-built `abflashkit.exe` into the launcher checkout's
`usb/Apps/abflashkit/` yourself, then `python tools/ab_drive.py start --tool abflashkit` then
`run "press down; press x; wait 8000; shot a.png"` does a full backup through the DebugDriver (the menu is
`GuiActionMenu` on the driver's screen stack, a question `GuiConfirm`; Up/Down pick the action, Cross runs it,
Circle quits) - logs in `usb/System/Logs/abflashkit.log`. There is no `kernel/` in the staged folder, so a
Flash on the dev host reports "Invalid backup or invalid kernel image" and "reboots"; drop a `kernel/boot.img`
+ `boot.md5` into `usb/Apps/abflashkit/` to see the whole flash sequence.

## Behaviour changes vs the 2020 tool

- A confirmation before a Full backup overwrites the existing `LBOOT.EPB` (it is what a restore uses),
  and before a Restore sets the recovery flag and reboots (the point of no return). Neither was asked.
- A backup that fails half way is deleted, so the next flash does not take it for a good one.
- The kernel md5 and the backup's contents are checked in-process (`ableem::Md5`), not through `md5sum`.
- The kernel is written in-process and read back, not with `dd`; a failed write keeps the recovery flag on
  (see "Progress"). Every long step shows a byte bar or a turning spinner instead of a frozen screen.
- Back up games is new.
- The blinker is a thread, not an orphaned fork; `systemctl reboot` and the rest are unchanged.
- The dev host runs against a fake and never runs `dd`, `systemctl` or `image_verify_tool`; the 2020 tool
  ran them all on x86 (a Cross would have rebooted the machine).
