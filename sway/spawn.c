#define _GNU_SOURCE // MSG_CMSG_CLOEXEC
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include "sway/log.h"
#include "sway/spawn.h"

extern char **environ;

// Request: header, then argc + envc NUL-terminated strings, with the optional
// fds (wayland socket first, then stdin) as SCM_RIGHTS. Reply: an int32_t,
// the child's pid or -errno.
#define SPAWN_MAGIC 0x73706e31
#define SPAWN_MAX_MSG (256 * 1024)
#define SPAWN_MAX_STRINGS 4096

enum spawn_flags {
	SPAWN_SETSID = 1 << 0,
	SPAWN_WAYLAND_FD = 1 << 1,
	SPAWN_STDIN_FD = 1 << 2,
};

struct spawn_header {
	uint32_t magic;
	uint32_t flags;
	uint32_t argc;
	uint32_t envc;
};

static int broker_fd = -1;

static void child_exec(char **argv, char **envp, uint32_t flags,
		int wayland_fd, int stdin_fd) {
	sigset_t set;
	sigemptyset(&set);
	sigprocmask(SIG_SETMASK, &set, NULL);
	struct sigaction sa_dfl = { .sa_handler = SIG_DFL };
	sigaction(SIGCHLD, &sa_dfl, NULL);
	sigaction(SIGPIPE, &sa_dfl, NULL);

	if (flags & SPAWN_SETSID) {
		setsid();
	}
	if (stdin_fd >= 0 && dup2(stdin_fd, STDIN_FILENO) < 0) {
		_exit(127);
	}

	size_t envc = 0;
	while (envp[envc]) {
		envc++;
	}
	char **env = calloc(envc + 2, sizeof(char *));
	if (!env) {
		_exit(127);
	}
	size_t n = 0;
	for (size_t i = 0; i < envc; i++) {
		if (strncmp(envp[i], "WAYLAND_SOCKET=", 15) != 0) {
			env[n++] = envp[i];
		}
	}
	if (wayland_fd >= 0) {
		int fl = fcntl(wayland_fd, F_GETFD);
		if (fl < 0 || fcntl(wayland_fd, F_SETFD, fl & ~FD_CLOEXEC) < 0) {
			_exit(127);
		}
		char *ws = malloc(32);
		if (!ws) {
			_exit(127);
		}
		snprintf(ws, 32, "WAYLAND_SOCKET=%d", wayland_fd);
		env[n++] = ws;
	}
	env[n] = NULL;

	environ = env;
	execvp(argv[0], argv);
	sway_log_errno(SWAY_ERROR, "execvp %s failed", argv[0]);
	_exit(127);
}

static int32_t broker_handle(char *buf, size_t len, int *fds, int nfds) {
	if (len < sizeof(struct spawn_header)) {
		return -EINVAL;
	}
	struct spawn_header hdr;
	memcpy(&hdr, buf, sizeof(hdr));
	if (hdr.magic != SPAWN_MAGIC || hdr.argc == 0 ||
			hdr.argc > SPAWN_MAX_STRINGS || hdr.envc > SPAWN_MAX_STRINGS) {
		return -EINVAL;
	}
	int want = !!(hdr.flags & SPAWN_WAYLAND_FD) + !!(hdr.flags & SPAWN_STDIN_FD);
	if (want != nfds) {
		return -EINVAL;
	}

	char *p = buf + sizeof(hdr), *end = buf + len;
	if (p == end || end[-1] != '\0') {
		return -EINVAL;
	}
	size_t total = hdr.argc + hdr.envc;
	char **strs = calloc(total + 2, sizeof(char *));
	if (!strs) {
		return -ENOMEM;
	}
	size_t n = 0;
	while (p < end) {
		if (n == total) {
			free(strs);
			return -EINVAL;
		}
		strs[n++] = p;
		p += strlen(p) + 1;
	}
	if (n != total) {
		free(strs);
		return -EINVAL;
	}
	// argv and envp share the array: argv[argc] becomes the separator
	char **argv = strs;
	char **envp = calloc(hdr.envc + 1, sizeof(char *));
	if (!envp) {
		free(strs);
		return -ENOMEM;
	}
	memcpy(envp, strs + hdr.argc, hdr.envc * sizeof(char *));
	argv[hdr.argc] = NULL;

	int i = 0;
	int wayland_fd = (hdr.flags & SPAWN_WAYLAND_FD) ? fds[i++] : -1;
	int stdin_fd = (hdr.flags & SPAWN_STDIN_FD) ? fds[i++] : -1;

	pid_t pid = fork();
	if (pid == 0) {
		child_exec(argv, envp, hdr.flags, wayland_fd, stdin_fd);
	}
	int32_t res = pid < 0 ? -errno : pid;
	free(envp);
	free(strs);
	return res;
}

static void broker_run(int fd) {
	prctl(PR_SET_NAME, "scroll-spawn", 0, 0, 0);
	// The compositor exiting must not leave the broker behind
	prctl(PR_SET_PDEATHSIG, SIGTERM, 0, 0, 0);
	struct sigaction sa_ign = { .sa_handler = SIG_IGN };
	// avoid need to reap children
	sigaction(SIGCHLD, &sa_ign, NULL);
	sigaction(SIGPIPE, &sa_ign, NULL);

	char *buf = malloc(SPAWN_MAX_MSG);
	if (!buf) {
		_exit(EXIT_FAILURE);
	}
	while (true) {
		union {
			char buf[CMSG_SPACE(2 * sizeof(int))];
			struct cmsghdr align;
		} cbuf;
		struct iovec iov = { .iov_base = buf, .iov_len = SPAWN_MAX_MSG };
		struct msghdr msg = {
			.msg_iov = &iov,
			.msg_iovlen = 1,
			.msg_control = cbuf.buf,
			.msg_controllen = sizeof(cbuf.buf),
		};
		ssize_t len = recvmsg(fd, &msg, MSG_CMSG_CLOEXEC);
		if (len == 0) {
			_exit(EXIT_SUCCESS);
		} else if (len < 0) {
			if (errno == EINTR) {
				continue;
			}
			_exit(EXIT_FAILURE);
		}

		int fds[2] = { -1, -1 };
		int nfds = 0;
		bool bad = (msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0;
		for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
			if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) {
				bad = true;
				continue;
			}
			size_t count = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
			int *in = (int *)CMSG_DATA(c);
			for (size_t j = 0; j < count; j++) {
				if (nfds < 2) {
					fds[nfds++] = in[j];
				} else {
					close(in[j]);
					bad = true;
				}
			}
		}

		int32_t res = bad ? -EINVAL : broker_handle(buf, len, fds, nfds);
		for (int j = 0; j < nfds; j++) {
			close(fds[j]);
		}
		if (send(fd, &res, sizeof(res), MSG_NOSIGNAL) != sizeof(res)) {
			_exit(EXIT_FAILURE);
		}
	}
}

bool spawn_init(void) {
	int sv[2];
	if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) != 0) {
		sway_log_errno(SWAY_ERROR, "spawn broker: socketpair failed");
		return false;
	}
	pid_t pid = fork();
	if (pid < 0) {
		sway_log_errno(SWAY_ERROR, "spawn broker: fork failed");
		close(sv[0]);
		close(sv[1]);
		return false;
	} else if (pid == 0) {
		close(sv[0]);
		broker_run(sv[1]);
		_exit(EXIT_SUCCESS);
	}
	close(sv[1]);
	broker_fd = sv[0];
	sway_log(SWAY_DEBUG, "Spawn broker started with pid %d", pid);
	return true;
}

void spawn_fini(void) {
	if (broker_fd >= 0) {
		close(broker_fd);
		broker_fd = -1;
	}
}

static bool env_overridden(const char *entry, const char *const *env) {
	const char *eq = strchr(entry, '=');
	size_t name_len = eq ? (size_t)(eq - entry) : strlen(entry);
	if (name_len == 14 && strncmp(entry, "WAYLAND_SOCKET", 14) == 0) {
		return true;
	}
	for (size_t i = 0; env && env[i]; i++) {
		if (strncmp(env[i], entry, name_len) == 0 && env[i][name_len] == '=') {
			return true;
		}
	}
	return false;
}

static bool buf_add(char *buf, size_t *len, const char *s) {
	size_t n = strlen(s) + 1;
	if (*len + n > SPAWN_MAX_MSG) {
		return false;
	}
	memcpy(buf + *len, s, n);
	*len += n;
	return true;
}

pid_t spawn_argv(char *const argv[], const struct spawn_options *opts) {
	if (broker_fd < 0) {
		sway_log(SWAY_ERROR, "spawn broker not running, can't run %s", argv[0]);
		return -1;
	}
	struct spawn_options none = { .wayland_fd = -1, .stdin_fd = -1 };
	if (!opts) {
		opts = &none;
	}

	char *buf = malloc(SPAWN_MAX_MSG);
	if (!buf) {
		return -1;
	}
	struct spawn_header hdr = { .magic = SPAWN_MAGIC };
	size_t len = sizeof(hdr);
	bool ok = true;
	for (; argv[hdr.argc] && ok; hdr.argc++) {
		ok = buf_add(buf, &len, argv[hdr.argc]);
	}
	for (size_t i = 0; environ[i] && ok; i++) {
		if (!env_overridden(environ[i], opts->env)) {
			ok = buf_add(buf, &len, environ[i]);
			hdr.envc++;
		}
	}
	for (size_t i = 0; opts->env && opts->env[i] && ok; i++) {
		ok = buf_add(buf, &len, opts->env[i]);
		hdr.envc++;
	}
	if (!ok || hdr.argc == 0) {
		sway_log(SWAY_ERROR, "spawn request for %s too large", argv[0]);
		free(buf);
		return -1;
	}

	int fds[2];
	int nfds = 0;
	if (opts->setsid) {
		hdr.flags |= SPAWN_SETSID;
	}
	if (opts->wayland_fd >= 0) {
		hdr.flags |= SPAWN_WAYLAND_FD;
		fds[nfds++] = opts->wayland_fd;
	}
	if (opts->stdin_fd >= 0) {
		hdr.flags |= SPAWN_STDIN_FD;
		fds[nfds++] = opts->stdin_fd;
	}
	memcpy(buf, &hdr, sizeof(hdr));

	union {
		char buf[CMSG_SPACE(2 * sizeof(int))];
		struct cmsghdr align;
	} cbuf;
	memset(&cbuf, 0, sizeof(cbuf));
	struct iovec iov = { .iov_base = buf, .iov_len = len };
	struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1 };
	if (nfds > 0) {
		msg.msg_control = cbuf.buf;
		msg.msg_controllen = CMSG_SPACE(nfds * sizeof(int));
		struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
		c->cmsg_level = SOL_SOCKET;
		c->cmsg_type = SCM_RIGHTS;
		c->cmsg_len = CMSG_LEN(nfds * sizeof(int));
		memcpy(CMSG_DATA(c), fds, nfds * sizeof(int));
	}

	ssize_t sent;
	do {
		sent = sendmsg(broker_fd, &msg, MSG_NOSIGNAL);
	} while (sent < 0 && errno == EINTR);
	free(buf);
	if (sent != (ssize_t)len) {
		sway_log_errno(SWAY_ERROR, "spawn broker: send failed");
		return -1;
	}

	int32_t res;
	ssize_t got;
	do {
		got = recv(broker_fd, &res, sizeof(res), 0);
	} while (got < 0 && errno == EINTR);
	if (got != sizeof(res)) {
		sway_log_errno(SWAY_ERROR, "spawn broker: no reply");
		return -1;
	}
	if (res < 0) {
		errno = -res;
		sway_log_errno(SWAY_ERROR, "spawn broker: could not run %s", argv[0]);
		return -1;
	}
	return res;
}

pid_t spawn_shell(const char *cmd, const struct spawn_options *opts) {
	char *const argv[] = { "sh", "-c", (char *)cmd, NULL };
	return spawn_argv(argv, opts);
}
