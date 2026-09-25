/*
 * AutoBleem shim for hostap's src/utils/common.h + os.h: the os_* wrappers wpa_ctrl.c calls, as the plain
 * libc functions hostap's os_unix.c maps them to (os_strlcpy and the reltime helpers written out here, as in
 * os_unix.c/os.h). Linux only. See README.autobleem.txt.
 */
#ifndef AB_WPA_CTRL_COMMON_H
#define AB_WPA_CTRL_COMMON_H

#include "includes.h"

#define os_malloc(s) malloc((s))
#define os_free(p) free((p))
#define os_memcpy(d, s, n) memcpy((d), (s), (n))
#define os_memset(s, c, n) memset(s, c, n)
#define os_memcmp(s1, s2, n) memcmp((s1), (s2), (n))
#define os_strdup(s) strdup(s)
#define os_strlen(s) strlen(s)
#define os_strchr(s, c) strchr((s), (c))
#define os_strncmp(s1, s2, n) strncmp((s1), (s2), (n))
#define os_snprintf snprintf

static inline void *os_zalloc(size_t size) {
    return calloc(1, size);
}

static inline int os_snprintf_error(size_t size, int res) {
    return res < 0 || (unsigned int)res >= size;
}

/* copies at most siz - 1 characters and always terminates; returns strlen(src) (hostap's os_strlcpy) */
static inline size_t os_strlcpy(char *dest, const char *src, size_t siz) {
    const char *s = src;
    size_t left = siz;

    if (left) {
        while (--left != 0) {
            if ((*dest++ = *s++) == '\0')
                break;
        }
    }
    if (left == 0) {
        if (siz != 0)
            *dest = '\0';
        while (*s++)
            ; /* the rest of src, for the length */
    }
    return s - src - 1;
}

struct os_reltime {
    long sec;
    long usec;
};

static inline int os_get_reltime(struct os_reltime *t) {
    struct timespec ts;
    int res = clock_gettime(CLOCK_MONOTONIC, &ts);
    t->sec = ts.tv_sec;
    t->usec = ts.tv_nsec / 1000;
    return res;
}

static inline int os_reltime_expired(struct os_reltime *now, struct os_reltime *ts, long timeout_secs) {
    struct os_reltime age;
    age.sec = now->sec - ts->sec;
    age.usec = now->usec - ts->usec;
    if (age.usec < 0) {
        age.sec--;
        age.usec += 1000000;
    }
    return age.sec > timeout_secs || (age.sec == timeout_secs && age.usec > 0);
}

static inline void os_sleep(long sec, long usec) {
    if (sec)
        sleep(sec);
    if (usec)
        usleep(usec);
}

#endif /* AB_WPA_CTRL_COMMON_H */
