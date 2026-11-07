/*
obs-discord-golive
Copyright (C) 2026 obs-discord-golive contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

/*
 * Child process with a writable stdin and a readable stderr.
 *
 * libobs has os_process_pipe_*, but its stdin is a buffered FILE* with no
 * flush, so a small frame (CONFIG, STOP) can sit in the buffer forever, and
 * its destroy waits for the child with no timeout. This module writes
 * straight to the pipe, can close stdin on its own, waits with a timeout
 * and can kill the child.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct helper_process;

/* argv[0] must be an absolute path to the executable. argv ends with NULL. */
struct helper_process *helper_process_start(const char *const *argv);

/* Writes all of buf. Never raises SIGPIPE. False once the child is gone. */
bool helper_process_write(struct helper_process *hp, const void *buf, size_t len);

/* Reads from the child's stderr. Blocks. Returns 0 at EOF or on error. */
size_t helper_process_read_err(struct helper_process *hp, char *buf, size_t len);

/* Sends EOF on the child's stdin. */
void helper_process_close_stdin(struct helper_process *hp);

/* Waits up to timeout_ms for the child to exit. True if it has exited. */
bool helper_process_wait(struct helper_process *hp, uint32_t timeout_ms, int *exit_code);

/* Kills the child if it is still running and reaps it. Leaves stderr open,
 * so a thread blocked in helper_process_read_err returns 0 safely. */
void helper_process_kill(struct helper_process *hp);

/* Kills the child if it is still running, reaps it and frees everything.
 * No other thread may be using hp. */
void helper_process_destroy(struct helper_process *hp);

/*
 * Finds an executable by name in PATH and in the usual install locations
 * (OBS started from Finder or a desktop launcher has a minimal PATH).
 * Returns a bmalloc'd absolute path or NULL.
 */
char *helper_process_find_executable(const char *name);

#ifdef __cplusplus
}
#endif
