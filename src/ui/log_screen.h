/* Kick Vita — reads ux0:data/Kick Vita/log.txt back on the device.
 *
 * Most of this project's hard bugs were diagnosed from that file, and until
 * now getting at it meant pulling the memory card. Shows the tail of the
 * current run, with Square switching to log.prev.txt — the previous run's
 * log, which is the one that matters after a crash. */
#ifndef KICK_UI_LOG_SCREEN_H
#define KICK_UI_LOG_SCREEN_H

void log_screen_run(void);

#endif
