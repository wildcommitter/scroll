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
| Privileged protocols | always | `privileged_clients all` (config only) restores sway | Only clients scroll starts over `WAYLAND_SOCKET` (swaybg, scrollnag, scrollbar, `exec_privileged`, `lock_command`) can bind screen capture, virtual input, data-control, input-method, session-lock, layer-shell, toplevel/workspace/output management, gamma and DRM leases. Only holds with `-Dipc=false` |
| Locking | always | `lock`, SIGUSR1, `scroll-lock` | `lock` starts `lock_command` as a privileged client. `$XDG_RUNTIME_DIR/scroll-locked.$SCROLL_PID` exists while locked; `scroll-lock` signals scroll and waits for it (for swayidle `before-sleep`) |
| IPC socket | `-Dipc=false` | `SCROLL_DISABLE_IPC=1` | No socket is created, `SWAYSOCK`/`I3SOCK`/`SCROLLSOCK` are unset, and `bar {}` isn't started. scrollmsg, scrollbar and waybar stop working |

The minimal build is `-Dlua=disabled -Dipc=false -Dscrollbar=false`. wayvnc still works,
because it uses Wayland protocols, not IPC. `tests/test_ipc_disabled.py` checks this with an
RFB handshake, with wayvnc started by `exec_privileged`; started outside scroll it gets no
virtual input. `tests/test_privileged.py` covers the allowlist and locking, using the
stdlib Wayland client `tests/wl_client.py`. Most of the pytest suite talks over IPC and Lua, so run it on a default build.
`liblua` can still show up in `ldd` through libinput; scroll itself doesn't link it.

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

## State when forked (2026-10-19)

- Tag `1.12.21` is `f80c5820` (2026-08-21). That's the commit scroll-secure pins.
- `master` is **76 commits ahead** of that tag. They include wlroots chases for an
  input-method-v2 NULL deref, xdg-activation token leaks, output-management client
  filtering, and 7 commits whose subjects mention a crash, UAF, leak or NULL. Bump the pin
  once a tag includes them.
