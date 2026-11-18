/* A small made-up "console system library" for the tests. A real console
 * game calls the console's own libraries for text, files, memory, the
 * controller and the clock; the recompiled program cannot use them on a PC,
 * so runtime/system/ gives it PC versions of the same functions. */
#ifndef P4TP_SYS_H
#define P4TP_SYS_H

void sys_print(const char *text);
int sys_file_open(const char *path);                        /* -1 if missing */
long sys_file_read(int handle, void *buf, unsigned long n);
void sys_file_close(int handle);
void *sys_alloc(unsigned long n);                           /* zeroed memory */
unsigned sys_pad_read(int port);                            /* buttons this frame */
void sys_frame_end(void);                                   /* next frame */
unsigned long sys_time_ms(void);                            /* a clock that never goes back */
int sys_extra(int x);                                       /* has no PC version on purpose */

#define PAD_UP 1u
#define PAD_DOWN 2u
#define PAD_LEFT 4u
#define PAD_RIGHT 8u
#define PAD_A 16u

#endif
