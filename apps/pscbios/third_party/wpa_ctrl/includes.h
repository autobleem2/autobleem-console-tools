/*
 * AutoBleem shim for hostap's src/utils/includes.h: the system headers wpa_ctrl.c needs for the Unix-socket
 * client (CONFIG_CTRL_IFACE + CONFIG_CTRL_IFACE_UNIX) on Linux, and nothing else. See README.autobleem.txt.
 */
#ifndef AB_WPA_CTRL_INCLUDES_H
#define AB_WPA_CTRL_INCLUDES_H

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#endif /* AB_WPA_CTRL_INCLUDES_H */
