/* PC versions of the test "console library" in tests/sysdemo/p4tp_sys.h.
 * This is the shape every replaced system library takes: read the arguments,
 * do the same job with the PC's own means, return the result.
 * sys_extra is left out on purpose: the tests check the "missing" report. */
#include "recomp_system.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static FILE *files[64];

static void pc_sys_print(Ctx *c) {
    fputs((const char *)GUEST(ARG0), stdout);
    RETURN(0);
}

static void pc_sys_file_open(Ctx *c) {
    FILE *f = fopen((const char *)GUEST(ARG0), "rb");
    if (!f) RETURN((uint64_t)(int64_t)-1);
    for (int i = 3; i < 64; i++)
        if (!files[i]) { files[i] = f; RETURN(i); }
    fclose(f);
    RETURN((uint64_t)(int64_t)-1);
}

static void pc_sys_file_read(Ctx *c) {
    int h = (int)ARG0;
    if (h < 0 || h >= 64 || !files[h]) RETURN((uint64_t)(int64_t)-1);
    RETURN(fread(GUEST(ARG1), 1, ARG2, files[h]));
}

static void pc_sys_file_close(Ctx *c) {
    int h = (int)ARG0;
    if (h >= 0 && h < 64 && files[h]) { fclose(files[h]); files[h] = NULL; }
    RETURN(0);
}

static void pc_sys_alloc(Ctx *c) { RETURN(recomp_guest_alloc(ARG0)); }

/* The controller: same script file as the console version, so the tests can
 * compare them. A real port reads a PC game pad or the keyboard here. */
static unsigned pad[64];
static int pad_frames = -1, frame;

static void pc_sys_pad_read(Ctx *c) {
    if (pad_frames < 0) {
        pad_frames = 0;
        FILE *f = fopen("p4tp_pad.txt", "r");
        unsigned v;
        while (f && pad_frames < 64 && fscanf(f, "%u", &v) == 1) pad[pad_frames++] = v;
        if (f) fclose(f);
    }
    RETURN(ARG0 == 0 && frame < pad_frames ? pad[frame] : 0);
}

static void pc_sys_frame_end(Ctx *c) { fflush(stdout); frame++; RETURN(0); }

static void pc_sys_time_ms(Ctx *c) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    RETURN((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

/* Threads: the runtime runs each guest thread on a host thread. */
static void pc_sys_thread_create(Ctx *c) { RETURN(recomp_thread_spawn(ARG0, ARG1)); }
static void pc_sys_thread_join(Ctx *c) { RETURN((uint64_t)(int64_t)recomp_thread_join(ARG0)); }

RECOMP_SYSTEM_TABLE(recomp_system_p4tp,
    { "sys_thread_create", pc_sys_thread_create },
    { "sys_thread_join", pc_sys_thread_join },
    { "sys_print", pc_sys_print },
    { "sys_file_open", pc_sys_file_open },
    { "sys_file_read", pc_sys_file_read },
    { "sys_file_close", pc_sys_file_close },
    { "sys_alloc", pc_sys_alloc },
    { "sys_pad_read", pc_sys_pad_read },
    { "sys_frame_end", pc_sys_frame_end },
    { "sys_time_ms", pc_sys_time_ms },
);
