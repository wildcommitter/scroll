#define _GNU_SOURCE // syscall()
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/landlock.h>
#include <linux/seccomp.h>
#include <sched.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "sway/log.h"
#include "sway/sandbox.h"
#include "stringop.h"

#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "sandbox.c reads the low word of syscall arguments: little endian only"
#endif

#if defined(__x86_64__)
#define SANDBOX_AUDIT_ARCH AUDIT_ARCH_X86_64
#elif defined(__aarch64__)
#define SANDBOX_AUDIT_ARCH AUDIT_ARCH_AARCH64
#else
#error "-Dsandbox=true supports x86_64 and aarch64 only"
#endif

// --- Landlock ---

static int thread_count(void) {
	FILE *f = fopen("/proc/self/status", "r");
	if (!f) {
		return -1;
	}
	char line[256];
	int n = -1;
	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "Threads: %d", &n) == 1) {
			break;
		}
	}
	fclose(f);
	return n;
}


#define FS_RO_DIR (LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR)
#define FS_RW_DIR (FS_RO_DIR | LANDLOCK_ACCESS_FS_WRITE_FILE | \
	LANDLOCK_ACCESS_FS_REMOVE_DIR | LANDLOCK_ACCESS_FS_REMOVE_FILE | \
	LANDLOCK_ACCESS_FS_MAKE_DIR | LANDLOCK_ACCESS_FS_MAKE_REG | \
	LANDLOCK_ACCESS_FS_MAKE_SOCK | LANDLOCK_ACCESS_FS_MAKE_FIFO | \
	LANDLOCK_ACCESS_FS_MAKE_SYM | LANDLOCK_ACCESS_FS_REFER | \
	LANDLOCK_ACCESS_FS_TRUNCATE)
#define FS_DEV (FS_RO_DIR | LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_IOCTL_DEV)
// Rights that apply to a non-directory
#define FS_FILE_RIGHTS (LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_WRITE_FILE | \
	LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_TRUNCATE | \
	LANDLOCK_ACCESS_FS_IOCTL_DEV)

static uint64_t fs_handled_for_abi(int abi) {
	uint64_t fs = (LANDLOCK_ACCESS_FS_MAKE_SYM << 1) - 1; // ABI 1
	if (abi >= 2) {
		fs |= LANDLOCK_ACCESS_FS_REFER;
	}
	if (abi >= 3) {
		fs |= LANDLOCK_ACCESS_FS_TRUNCATE;
	}
	if (abi >= 5) {
		fs |= LANDLOCK_ACCESS_FS_IOCTL_DEV;
	}
	return fs;
}

struct landlock {
	int fd;
	uint64_t handled;
};

static bool ll_add(struct landlock *ll, const char *path, uint64_t rights) {
	if (!path || path[0] != '/') {
		return true;
	}
	int fd = open(path, O_PATH | O_CLOEXEC);
	if (fd < 0) {
		// Not present here; nothing to allow
		return true;
	}
	struct stat st;
	if (fstat(fd, &st) != 0) {
		close(fd);
		return true;
	}
	rights &= ll->handled;
	if (!S_ISDIR(st.st_mode)) {
		rights &= FS_FILE_RIGHTS;
	}
	struct landlock_path_beneath_attr attr = {
		.allowed_access = rights,
		.parent_fd = fd,
	};
	int ret = syscall(SYS_landlock_add_rule, ll->fd, LANDLOCK_RULE_PATH_BENEATH, &attr, 0);
	int err = errno;
	close(fd);
	if (ret != 0) {
		errno = err;
		sway_log_errno(SWAY_ERROR, "sandbox: landlock rule for %s failed", path);
		return false;
	}
	sway_log(SWAY_DEBUG, "sandbox: allow %s %s", path,
		(rights & LANDLOCK_ACCESS_FS_WRITE_FILE) ? "rw" : "ro");
	return true;
}

// Add base/sub, with base from env (or HOME/fallback when unset)
static bool ll_add_xdg(struct landlock *ll, const char *env, const char *fallback,
		const char *sub, uint64_t rights) {
	const char *home = getenv("HOME");
	const char *base = getenv(env);
	char *path;
	if (base && base[0] == '/') {
		path = format_str("%s/%s", base, sub);
	} else if (home && home[0] == '/') {
		path = format_str("%s/%s/%s", home, fallback, sub);
	} else {
		return true;
	}
	bool ok = path && ll_add(ll, path, rights);
	free(path);
	return ok;
}

static bool ll_add_home(struct landlock *ll, const char *sub, uint64_t rights) {
	const char *home = getenv("HOME");
	if (!home || home[0] != '/') {
		return true;
	}
	char *path = format_str("%s/%s", home, sub);
	bool ok = path && ll_add(ll, path, rights);
	free(path);
	return ok;
}

// Each ':'-separated entry of env, read-only
static bool ll_add_list(struct landlock *ll, const char *env) {
	const char *value = getenv(env);
	if (!value) {
		return true;
	}
	char *copy = strdup(value);
	if (!copy) {
		return false;
	}
	bool ok = true;
	char *save = NULL;
	for (char *p = strtok_r(copy, ":", &save); p && ok; p = strtok_r(NULL, ":", &save)) {
		ok = ll_add(ll, p, FS_RO_DIR);
	}
	free(copy);
	return ok;
}

static bool landlock_apply(const char *config_path) {
	int abi = syscall(SYS_landlock_create_ruleset, NULL, 0,
		LANDLOCK_CREATE_RULESET_VERSION);
	if (abi < 0) {
		// Best effort: the seccomp filter still applies
		sway_log_errno(SWAY_INFO, "sandbox: Landlock unavailable, skipping it");
		return true;
	}

	struct landlock_ruleset_attr attr = {
		.handled_access_fs = fs_handled_for_abi(abi),
	};
	size_t attr_size = offsetof(struct landlock_ruleset_attr, handled_access_net);
	if (abi >= 4) {
		// TCP: no rules, so no bind or connect
		attr.handled_access_net = LANDLOCK_ACCESS_NET_BIND_TCP |
			LANDLOCK_ACCESS_NET_CONNECT_TCP;
		attr_size = offsetof(struct landlock_ruleset_attr, handled_access_net) +
			sizeof(attr.handled_access_net);
	}
#ifdef LANDLOCK_SCOPE_SIGNAL
	if (abi >= 6) {
		attr.scoped = LANDLOCK_SCOPE_ABSTRACT_UNIX_SOCKET | LANDLOCK_SCOPE_SIGNAL;
		attr_size = sizeof(attr);
	}
#endif

	struct landlock ll = { .handled = attr.handled_access_fs };
	ll.fd = syscall(SYS_landlock_create_ruleset, &attr, attr_size, 0);
	if (ll.fd < 0) {
		sway_log_errno(SWAY_ERROR, "sandbox: landlock_create_ruleset failed");
		return false;
	}

	bool ok = true;
	// System data, libraries loaded later, udev and hotplug, config reload
	const char *ro[] = {
		"/usr", "/etc", "/sys", "/proc", "/run/udev",
		"/var/lib/flatpak/exports", "/var/cache/fontconfig",
		"/dev/urandom", "/dev/random", "/dev/zero",
	};
	for (size_t i = 0; ok && i < sizeof(ro) / sizeof(ro[0]); i++) {
		ok = ll_add(&ll, ro[i], FS_RO_DIR);
	}
	ok = ok && ll_add(&ll, "/dev/null", FS_RO_DIR | LANDLOCK_ACCESS_FS_WRITE_FILE);
	// DRM nodes (render node, hotplug) and evdev
	ok = ok && ll_add(&ll, "/dev/dri", FS_DEV);
	ok = ok && ll_add(&ll, "/dev/input", FS_DEV);
	// shm_open fallback of the shm allocator
	ok = ok && ll_add(&ll, "/dev/shm", FS_RW_DIR);
	// Wayland socket, lock state file
	ok = ok && ll_add(&ll, getenv("XDG_RUNTIME_DIR"), FS_RW_DIR);

	// Icon/cursor themes, fonts, xkb and Mesa config under HOME; the rest of
	// HOME stays unreadable
	ok = ok && ll_add_list(&ll, "XDG_DATA_DIRS");
	ok = ok && ll_add_list(&ll, "XCURSOR_PATH");
	ok = ok && ll_add_xdg(&ll, "XDG_DATA_HOME", ".local/share", "icons", FS_RO_DIR);
	ok = ok && ll_add_xdg(&ll, "XDG_DATA_HOME", ".local/share", "fonts", FS_RO_DIR);
	ok = ok && ll_add_xdg(&ll, "XDG_DATA_HOME", ".local/share", "flatpak/exports/share", FS_RO_DIR);
	ok = ok && ll_add_xdg(&ll, "XDG_CONFIG_HOME", ".config", "fontconfig", FS_RO_DIR);
	ok = ok && ll_add_xdg(&ll, "XDG_CONFIG_HOME", ".config", "xkb", FS_RO_DIR);
	ok = ok && ll_add_xdg(&ll, "XDG_CACHE_HOME", ".cache", "fontconfig", FS_RW_DIR);
	ok = ok && ll_add_xdg(&ll, "XDG_CACHE_HOME", ".cache", "mesa_shader_cache", FS_RW_DIR);
	ok = ok && ll_add_xdg(&ll, "XDG_CACHE_HOME", ".cache", "mesa_shader_cache_db", FS_RW_DIR);
	ok = ok && ll_add_home(&ll, ".icons", FS_RO_DIR);
	ok = ok && ll_add_home(&ll, ".fonts", FS_RO_DIR);
	ok = ok && ll_add_home(&ll, ".xkb", FS_RO_DIR);
	ok = ok && ll_add_home(&ll, ".drirc", FS_RO_DIR);

	// The main config's directory, for reload
	if (ok && config_path) {
		char *dir = strdup(config_path);
		char *slash = dir ? strrchr(dir, '/') : NULL;
		if (slash && slash != dir) {
			*slash = '\0';
			ok = ll_add(&ll, dir, FS_RO_DIR);
		}
		free(dir);
	}

	bool all_threads = false;
	if (ok) {
		int ret = -1;
#ifdef LANDLOCK_RESTRICT_SELF_TSYNC
		// Every thread, not only this one
		ret = syscall(SYS_landlock_restrict_self, ll.fd, LANDLOCK_RESTRICT_SELF_TSYNC);
		all_threads = ret == 0;
#endif
		if (ret != 0) {
			ret = syscall(SYS_landlock_restrict_self, ll.fd, 0);
		}
		if (ret != 0) {
			sway_log_errno(SWAY_ERROR, "sandbox: landlock_restrict_self failed");
			ok = false;
		}
	}
	close(ll.fd);
	if (ok) {
		int threads = thread_count();
		if (!all_threads && threads > 1) {
			sway_log(SWAY_INFO, "sandbox: no Landlock TSYNC; %d threads exist and "
				"only this one and its future threads are restricted", threads);
		}
		sway_log(SWAY_INFO, "sandbox: Landlock ABI %d applied%s", abi,
			all_threads ? " to all threads" : "");
	}
	return ok;
}

// --- seccomp ---

#define SC_LOAD(field) BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, field))
#define SC_RET(val) BPF_STMT(BPF_RET | BPF_K, (val))
#define SC_ERRNO(e) SC_RET(SECCOMP_RET_ERRNO | ((e) & SECCOMP_RET_DATA))
#define SC_DENY(nr) BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (nr), 0, 1), SC_ERRNO(EPERM)

#define CLONE_NS_FLAGS (CLONE_NEWNS | CLONE_NEWUSER | CLONE_NEWPID | \
	CLONE_NEWNET | CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWCGROUP)

static bool seccomp_apply(void) {
	struct sock_filter filter[] = {
		SC_LOAD(arch),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SANDBOX_AUDIT_ARCH, 1, 0),
		SC_RET(SECCOMP_RET_KILL_PROCESS),
		SC_LOAD(nr),
#ifdef __x86_64__
		// x32 syscalls
		BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, 0x40000000, 0, 1),
		SC_RET(SECCOMP_RET_KILL_PROCESS),
#endif
		// exec: the broker does it
		SC_DENY(__NR_execve),
		SC_DENY(__NR_execveat),
		// other processes' memory
		SC_DENY(__NR_ptrace),
		SC_DENY(__NR_process_vm_readv),
		SC_DENY(__NR_process_vm_writev),
		SC_DENY(__NR_pidfd_getfd),
		// kernel attack surface
		SC_DENY(__NR_bpf),
		SC_DENY(__NR_perf_event_open),
		SC_DENY(__NR_userfaultfd),
		SC_DENY(__NR_keyctl),
		SC_DENY(__NR_add_key),
		SC_DENY(__NR_request_key),
		SC_DENY(__NR_io_uring_setup),
		SC_DENY(__NR_io_uring_enter),
		SC_DENY(__NR_io_uring_register),
		SC_DENY(__NR_open_by_handle_at),
		// mounts and namespaces
		SC_DENY(__NR_mount),
		SC_DENY(__NR_umount2),
		SC_DENY(__NR_pivot_root),
		SC_DENY(__NR_chroot),
		SC_DENY(__NR_move_mount),
		SC_DENY(__NR_open_tree),
		SC_DENY(__NR_fsopen),
		SC_DENY(__NR_fsconfig),
		SC_DENY(__NR_fsmount),
		SC_DENY(__NR_fspick),
		SC_DENY(__NR_mount_setattr),
		SC_DENY(__NR_unshare),
		SC_DENY(__NR_setns),
		// system administration
		SC_DENY(__NR_kexec_load),
		SC_DENY(__NR_kexec_file_load),
		SC_DENY(__NR_init_module),
		SC_DENY(__NR_finit_module),
		SC_DENY(__NR_delete_module),
		SC_DENY(__NR_acct),
		SC_DENY(__NR_swapon),
		SC_DENY(__NR_swapoff),
		SC_DENY(__NR_reboot),
		SC_DENY(__NR_quotactl),
		SC_DENY(__NR_syslog),
		SC_DENY(__NR_vhangup),
		SC_DENY(__NR_settimeofday),
		SC_DENY(__NR_clock_settime),
		SC_DENY(__NR_clock_adjtime),
		SC_DENY(__NR_adjtimex),
#ifdef __x86_64__
		SC_DENY(__NR_iopl),
		SC_DENY(__NR_ioperm),
		SC_DENY(__NR_modify_ldt),
		SC_DENY(__NR_uselib),
		SC_DENY(__NR_ustat),
		SC_DENY(__NR_sysfs),
		SC_DENY(__NR__sysctl),
		SC_DENY(__NR_lookup_dcookie),
#endif
		// clone3 hides its flags in memory: glibc falls back to clone
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_clone3, 0, 1),
		SC_ERRNO(ENOSYS),
		// personality: only the query
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_personality, 0, 4),
		SC_LOAD(args[0]),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0xffffffff, 0, 1),
		SC_RET(SECCOMP_RET_ALLOW),
		SC_ERRNO(EPERM),
		// clone: threads and plain forks, no namespaces
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_clone, 0, 4),
		SC_LOAD(args[0]),
		BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, CLONE_NS_FLAGS, 0, 1),
		SC_ERRNO(EPERM),
		SC_RET(SECCOMP_RET_ALLOW),
		// sockets: unix (Wayland, D-Bus) and netlink (udev) only
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_socket, 0, 5),
		SC_LOAD(args[0]),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AF_UNIX, 2, 0),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AF_NETLINK, 1, 0),
		SC_ERRNO(EAFNOSUPPORT),
		SC_RET(SECCOMP_RET_ALLOW),
		SC_RET(SECCOMP_RET_ALLOW),
	};
	struct sock_fprog prog = {
		.len = sizeof(filter) / sizeof(filter[0]),
		.filter = filter,
	};
	if (syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_TSYNC,
			&prog) != 0) {
		sway_log_errno(SWAY_ERROR, "sandbox: installing the seccomp filter failed");
		return false;
	}
	sway_log(SWAY_INFO, "sandbox: seccomp filter applied (%u instructions)", prog.len);
	return true;
}

bool sandbox_apply(const char *config_path) {
	// Required for seccomp and Landlock without CAP_SYS_ADMIN. The broker was
	// forked before this, so children don't get it.
	if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
		sway_log_errno(SWAY_ERROR, "sandbox: PR_SET_NO_NEW_PRIVS failed");
		return false;
	}
	if (!landlock_apply(config_path)) {
		return false;
	}
	return seccomp_apply();
}
