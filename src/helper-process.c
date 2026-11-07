/*
obs-discord-golive
Copyright (C) 2026 obs-discord-golive contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "helper-process.h"

#include <util/bmem.h>
#include <util/dstr.h>
#include <util/platform.h>

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

struct helper_process {
	HANDLE process;
	HANDLE in_w;
	HANDLE err_r;
};

/* Quote one argument the way CommandLineToArgvW / the MSVC CRT parse it. */
static void append_quoted(struct dstr *cmd, const char *arg)
{
	if (*arg && !strpbrk(arg, " \t\n\v\"")) {
		dstr_cat(cmd, arg);
		return;
	}

	dstr_cat_ch(cmd, '"');
	for (const char *p = arg;; p++) {
		size_t backslashes = 0;
		while (*p == '\\') {
			backslashes++;
			p++;
		}

		if (!*p) {
			for (size_t i = 0; i < backslashes * 2; i++)
				dstr_cat_ch(cmd, '\\');
			break;
		}

		size_t n = *p == '"' ? backslashes * 2 + 1 : backslashes;
		for (size_t i = 0; i < n; i++)
			dstr_cat_ch(cmd, '\\');
		dstr_cat_ch(cmd, *p);
	}
	dstr_cat_ch(cmd, '"');
}

struct helper_process *helper_process_start(const char *const *argv)
{
	SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
	HANDLE in_r = NULL, in_w = NULL, err_r = NULL, err_w = NULL, nul = INVALID_HANDLE_VALUE;
	LPPROC_THREAD_ATTRIBUTE_LIST attrs = NULL;
	wchar_t *exe_w = NULL, *cmd_w = NULL;
	struct helper_process *hp = NULL;
	struct dstr cmd = {0};

	if (!CreatePipe(&in_r, &in_w, &sa, 0) || !CreatePipe(&err_r, &err_w, &sa, 0))
		goto done;

	/* Only the child's ends are inheritable. */
	SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
	SetHandleInformation(err_r, HANDLE_FLAG_INHERIT, 0);

	nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
	if (nul == INVALID_HANDLE_VALUE)
		goto done;

	/* Inherit exactly these three handles, not every inheritable handle OBS has open. */
	HANDLE inherit[3] = {in_r, nul, err_w};
	SIZE_T attr_size = 0;
	InitializeProcThreadAttributeList(NULL, 1, 0, &attr_size);
	attrs = bmalloc(attr_size);
	if (!InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size) ||
	    !UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof(inherit), NULL,
				       NULL)) {
		bfree(attrs);
		attrs = NULL;
		goto done;
	}

	for (size_t i = 0; argv[i]; i++) {
		if (i)
			dstr_cat_ch(&cmd, ' ');
		append_quoted(&cmd, argv[i]);
	}

	os_utf8_to_wcs_ptr(argv[0], 0, &exe_w);
	os_utf8_to_wcs_ptr(cmd.array, 0, &cmd_w);
	if (!exe_w || !cmd_w)
		goto done;

	STARTUPINFOEXW si = {0};
	PROCESS_INFORMATION pi = {0};
	si.StartupInfo.cb = sizeof(si);
	si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
	si.StartupInfo.hStdInput = in_r;
	si.StartupInfo.hStdOutput = nul;
	si.StartupInfo.hStdError = err_w;
	si.lpAttributeList = attrs;

	if (!CreateProcessW(exe_w, cmd_w, NULL, NULL, TRUE, EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, NULL,
			    NULL, &si.StartupInfo, &pi))
		goto done;

	CloseHandle(pi.hThread);

	hp = bzalloc(sizeof(*hp));
	hp->process = pi.hProcess;
	hp->in_w = in_w;
	hp->err_r = err_r;
	in_w = NULL;
	err_r = NULL;

done:
	if (attrs) {
		DeleteProcThreadAttributeList(attrs);
		bfree(attrs);
	}
	if (in_r)
		CloseHandle(in_r);
	if (err_w)
		CloseHandle(err_w);
	if (in_w)
		CloseHandle(in_w);
	if (err_r)
		CloseHandle(err_r);
	if (nul != INVALID_HANDLE_VALUE)
		CloseHandle(nul);
	bfree(exe_w);
	bfree(cmd_w);
	dstr_free(&cmd);
	return hp;
}

bool helper_process_write(struct helper_process *hp, const void *buf, size_t len)
{
	const uint8_t *p = buf;

	while (len && hp->in_w) {
		DWORD chunk = len > 0x40000000 ? 0x40000000 : (DWORD)len;
		DWORD written = 0;
		if (!WriteFile(hp->in_w, p, chunk, &written, NULL) || !written)
			return false;
		p += written;
		len -= written;
	}
	return len == 0;
}

size_t helper_process_read_err(struct helper_process *hp, char *buf, size_t len)
{
	DWORD n = 0;
	if (!ReadFile(hp->err_r, buf, (DWORD)len, &n, NULL))
		return 0;
	return n;
}

void helper_process_close_stdin(struct helper_process *hp)
{
	if (hp->in_w) {
		CloseHandle(hp->in_w);
		hp->in_w = NULL;
	}
}

bool helper_process_wait(struct helper_process *hp, uint32_t timeout_ms, int *exit_code)
{
	if (WaitForSingleObject(hp->process, timeout_ms) != WAIT_OBJECT_0)
		return false;

	DWORD code = 0;
	GetExitCodeProcess(hp->process, &code);
	if (exit_code)
		*exit_code = (int)code;
	return true;
}

void helper_process_kill(struct helper_process *hp)
{
	if (!hp)
		return;

	helper_process_close_stdin(hp);
	if (WaitForSingleObject(hp->process, 0) != WAIT_OBJECT_0) {
		TerminateProcess(hp->process, 1);
		WaitForSingleObject(hp->process, 5000);
	}
}

void helper_process_destroy(struct helper_process *hp)
{
	if (!hp)
		return;

	helper_process_kill(hp);
	CloseHandle(hp->process);
	CloseHandle(hp->err_r);
	bfree(hp);
}

char *helper_process_find_executable(const char *name)
{
	struct dstr exe = {0};
	dstr_copy(&exe, name);
	if (!dstr_find(&exe, "."))
		dstr_cat(&exe, ".exe");

	char *found = NULL;
	struct dstr dirs = {0};
	dstr_copy(&dirs, getenv("PATH"));
	dstr_cat(&dirs, ";C:\\Program Files\\nodejs");

	char **list = strlist_split(dirs.array ? dirs.array : "", ';', false);
	for (char **dir = list; dir && *dir && !found; dir++) {
		struct dstr candidate = {0};
		dstr_printf(&candidate, "%s\\%s", *dir, exe.array);
		if (os_file_exists(candidate.array))
			found = bstrdup(candidate.array);
		dstr_free(&candidate);
	}

	strlist_free(list);
	dstr_free(&dirs);
	dstr_free(&exe);
	return found;
}

#else /* POSIX */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifdef __APPLE__
#include <crt_externs.h>
#define ENVIRON (*_NSGetEnviron())
#else
extern char **environ;
#define ENVIRON environ
#endif

struct helper_process {
	pid_t pid;
	int in_fd;
	int err_fd;
	bool reaped;
};

static void set_cloexec(int fd)
{
	int flags = fcntl(fd, F_GETFD);
	if (flags >= 0)
		fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

static void close_fd(int *fd)
{
	if (*fd >= 0) {
		close(*fd);
		*fd = -1;
	}
}

struct helper_process *helper_process_start(const char *const *argv)
{
	int in[2] = {-1, -1}, err[2] = {-1, -1};
	posix_spawn_file_actions_t actions;
	posix_spawnattr_t attr;
	struct helper_process *hp = NULL;

	if (pipe(in) != 0)
		return NULL;
	if (pipe(err) != 0) {
		close_fd(&in[0]);
		close_fd(&in[1]);
		return NULL;
	}

	/* dup2 in the child clears FD_CLOEXEC on 0 and 2, so all four can be set. */
	set_cloexec(in[0]);
	set_cloexec(in[1]);
	set_cloexec(err[0]);
	set_cloexec(err[1]);
#ifdef __APPLE__
	fcntl(in[1], F_SETNOSIGPIPE, 1);
#endif

	posix_spawn_file_actions_init(&actions);
	posix_spawn_file_actions_adddup2(&actions, in[0], STDIN_FILENO);
	posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
	posix_spawn_file_actions_adddup2(&actions, err[1], STDERR_FILENO);

	/* The calling OBS thread may block signals; give the child a clean slate. */
	sigset_t empty, defaults;
	sigemptyset(&empty);
	sigemptyset(&defaults);
	sigaddset(&defaults, SIGPIPE);

	short flags = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
#ifdef __APPLE__
	flags |= POSIX_SPAWN_CLOEXEC_DEFAULT; /* don't leak OBS's other descriptors */
#endif
	posix_spawnattr_init(&attr);
	posix_spawnattr_setflags(&attr, flags);
	posix_spawnattr_setsigmask(&attr, &empty);
	posix_spawnattr_setsigdefault(&attr, &defaults);

	pid_t pid;
	int ret = posix_spawn(&pid, argv[0], &actions, &attr, (char *const *)argv, ENVIRON);

	posix_spawn_file_actions_destroy(&actions);
	posix_spawnattr_destroy(&attr);
	close_fd(&in[0]);
	close_fd(&err[1]);

	if (ret != 0) {
		close_fd(&in[1]);
		close_fd(&err[0]);
		return NULL;
	}

	hp = bzalloc(sizeof(*hp));
	hp->pid = pid;
	hp->in_fd = in[1];
	hp->err_fd = err[0];
	return hp;
}

bool helper_process_write(struct helper_process *hp, const void *buf, size_t len)
{
	const uint8_t *p = buf;
	bool ok = true;

#ifndef __APPLE__
	/* Linux has no per-descriptor SIGPIPE switch: block it in this thread and
	 * swallow the one a write to a closed pipe raises. */
	sigset_t pipe_set, old_set;
	sigemptyset(&pipe_set);
	sigaddset(&pipe_set, SIGPIPE);
	pthread_sigmask(SIG_BLOCK, &pipe_set, &old_set);
#endif

	while (len && hp->in_fd >= 0) {
		ssize_t n = write(hp->in_fd, p, len);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			ok = false;
			break;
		}
		p += n;
		len -= (size_t)n;
	}
	if (len)
		ok = false;

#ifndef __APPLE__
	if (!ok && errno == EPIPE) {
		struct timespec zero = {0, 0};
		sigtimedwait(&pipe_set, NULL, &zero);
	}
	pthread_sigmask(SIG_SETMASK, &old_set, NULL);
#endif

	return ok;
}

size_t helper_process_read_err(struct helper_process *hp, char *buf, size_t len)
{
	for (;;) {
		ssize_t n = read(hp->err_fd, buf, len);
		if (n >= 0)
			return (size_t)n;
		if (errno != EINTR)
			return 0;
	}
}

void helper_process_close_stdin(struct helper_process *hp)
{
	close_fd(&hp->in_fd);
}

static bool reap(struct helper_process *hp, int options, int *exit_code)
{
	if (hp->reaped)
		return true;

	int status = 0;
	pid_t ret;
	do {
		ret = waitpid(hp->pid, &status, options);
	} while (ret < 0 && errno == EINTR);

	if (ret == 0)
		return false;

	hp->reaped = true;
	if (exit_code)
		*exit_code = (ret > 0 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
	return true;
}

bool helper_process_wait(struct helper_process *hp, uint32_t timeout_ms, int *exit_code)
{
	uint64_t deadline = os_gettime_ns() + (uint64_t)timeout_ms * 1000000ULL;

	for (;;) {
		if (reap(hp, WNOHANG, exit_code))
			return true;
		if (os_gettime_ns() >= deadline)
			return false;
		os_sleep_ms(20);
	}
}

void helper_process_kill(struct helper_process *hp)
{
	if (!hp)
		return;

	close_fd(&hp->in_fd);
	if (!reap(hp, WNOHANG, NULL)) {
		kill(hp->pid, SIGKILL);
		reap(hp, 0, NULL);
	}
}

void helper_process_destroy(struct helper_process *hp)
{
	if (!hp)
		return;

	helper_process_kill(hp);
	close_fd(&hp->err_fd);
	bfree(hp);
}

char *helper_process_find_executable(const char *name)
{
	char *found = NULL;
	struct dstr dirs = {0};
	dstr_copy(&dirs, getenv("PATH"));
	dstr_cat(&dirs, ":/opt/homebrew/bin:/usr/local/bin:/usr/bin");

	char **list = strlist_split(dirs.array ? dirs.array : "", ':', false);
	for (char **dir = list; dir && *dir && !found; dir++) {
		struct dstr candidate = {0};
		dstr_printf(&candidate, "%s/%s", *dir, name);
		if (access(candidate.array, X_OK) == 0)
			found = bstrdup(candidate.array);
		dstr_free(&candidate);
	}

	strlist_free(list);
	dstr_free(&dirs);
	return found;
}

#endif
