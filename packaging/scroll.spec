# scroll (wildcommitter fork) for Fedora. Build it with packaging/make-srpm.sh,
# which fills in the @...@ fields from the git checkout and makes the tarball
# with git archive, so the build never downloads anything.
#
#   rpmbuild --rebuild scroll-*.src.rpm                    # upstream feature set
#   rpmbuild --rebuild --with hardened scroll-*.src.rpm    # scroll-secure profile
#
# wlroots is vendored (subprojects/wlroots, modified in-tree) and linked
# statically; nothing of it is installed.

%global commit      @COMMIT@
%global shortcommit %(c=%{commit}; echo ${c:0:8})
%global snapdate    @SNAPDATE@

# One switch for the scroll-secure image. It sets the defaults of the
# individual switches below; --with/--without still override each of them.
%bcond hardened 0

%if %{with hardened}
# Exec broker + seccomp/Landlock (needs xwayland off: wlroots execs Xwayland)
%bcond sandbox      1
%bcond lua          0
%bcond ipc          0
%bcond scrollbar    0
%bcond xwayland     0
%bcond trim_wlroots 1
%else
%bcond sandbox      0
%bcond lua          1
%bcond ipc          1
%bcond scrollbar    1
%bcond xwayland     1
%bcond trim_wlroots 0
%endif

Name:           scroll
Version:        @VERSION@
Release:        0.%{snapdate}git%{shortcommit}%{?with_hardened:.hardened}%{?dist}
Summary:        Wayland compositor with a scrolling layout, compatible with i3

# scroll and the vendored wlroots are MIT
License:        MIT
URL:            https://github.com/wildcommitter/scroll
Source0:        %{name}-%{shortcommit}.tar.gz

BuildRequires:  gcc
BuildRequires:  meson >= 1.3
BuildRequires:  binutils
BuildRequires:  pkgconfig(cairo)
BuildRequires:  pkgconfig(egl)
BuildRequires:  pkgconfig(gbm) >= 21.1
BuildRequires:  pkgconfig(glesv2)
BuildRequires:  pkgconfig(hwdata)
BuildRequires:  pkgconfig(json-c) >= 0.13
BuildRequires:  pkgconfig(lcms2)
BuildRequires:  pkgconfig(libdisplay-info)
BuildRequires:  pkgconfig(libdrm)
BuildRequires:  pkgconfig(libevdev)
BuildRequires:  pkgconfig(libinput) >= 1.26.0
BuildRequires:  pkgconfig(libliftoff)
BuildRequires:  pkgconfig(libpcre2-8)
BuildRequires:  pkgconfig(libseat)
BuildRequires:  pkgconfig(libsystemd) >= 239
BuildRequires:  pkgconfig(libudev)
BuildRequires:  pkgconfig(pango)
BuildRequires:  pkgconfig(pangocairo)
BuildRequires:  pkgconfig(pixman-1) >= 0.46.0
BuildRequires:  pkgconfig(scdoc)
BuildRequires:  pkgconfig(wayland-client)
BuildRequires:  pkgconfig(wayland-cursor)
BuildRequires:  pkgconfig(wayland-protocols) >= 1.47
BuildRequires:  pkgconfig(wayland-scanner)
BuildRequires:  pkgconfig(wayland-server) >= 1.24.0
BuildRequires:  pkgconfig(xkbcommon) >= 1.8.0
BuildRequires:  bash-completion
%if %{with lua}
BuildRequires:  pkgconfig(lua) >= 5.4
%endif
%if %{with ipc}
# scrollmsg
BuildRequires:  readline-devel
%endif
%if %{with scrollbar}
BuildRequires:  pkgconfig(gdk-pixbuf-2.0)
%endif
%if %{without trim_wlroots}
# Vulkan renderer and the X11 backend
BuildRequires:  glslang
BuildRequires:  pkgconfig(vulkan) >= 1.2.182
BuildRequires:  pkgconfig(x11-xcb)
BuildRequires:  pkgconfig(xcb-dri3)
BuildRequires:  pkgconfig(xcb-present)
BuildRequires:  pkgconfig(xcb-renderutil)
BuildRequires:  pkgconfig(xcb-shm)
BuildRequires:  pkgconfig(xcb-xinput)
%endif
%if %{with xwayland} || %{without trim_wlroots}
BuildRequires:  pkgconfig(xcb)
BuildRequires:  pkgconfig(xcb-errors)
BuildRequires:  pkgconfig(xcb-render)
BuildRequires:  pkgconfig(xcb-xfixes) >= 1.15
%endif
%if %{with xwayland}
BuildRequires:  pkgconfig(xcb-composite)
BuildRequires:  pkgconfig(xcb-ewmh)
BuildRequires:  pkgconfig(xcb-icccm)
BuildRequires:  pkgconfig(xcb-res)
BuildRequires:  pkgconfig(xwayland)
%endif

Recommends:     mesa-dri-drivers
Recommends:     swaybg
Recommends:     swayidle
%if %{with xwayland}
Recommends:     xorg-x11-server-Xwayland
%endif

%description
scroll is a fork of sway, the i3-compatible Wayland compositor, with a
scrolling tiling layout like PaperWM, animations and (optionally) a Lua API.
This is a hardened fork: privileged Wayland protocols are restricted to
clients scroll starts itself, and the Lua API and the IPC socket can be
compiled out.

%prep
%autosetup -n %{name}-%{shortcommit}

%build
%meson \
    -Dwerror=false \
    -Db_pie=true \
    -Dsd-bus-provider=libsystemd \
    -Dvcs-commit=%{shortcommit} \
    -Dlua=%{?with_lua:enabled}%{!?with_lua:disabled} \
    -Dipc=%{?with_ipc:true}%{!?with_ipc:false} \
    -Dscrollbar=%{?with_scrollbar:true}%{!?with_scrollbar:false} \
    -Dtray=%{?with_scrollbar:enabled}%{!?with_scrollbar:disabled} \
    -Dgdk-pixbuf=%{?with_scrollbar:enabled}%{!?with_scrollbar:disabled} \
    -Ddefault-wallpaper=%{?with_hardened:false}%{!?with_hardened:true} \
    -Dsandbox=%{?with_sandbox:true}%{!?with_sandbox:false} \
    -Dwlroots:default_library=static \
    -Dwlroots:examples=false \
    -Dwlroots:xwayland=%{?with_xwayland:enabled}%{!?with_xwayland:disabled} \
%if %{with trim_wlroots}
    -Dwlroots:renderers=gles2 \
    -Dwlroots:backends=drm,libinput \
    -Dwlroots:xcb-errors=disabled \
%endif
    %{nil}
%meson_build

%install
# --skip-subprojects: the static wlroots stays inside the binary
DESTDIR=%{buildroot} %{__meson} install -C %{_vpath_builddir} --no-rebuild --skip-subprojects
install -d -m 0755 %{buildroot}%{_sysconfdir}/scroll/config.d

%check
# Refuse to ship a compositor without Fedora's hardening flags
bash packaging/check-hardening.sh %{buildroot}%{_bindir}/scroll
%{buildroot}%{_bindir}/scroll -v
# -C still brings up a backend: use the headless one. No sanitizers here; with
# the sandbox, LeakSanitizer couldn't ptrace the compositor anyway.
export XDG_RUNTIME_DIR=$(mktemp -d) WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1
%{buildroot}%{_bindir}/scroll -C -c %{buildroot}%{_sysconfdir}/scroll/config

%files
%license LICENSE
%doc README.md TUTORIAL.md FORK.md
%{_bindir}/scroll
%{_bindir}/scroll-lock
%{_bindir}/scrollnag
%if %{with ipc}
%{_bindir}/scrollmsg
%endif
%if %{with scrollbar}
%{_bindir}/scrollbar
%endif
%dir %{_sysconfdir}/scroll
%dir %{_sysconfdir}/scroll/config.d
%config(noreplace) %{_sysconfdir}/scroll/config
%{_datadir}/wayland-sessions/scroll.desktop
%{_datadir}/icons/scroll.png
%{_mandir}/man1/scroll*.1*
%{_mandir}/man5/scroll*.5*
%{_mandir}/man7/scroll*.7*
%{bash_completions_dir}/scroll*
%{fish_completions_dir}/scroll*.fish
%{zsh_completions_dir}/_scroll*
%if %{without hardened}
%{_datadir}/backgrounds/scroll
%endif

%changelog
* @CHANGELOGDATE@ wildcommitter <40472473+wildcommitter@users.noreply.github.com> - @VERSION@-0.@SNAPDATE@git@SHORTCOMMIT@
- Snapshot of the fork at commit @COMMIT@
