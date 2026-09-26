wpa_ctrl - wpa_supplicant's control interface client, vendored for PSC-Bios
===========================================================================

From hostap (https://w1.fi/hostap.git) at tag hostap_2_10, fetched 2026-09-25:

  wpa_ctrl.c   src/common/wpa_ctrl.c
  wpa_ctrl.h   src/common/wpa_ctrl.h
  COPYING      COPYING
  README       README   (the licence: BSD, advertisement clause removed)

Built by apps/pscbios/CMakeLists.txt on Linux only, with CONFIG_CTRL_IFACE and CONFIG_CTRL_IFACE_UNIX defined -
the Unix datagram socket client of /var/run/wpa_supplicant/<iface>. The UDP, named-pipe and Android branches are
left in the file as they are and never compiled. Client sockets are bound in /tmp (hostap's default
CONFIG_CTRL_IFACE_CLIENT_DIR), as wpa_cli does.

Local changes (each marked "AutoBleem" in the source):

  - includes.h and common.h are ours: shims standing in for hostap's src/utils/includes.h, common.h and os.h,
    giving the os_* wrappers the file calls as the libc functions hostap's os_unix.c maps them to. Nothing
    else of hostap's utils is needed.
  - wpa_ctrl_set_timeout(ctrl, ms) (wpa_ctrl.c, declared in wpa_ctrl.h): the reply timeout of
    wpa_ctrl_request() per connection, instead of the fixed 10 s (0 keeps the 10 s). A new field,
    timeout_ms, in struct wpa_ctrl's Unix part.

Otherwise the files are upstream's, byte for byte. Formatting is upstream's (.clang-format here disables it).
