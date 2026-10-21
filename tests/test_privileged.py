import json
import os
import signal
import subprocess
import sys
import time
from pathlib import Path
from typing import Callable, Generator

import pytest
from test_utils import DEFAULT_CONFIG, ScrollInstance, run_compositor

TESTS = Path(__file__).parent.resolve()
WL_CLIENT = TESTS / "wl_client.py"
SCROLL_LOCK = TESTS.parent / "scroll-lock"

RESTRICTED = [
    "ext_data_control_manager_v1",
    "ext_foreign_toplevel_image_capture_source_manager_v1",
    "ext_foreign_toplevel_list_v1",
    "ext_image_copy_capture_manager_v1",
    "ext_output_image_capture_source_manager_v1",
    "ext_session_lock_manager_v1",
    "ext_transient_seat_manager_v1",
    "ext_workspace_manager_v1",
    "zwlr_data_control_manager_v1",
    "zwlr_export_dmabuf_manager_v1",
    "zwlr_foreign_toplevel_manager_v1",
    "zwlr_gamma_control_manager_v1",
    "zwlr_layer_shell_v1",
    "zwlr_output_manager_v1",
    "zwlr_output_power_manager_v1",
    "zwlr_screencopy_manager_v1",
    "zwlr_virtual_pointer_manager_v1",
    "zwp_input_method_manager_v2",
    "zwp_virtual_keyboard_manager_v1",
]
ALLOWED = [
    "wl_compositor",
    "wp_security_context_manager_v1",
    "zwp_keyboard_shortcuts_inhibit_manager_v1",
    "zxdg_output_manager_v1",
]


def wait_for(cond: Callable[[], bool], timeout: float = 10.0) -> bool:
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if cond():
            return True
        time.sleep(0.05)
    return False


def client_env(inst: ScrollInstance) -> dict:
    env = os.environ.copy()
    env.pop("WAYLAND_SOCKET", None)
    env["WAYLAND_DISPLAY"] = inst.getenv("WAYLAND_DISPLAY")
    return env


def plain_globals(inst: ScrollInstance, tmp_path: Path) -> list:
    out = tmp_path / "plain.json"
    subprocess.run(
        [sys.executable, "-I", str(WL_CLIENT), "globals", str(out)],
        env=client_env(inst), check=True, timeout=10,
    )
    return json.loads(out.read_text())


@pytest.fixture
def compositor(
    scroll_compositor_binary: str, tmp_path: Path
) -> Callable[[str], "ScrollInstance"]:
    stack: list = []

    def start(extra: str = "") -> ScrollInstance:
        cm = run_compositor(scroll_compositor_binary, tmp_path, DEFAULT_CONFIG + extra)
        inst = cm.__enter__()
        stack.append(cm)
        return inst

    yield start
    for cm in reversed(stack):
        cm.__exit__(None, None, None)


def test_plain_client_cannot_see_restricted(compositor, tmp_path: Path) -> None:
    inst = compositor()
    names = plain_globals(inst, tmp_path)
    for iface in ALLOWED:
        assert iface in names
    for iface in RESTRICTED:
        assert iface not in names, f"{iface} visible to an unprivileged client"


def test_exec_privileged_sees_restricted(compositor, tmp_path: Path) -> None:
    out = tmp_path / "priv.json"
    inst = compositor(f"exec_privileged {sys.executable} -I {WL_CLIENT} globals {out}\n")
    assert wait_for(out.exists), inst.read_log()
    names = json.loads(out.read_text())
    for iface in RESTRICTED + ALLOWED:
        assert iface in names, f"{iface} hidden from a privileged client"
    # Its privilege doesn't leak to other clients
    assert "zwlr_screencopy_manager_v1" not in plain_globals(inst, tmp_path)


def test_privileged_clients_all(compositor, tmp_path: Path) -> None:
    inst = compositor("privileged_clients all\n")
    names = plain_globals(inst, tmp_path)
    for iface in RESTRICTED + ALLOWED:
        assert iface in names


def test_exec_privileged_refused_over_ipc(compositor, tmp_path: Path) -> None:
    inst = compositor()
    marker = tmp_path / "ran"
    res = inst.cmd(f"exec_privileged touch {marker}")
    assert res[0]["success"] is False
    assert "IPC" in res[0]["error"]
    time.sleep(0.5)
    assert not marker.exists()
    # Config-only commands are refused at runtime
    assert inst.cmd("privileged_clients all")[0]["success"] is False
    assert inst.cmd("lock_command true")[0]["success"] is False


def test_lock_without_lock_command(compositor) -> None:
    inst = compositor()
    res = inst.cmd("lock")
    assert res[0]["success"] is False


class Locker:
    def __init__(self, inst: ScrollInstance, ctl: Path):
        self.inst = inst
        self.ctl = ctl
        self.state = Path(os.environ.get("XDG_RUNTIME_DIR", "/tmp")) / f"scroll-locked.{inst.proc.pid}"

    def started(self) -> list:
        return sorted(self.ctl.glob("started-*"))

    def pids(self) -> list:
        return [int(p.name.split("-")[1]) for p in self.started()]

    def kill_all(self) -> None:
        for pid in self.pids():
            try:
                os.kill(pid, signal.SIGKILL)
            except ProcessLookupError:
                pass


@pytest.fixture
def locker(compositor, tmp_path: Path) -> Generator[Locker, None, None]:
    ctl = tmp_path / "ctl"
    ctl.mkdir()
    inst = compositor(f"lock_command {sys.executable} -I {WL_CLIENT} lock {ctl}\n")
    lk = Locker(inst, ctl)
    assert not lk.state.exists()
    yield lk
    lk.kill_all()


def test_lock_command_and_state_file(locker: Locker) -> None:
    assert locker.inst.cmd("lock")[0]["success"] is True
    assert wait_for((locker.ctl / "locked").exists), locker.inst.read_log()
    assert locker.state.exists()
    # Locked by a live locker: lock does nothing
    assert locker.inst.cmd("lock")[0]["success"] is True
    time.sleep(0.5)
    assert len(locker.started()) == 1
    (locker.ctl / "unlock").touch()
    assert wait_for((locker.ctl / "unlocked").exists)
    assert wait_for(lambda: not locker.state.exists())
    assert locker.inst.proc.poll() is None


def test_sigusr1_and_abandoned_lock(locker: Locker) -> None:
    os.kill(locker.inst.proc.pid, signal.SIGUSR1)
    assert wait_for((locker.ctl / "locked").exists), locker.inst.read_log()
    (locker.ctl / "locked").unlink()
    locker.kill_all()
    assert wait_for(lambda: "session lock abandoned" in locker.inst.read_log())
    # An abandoned lock keeps the session locked
    assert locker.state.exists()
    os.kill(locker.inst.proc.pid, signal.SIGUSR1)
    assert wait_for(lambda: len(locker.started()) == 2)
    assert wait_for((locker.ctl / "locked").exists), locker.inst.read_log()
    assert "Replacing abandoned lock" in locker.inst.read_log()
    (locker.ctl / "unlock").touch()
    assert wait_for(lambda: not locker.state.exists())
    assert locker.inst.proc.poll() is None


def test_scroll_lock_script(locker: Locker) -> None:
    env = os.environ.copy()
    env["SCROLL_PID"] = str(locker.inst.proc.pid)
    res = subprocess.run([str(SCROLL_LOCK)], env=env, timeout=15)
    assert res.returncode == 0
    assert locker.state.exists()
    assert (locker.ctl / "locked").exists()
    # Already locked: returns at once
    start = time.monotonic()
    assert subprocess.run([str(SCROLL_LOCK)], env=env, timeout=15).returncode == 0
    assert time.monotonic() - start < 1
    assert len(locker.started()) == 1
    (locker.ctl / "unlock").touch()
    assert wait_for(lambda: not locker.state.exists())


def test_scroll_lock_without_session() -> None:
    env = os.environ.copy()
    env.pop("SCROLL_PID", None)
    res = subprocess.run([str(SCROLL_LOCK)], env=env, capture_output=True, text=True)
    assert res.returncode == 1
    assert "SCROLL_PID" in res.stderr
