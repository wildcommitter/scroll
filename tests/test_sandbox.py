"""The -Dsandbox=true build: seccomp and Landlock on the compositor, none on
its children (they come from the spawn broker)."""
import re
import time
from pathlib import Path
from typing import Callable

import pytest
from test_utils import DEFAULT_CONFIG, ScrollInstance, run_compositor


def status_field(path: Path, field: str) -> str:
    m = re.search(rf"^{field}:\s*(\S+)", path.read_text(), re.M)
    assert m, f"{field} missing from {path}"
    return m.group(1)


def wait_for(cond: Callable[[], bool], timeout: float = 10.0) -> bool:
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if cond():
            return True
        time.sleep(0.05)
    return False


SELF = Path("/proc/self/status")


def filters(status: Path) -> int:
    # Containers stack their own filter, so compare against this process
    return int(status_field(status, "Seccomp_filters"))


def sandboxed(inst: ScrollInstance) -> bool:
    # Applied right before the event loop; the IPC socket exists earlier
    if not wait_for(lambda: "sandbox: seccomp filter applied" in inst.read_log(), 3):
        return False
    return filters(Path(f"/proc/{inst.proc.pid}/status")) > filters(SELF)


def require_sandbox(inst: ScrollInstance) -> None:
    if not sandboxed(inst):
        pytest.skip("not a -Dsandbox=true build")


def require_landlock(inst: ScrollInstance) -> None:
    require_sandbox(inst)
    if "Landlock unavailable" in inst.read_log():
        pytest.skip("kernel or container has no Landlock")


def test_compositor_confined(scroll_compositor: ScrollInstance) -> None:
    require_sandbox(scroll_compositor)
    status = Path(f"/proc/{scroll_compositor.proc.pid}/status")
    assert status_field(status, "Seccomp") == "2"
    assert filters(status) == filters(SELF) + 1
    assert status_field(status, "NoNewPrivs") == "1"


def test_children_unconfined(scroll_compositor: ScrollInstance, tmp_path: Path) -> None:
    out = tmp_path / "child-status"
    res = scroll_compositor.cmd(f"exec cat /proc/self/status > {out}")
    assert res[0]["success"] is True
    assert wait_for(lambda: out.exists() and "Seccomp" in out.read_text())
    assert filters(out) == filters(SELF)
    assert status_field(out, "NoNewPrivs") == status_field(SELF, "NoNewPrivs")


def test_privileged_child_unconfined(scroll_compositor_binary: str, tmp_path: Path) -> None:
    out = tmp_path / "priv-status"
    config = DEFAULT_CONFIG + f"exec_privileged cat /proc/self/status > {out}\n"
    with run_compositor(scroll_compositor_binary, tmp_path, config):
        assert wait_for(lambda: out.exists() and "Seccomp" in out.read_text())
        assert filters(out) == filters(SELF)


def test_compositor_cannot_exec(scroll_compositor: ScrollInstance) -> None:
    require_sandbox(scroll_compositor)
    # os.execute runs sh in the compositor itself, not through the broker
    marker = scroll_compositor.temp_dir / "lua-exec"
    res = scroll_compositor.execute_lua(f"return os.execute('touch {marker}')")
    # os.execute runs sh in the compositor: execve gets EPERM
    assert not marker.exists()
    assert res is None or res is False or (isinstance(res, list) and res[0] in (None, False))
    assert scroll_compositor.proc.poll() is None


def test_home_unreadable(scroll_compositor_binary: str, tmp_path: Path) -> None:
    home = tmp_path / "home"
    (home / ".ssh").mkdir(parents=True)
    secret = home / ".ssh" / "id_test"
    secret.write_text("secret\n")
    (tmp_path / "cfg").mkdir()
    with run_compositor(scroll_compositor_binary, tmp_path / "cfg", home=home) as inst:
        require_landlock(inst)
        ok, err = inst.execute_lua(
            f'local f, e = io.open("{secret}") if f then f:close() end return f ~= nil, e')
        assert ok is False and "Permission denied" in err
        # System data stays readable
        assert inst.execute_lua(
            'local f = io.open("/etc/os-release") if f then f:close() end return f ~= nil') is True
        assert inst.proc.poll() is None


def test_lock_and_exec_privileged_work(scroll_compositor_binary: str, tmp_path: Path) -> None:
    marker = tmp_path / "ran"
    config = DEFAULT_CONFIG + f"lock_command touch {marker}\n"
    with run_compositor(scroll_compositor_binary, tmp_path, config) as inst:
        assert inst.cmd("lock")[0]["success"] is True
        assert wait_for(marker.exists)
        assert inst.proc.poll() is None
