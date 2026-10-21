# Fork notes (wildcommitter/scroll)

A downstream fork of [dawsers/scroll](https://github.com/dawsers/scroll), consumed pinned by
the scroll-secure image (`image/Containerfile`: `SCROLL_REF`/`SCROLL_COMMIT`). `master`
tracks upstream plus this file. Hardening patches live on `hardening` (see below), which
is rebased onto upstream.

```sh
git remote add upstream https://github.com/dawsers/scroll.git
git fetch upstream --tags && git merge --ff-only upstream/master   # or: gh repo sync
```

## What it is

scroll is a fork of sway, with scrolling tiling in the style of PaperWM and niri, animations
and Lua scripting. It's about 67k lines of C outside `subprojects/`. It's mostly one
maintainer: dawsers has about 760 of the 790 commits from the last year.

## Layout

| Path | What | Notes |
|---|---|---|
| `sway/tree/layout.c` (5k lines) | Scrolling layout: rows and columns, sizes, jumps, overview | The core of scroll. Most crash fixes land here and in `container.c` and `workspace.c` |
| `sway/tree/{container,view,workspace,space}.c` | Tree model inherited from sway; `space.c` is new in scroll (saved layouts) | |
| `sway/desktop/animation.c` | Animation curves, driving the wlroots scene | Reworked on 2026-09-11; also touches the wlroots copy |
| `sway/lua.c`, `commands/lua.c`, `scroll.lua` | Lua 5.4 API | Runs with the privileges of the config, so only trusted scripts |
| `sway/lock.c` | `ext-session-lock-v1` | hardened-unlock depends on this, so read every change to it |
| `sway/commands/` | One file per command (sway's model) | New in scroll: `align`, `cycle_size`, `fit_size`, `jump`, `overview`, `set_mode`, … |
| `sway/input/` | Seat, cursor, keyboard; `seatop_scroll_tiling.c` is new | |
| `sway/ipc-*.c`, `swaymsg/` | Sway's IPC, extended for scroll | |
| `subprojects/wlroots/` | **A full copy of wlroots 0.21-dev, modified in-tree** (not a wrap) | Linked statically. Distro wlroots updates never reach it |
| `tests/` | pytest plus Wayland and X11 test clients, built with ASan | Many regression tests for use-after-free and crashes |
| `swaybar/`, `swaynag/`, `client/`, `common/` | Same as sway | |

## Hardening patches (`hardening` branch)

| Switch | Build | Runtime | When it's off |
|---|---|---|---|
| Lua API | `-Dlua=disabled` (Lua isn't linked or compiled) | `SCROLL_DISABLE_LUA=1` | `lua`, `lua_eval` and IPC Lua eval return an error and no Lua runs |
| Privileged protocols | always | `privileged_clients all` (config only) restores sway | Only clients scroll starts over `WAYLAND_SOCKET` (swaybg, scrollnag, scrollbar, `exec_privileged`, `lock_command`) can bind screen capture (including the ext-image-capture-source managers), virtual input, data-control, input-method, session-lock, layer-shell, toplevel/workspace/output management, gamma and DRM leases. Only holds with `-Dipc=false` |
| Locking | always | `lock`, SIGUSR1, `scroll-lock` | `lock` starts `lock_command` as a privileged client. `$XDG_RUNTIME_DIR/scroll-locked.$SCROLL_PID` exists while locked; `scroll-lock` signals scroll and waits for it (for swayidle `before-sleep`) |
| Process sandbox | `-Dsandbox=true` (needs `-Dwlroots:xwayland=disabled`) | none: env vars are attacker-writable (environment.d, profile) | Default: no sandbox, but exec still goes through the broker. See [Sandbox](#sandbox-dsandboxtrue) |
| VM windows | always | none (also with `privileged_clients all`) | Only a privileged client can commit a `wp_security_context_v1` whose sandbox engine contains `scroll-vm`; others get `invalid_metadata` and no listener. `border_color <#rrggbb[aa]>\|none` (criteria, `for_window`) gives a window a frame color above every other state (urgent, selected, pinned, sticky) and the split indicator, darkened to 55% when unfocused. See [VM windows](#vm-windows) |
| IPC socket | `-Dipc=false` (the socket, read and command-dispatch code is compiled out; scrollmsg isn't built) | `SCROLL_DISABLE_IPC=1` | No socket is created, `SWAYSOCK`/`I3SOCK`/`SCROLLSOCK` are unset, and `bar {}` isn't started. scrollmsg, scrollbar and waybar stop working |

The minimal build is `-Dlua=disabled -Dipc=false -Dscrollbar=false` (scroll-secure adds
`-Dsandbox=true` and the wlroots options below). wayvnc still works,
because it uses Wayland protocols, not IPC. `tests/test_ipc_disabled.py` checks this with an
RFB handshake, with wayvnc started by `exec_privileged`; started outside scroll it gets no
virtual input. `tests/test_privileged.py` covers the allowlist and locking, using the
stdlib Wayland client `tests/wl_client.py`. Most of the pytest suite talks over IPC and Lua, so run it on a default build.
`liblua` can still show up in `ldd` through libinput; scroll itself doesn't link it.

CI (`.github/workflows/ci.yml`, Fedora 44) runs on push, PRs and weekly. It runs the full
ASan suite on the default build and on a `-Dsandbox=true` build, then builds the minimal
image configuration (plus `-Dwlroots:xwayland=disabled -Dwlroots:renderers=gles2
-Dwlroots:backends=drm,libinput -Dsandbox=true`).
It tests that build with wayvnc and checks with nm that IPC, Lua, Xwayland and Vulkan are
gone. The weekly run's summary lists the upstream commits and chases this branch is missing.

## VM windows

Qubes-style frames for guest apps. A helper started with `exec_privileged` creates a security
context with sandbox engine `scroll-vm` and app and instance ID `<vm>`, and runs `waypipe
client` on its listener. Guest windows are clients of that context, so a rule can mark them:

```
for_window [sandbox_engine="^scroll-vm$" sandbox_app_id="^work$"] border_color #cc3333, border normal 3
```

- **Unspoofable engine.** `sway/privileged.c` installs a `commit_filter` on the security
  context manager. That hook is ours (in the wlroots copy, `types/wlr_security_context_v1.c`),
  consulted before the listener is armed, because the `commit` event can't refuse. Engines
  that merely contain `scroll-vm` are refused too, since criteria are unanchored regexes.
- **No nesting.** A sandboxed client never sees `wp_security_context_manager_v1`
  (`filter_global`), and wlroots refuses a nested commit with the `nested` error anyway. A
  guest can't create contexts of its own.
- **The color is the identity, not the title.** `title_format` text such as `[work]` is
  next to a title the client controls. Anchor `sandbox_app_id` too, or VM `network` matches
  `work`.
- **pixman draws decorations.** Upstream's pixman renderer had an empty
  `add_decoration`, so headless or GPU-less sessions drew no borders and no title bar
  background at all, for any window. `render/pixman/pass.c` in the wlroots copy now fills
  the title bar and the four borders like the gles2 shader, with square corners
  (`border_radius` is ignored) and no antialiasing. Shadows are still not drawn with pixman.
- IPC `get_tree` shows `"border_color": "#rrggbbaa"` on containers that have one.
- `tests/test_vm_windows.py` (default build, including a screencopy check of all four
  sides, focused and unfocused, for `border normal 3` and `border pixel 3`) and the `seccontext` cases in
  `tests/test_ipc_disabled.py` (minimal build) cover this.

## Sandbox (`-Dsandbox=true`)

**Spawn broker** (`sway/spawn.c`, every build). `main()` forks `scroll-spawn` before any
socket exists, any config is read or the nofile limit is raised. All fork/exec goes through
it over a `SOCK_SEQPACKET` socketpair: `exec`, `exec_always`, `exec_privileged`,
`lock_command`, swaybg, scrollnag, scrollbar and Lua's `spawn`. A request carries argv, the
compositor's current environment plus overrides (`XDG_ACTIVATION_TOKEN`,
`DESKTOP_STARTUP_ID`), and optional fds by `SCM_RIGHTS`: the privileged `WAYLAND_SOCKET`
end, and scrollnag's stdin. The broker replies with the child's real pid, so launcher
workspace tracking keeps working. Children reset the signal mask and dispositions, and they
never inherit the compositor's seccomp, Landlock or `NO_NEW_PRIVS`.

The broker still runs whatever the compositor asks for. A compromised compositor can make it
exec arbitrary commands. It could do that anyway: it owns the keyboard and the screen, so
it can type into a terminal. The broker exists so the compositor itself can drop exec and
the kernel attack surface without breaking `exec`. It isn't an allowlist.

**seccomp** (fail closed: scroll exits if the filter can't be installed). Hand-written BPF.
It checks the architecture (x86_64 or aarch64), kills on a mismatch or an x32 syscall, and
returns `EPERM` for:

- exec: `execve`, `execveat`
- other processes: `ptrace`, `process_vm_readv/writev`, `pidfd_getfd`
- kernel surface: `bpf`, `perf_event_open`, `userfaultfd`, `keyctl`, `add_key`,
  `request_key`, `io_uring_setup/enter/register`, `open_by_handle_at`
- mounts and namespaces: `mount`, `umount2`, `pivot_root`, `chroot`, `move_mount`,
  `open_tree`, `fsopen`, `fsconfig`, `fsmount`, `fspick`, `mount_setattr`, `unshare`,
  `setns`, and `clone` with any `CLONE_NEW*` flag
- administration: `kexec_load`, `kexec_file_load`, `init/finit/delete_module`, `acct`,
  `swapon/off`, `reboot`, `quotactl`, `syslog`, `vhangup`, `settimeofday`,
  `clock_settime`, `clock_adjtime`, `adjtimex`
- x86_64 only: `iopl`, `ioperm`, `modify_ldt`, `uselib`, `ustat`, `sysfs`, `_sysctl`,
  `lookup_dcookie`
- `personality` except the `0xffffffff` query
- `socket` except `AF_UNIX` (Wayland, D-Bus) and `AF_NETLINK` (udev), which returns
  `EAFNOSUPPORT`
- `clone3` returns `ENOSYS`, so glibc falls back to `clone`, whose flags the filter can see

`kcmp` and `name_to_handle_at` stay allowed. Mesa (fd comparison) and libudev use them.

**Landlock** (best effort: logged and skipped when the kernel has none). It handles every
filesystem right of the running ABI, up to `IOCTL_DEV`. From ABI 4 it handles TCP
bind/connect with no rules, and from ABI 6 abstract-socket and signal scoping. It restricts
all threads when `LANDLOCK_RESTRICT_SELF_TSYNC` is available; otherwise it logs how many
threads were left out.

| Access | Paths |
|---|---|
| Read | `/usr`, `/etc`, `/sys`, `/proc`, `/run/udev`, `/var/lib/flatpak/exports`, `/var/cache/fontconfig`, `/dev/{urandom,random,zero}`, each `XDG_DATA_DIRS` and `XCURSOR_PATH` entry, `$XDG_DATA_HOME/{icons,fonts,flatpak/exports/share}`, `$XDG_CONFIG_HOME/{fontconfig,xkb}`, `~/.icons`, `~/.fonts`, `~/.xkb`, `~/.drirc`, the main config file's directory |
| Read + write | `$XDG_RUNTIME_DIR` (Wayland socket, lock state file), `/dev/shm`, `/dev/null`, `$XDG_CACHE_HOME/{fontconfig,mesa_shader_cache,mesa_shader_cache_db}` |
| Read + write + ioctl | `/dev/dri`, `/dev/input` |
| Nothing | everything else, so the rest of `HOME` (`~/.ssh`, documents) and `/tmp` |

Paths that don't exist when the sandbox is applied get no rule. The sandbox goes on right
before the event loop, after the first config load, output setup, scrollbar, scrollnag and
swaybg have started.

**Limits:**

- On reload, files the config `include`s, `lua` scripts, `output … bg` images and
  `xkb_file` paths must be under an allowed path, usually `/etc` or the config's
  directory. Otherwise they fail like a missing file.
- Fds opened before the sandbox, and fds received from logind (DRM and evdev via libseat),
  keep their access.
- Connecting to named Unix sockets isn't restricted (`LANDLOCK_ACCESS_FS_RESOLVE_UNIX`
  isn't handled yet). The compositor can still reach D-Bus, the system bus included.
- Xwayland must be off, because wlroots execs it from inside the compositor.
- `os.execute` and `io.popen` in Lua fail, since they exec in-process. `spawn` from Lua
  works (broker).
- LeakSanitizer's leak check uses ptrace. Run ASan builds with `detect_leaks=0`, as the
  tests do.

**VM smoke test** (for scroll-secure, on the DRM backend with a virtual GPU):

1. **Start:** the log shows `sandbox: Landlock ABI N applied` and `seccomp filter
   applied`, and `/proc/<pid>/status` shows `Seccomp: 2`, `NoNewPrivs: 1`.
2. **Render node:** with `WLR_RENDERER=gles2`, scroll starts and draws. Mesa opens
   `/dev/dri/renderD128` under the sandbox. Expect no `Permission denied` from EGL or GBM
   in the log.
3. **Output hotplug:** add a display, or virtio-gpu `max_outputs=2` plus QEMU monitor
   `set_link`, then remove it. The output appears and disappears, and nothing in the log
   says `/sys` or `/run/udev` access was denied.
4. **Input hotplug:** add and remove a USB keyboard (`device_add usb-kbd`). Typing works,
   since the keymap is compiled from `/usr/share/X11/xkb`.
5. **Cursor theme:** the cursor is drawn with the configured theme (`seat * xcursor_theme
   Adwaita 24`). Change the size and `reload`.
6. **Fonts:** title bars render text.
7. **exec:** a binding running `foot`, the swayidle → `scroll-lock` → locker path, and
   wayvnc via `exec_privileged` all work.
8. **Reload:** `reload` (binding) works with the config in `/etc/scroll`.
9. **Children:** `grep Seccomp /proc/$(pgrep -n foot)/status` shows `0`.
10. **Denials:** `journalctl -k | grep -i landlock` shows nothing unexpected. Landlock
    audit records need audit enabled.

## How upstream fixes arrive

- **wlroots:** commits titled `wlroots chase: <subject> (<upstream sha>)` are cherry-picks
  from wlroots. They are the only way upstream wlroots fixes reach scroll, so look at these
  first when bumping. 34 other commits change the wlroots copy directly: animations, scene,
  `linux_dmabuf`, vulkan. That's why a plain wlroots package can't replace the copy.
- **sway:** commits titled `sway chase: … (<sha>)` are cherry-picks from sway (52 so far).
- To check whether a wlroots advisory is covered, search for its upstream sha:
  `git log --grep=<sha>`.

## Build and test

```sh
meson setup build && ninja -C build              # -Werror is on; wrap_mode=nodownload
pytest tests/                                    # builds ./build itself with ASan; -n 4
```

Dependencies are sway's plus `lua >= 5.4`. In a Fedora container, `dnf builddep sway` plus
`lua-devel` covers them; scroll-secure's `scroll-build` stage is the reference.

## Fedora RPM (`packaging/`)

`packaging/scroll.spec` builds this tree for Fedora 44. Fedora's hardening flags come from
`%meson`, and wlroots is linked in statically; nothing from wlroots is installed.
`packaging/make-srpm.sh` makes the source RPM offline: the tarball is a `git archive` of
the commit, and the spec's `@...@` fields are filled in from it. It refuses a dirty tree.

| Profile | Switch | meson options |
|---|---|---|
| upstream feature set | (default) | Lua, IPC, scrollbar, Xwayland, all wlroots renderers and backends |
| scroll-secure | `--with hardened` | `-Dlua=disabled -Dipc=false -Dscrollbar=false -Dsandbox=true`, wlroots `xwayland=disabled renderers=gles2 backends=drm,libinput`. Release gets `.hardened` |

The individual switches (`lua`, `ipc`, `scrollbar`, `xwayland`, `sandbox`, `trim_wlroots`)
can be overridden with `--with`/`--without`.

`%check` fails the build unless `/usr/bin/scroll` passes `packaging/check-hardening.sh`:
PIE, RELRO, BIND_NOW, stack protector, FORTIFY, and CET IBT/SHSTK on x86_64. It also runs
`scroll -v` and `scroll -C` on the shipped config (headless backend). A build with Fedora's
flags overridden fails on 5 of the 7 checks.

```sh
srpm=$(packaging/make-srpm.sh -o /tmp/srpm)        # HEAD, or name a commit
dnf -y builddep "$srpm"
rpmbuild --rebuild --with hardened "$srpm"
dnf -y install ~/rpmbuild/RPMS/x86_64/scroll-*.hardened.*.rpm
```

`.github/workflows/rpm.yml` runs the same steps for both profiles: rpmlint (fails on
errors), checks of the hardened package's contents (no scrollmsg or scrollbar; no IPC
dispatch, Lua, Xwayland, Vulkan or X11 backend symbols; sandbox present, checked in the
debuginfo), and an install into a clean Fedora root. It uploads nothing. The scroll-secure
image builds its own RPM from a pinned checkout and never uses CI output.

## State when forked (2026-10-19)

- Tag `1.12.21` is `f80c5820` (2026-08-21). That's the commit scroll-secure pins.
- `master` is **76 commits ahead** of that tag. They include wlroots chases for an
  input-method-v2 NULL deref, xdg-activation token leaks, output-management client
  filtering, and 7 commits whose subjects mention a crash, UAF, leak or NULL. Bump the pin
  once a tag includes them.
