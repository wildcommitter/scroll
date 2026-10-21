import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path
from typing import Generator

import pytest

CONFIG = "output HEADLESS-1 resolution 640x480\n"


def _wait(cond, timeout: float = 10.0) -> bool:
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if cond():
            return True
        time.sleep(0.05)
    return False


VNC_PORT = 5917
VNC_LOG = Path("/tmp/scroll-test-wayvnc.log")


def _wayvnc_cmd(log: Path) -> str:
    return f"wayvnc --config=/dev/null 127.0.0.1 {VNC_PORT} >{log} 2>&1"


@pytest.fixture
def no_ipc_compositor(
    scroll_compositor_binary: str, tmp_path: Path, request: pytest.FixtureRequest
) -> Generator[tuple[subprocess.Popen, Path, dict], None, None]:
    run = tmp_path / "run"
    run.mkdir(mode=0o700)
    env = os.environ.copy()
    for var in ("WAYLAND_DISPLAY", "DISPLAY", "SWAYSOCK", "I3SOCK", "SCROLLSOCK"):
        env.pop(var, None)
    env.update(
        HOME=str(tmp_path),
        XDG_RUNTIME_DIR=str(run),
        WLR_BACKENDS="headless",
        WLR_RENDERER="pixman",
        SCROLL_DISABLE_IPC="1",
        LSAN_OPTIONS="detect_leaks=0",
    )
    config = tmp_path / "config"
    config.write_text(CONFIG + getattr(request, "param", ""))
    log_path = tmp_path / "scroll.log"
    with open(log_path, "w") as log:
        proc = subprocess.Popen(
            [scroll_compositor_binary, "-c", str(config), "-d"],
            env=env,
            stdout=log,
            stderr=subprocess.STDOUT,
        )
    try:
        assert _wait(lambda: (run / "wayland-1").exists() or (run / "wayland-0").exists())
        env["WAYLAND_DISPLAY"] = "wayland-1" if (run / "wayland-1").exists() else "wayland-0"
        assert _wait(lambda: "IPC disabled" in log_path.read_text())
        yield proc, run, env
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()


def test_no_ipc_socket(no_ipc_compositor: tuple[subprocess.Popen, Path, dict]) -> None:
    proc, run, _ = no_ipc_compositor
    assert not list(run.glob("scroll-ipc.*")), "IPC socket created while disabled"
    assert proc.poll() is None


def _recv(sock: socket.socket, n: int) -> bytes:
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        assert chunk, "VNC server closed the connection"
        buf += chunk
    return buf


@pytest.mark.skipif(shutil.which("wayvnc") is None, reason="wayvnc not installed")
def test_unprivileged_wayvnc_refused(
    no_ipc_compositor: tuple[subprocess.Popen, Path, dict], tmp_path: Path
) -> None:
    # Started outside scroll, wayvnc gets no virtual input or screen capture
    _, _, env = no_ipc_compositor
    res = subprocess.run(
        ["wayvnc", "--config=/dev/null", "127.0.0.1", str(VNC_PORT)],
        env=env, capture_output=True, text=True, timeout=10,
    )
    assert res.returncode != 0
    assert "not supported by compositor" in res.stdout + res.stderr


@pytest.mark.skipif(shutil.which("wayvnc") is None, reason="wayvnc not installed")
@pytest.mark.parametrize(
    "no_ipc_compositor", [f"exec_privileged {_wayvnc_cmd(VNC_LOG)}\n"],
    indirect=True,
)
def test_wayvnc_without_ipc(
    no_ipc_compositor: tuple[subprocess.Popen, Path, dict], tmp_path: Path
) -> None:
    proc, _, env = no_ipc_compositor
    port = VNC_PORT
    vnc_log = VNC_LOG
    try:
        sock = None

        def connect() -> bool:
            nonlocal sock
            try:
                sock = socket.create_connection(("127.0.0.1", port), timeout=5)
                return True
            except OSError:
                return False

        assert _wait(connect), f"wayvnc did not listen:\n{vnc_log.read_text()}"
        assert sock is not None
        with sock:
            # RFB 3.8 handshake with security type None
            assert _recv(sock, 12) == b"RFB 003.008\n"
            sock.sendall(b"RFB 003.008\n")
            (count,) = struct.unpack("B", _recv(sock, 1))
            assert 1 in _recv(sock, count), "security type None not offered"
            sock.sendall(b"\x01")
            assert _recv(sock, 4) == b"\0\0\0\0"
            sock.sendall(b"\x01")  # ClientInit, shared
            width, height = struct.unpack(">HH", _recv(sock, 4))
            _recv(sock, 16)
            (name_len,) = struct.unpack(">I", _recv(sock, 4))
            _recv(sock, name_len)
            assert (width, height) == (640, 480)
            # Input goes through the virtual keyboard and pointer protocols
            sock.sendall(struct.pack(">BBxxI", 4, 1, 0x61) + struct.pack(">BBxxI", 4, 0, 0x61))
            sock.sendall(struct.pack(">BBHH", 5, 0, 10, 10))
            # A full FramebufferUpdateRequest needs a screen capture
            sock.sendall(struct.pack(">BBHHHH", 3, 0, 0, 0, width, height))
            assert _recv(sock, 1) == b"\x00", "no FramebufferUpdate"
        assert proc.poll() is None
    finally:
        for comm in Path("/proc").glob("[0-9]*/comm"):
            try:
                if comm.read_text().strip() == "wayvnc":
                    os.kill(int(comm.parent.name), 15)
            except (OSError, ValueError):
                pass


def _status(path: str, field: str) -> str:
    with open(path) as f:
        for line in f:
            if line.startswith(field + ":"):
                return line.split()[1]
    raise AssertionError(f"{field} missing from {path}")


def test_no_ipc_sandbox(
    no_ipc_compositor: tuple[subprocess.Popen, Path, dict], tmp_path: Path
) -> None:
    proc, _, _ = no_ipc_compositor
    log = tmp_path / "scroll.log"
    # Applied right before the event loop, after the fixture saw "IPC disabled"
    if not _wait(lambda: "sandbox: seccomp filter applied" in log.read_text(), 3):
        pytest.skip("not a -Dsandbox=true build")
    # Containers stack their own filter: compare with this process
    mine = int(_status("/proc/self/status", "Seccomp_filters"))
    assert int(_status(f"/proc/{proc.pid}/status", "Seccomp_filters")) == mine + 1
    assert _status(f"/proc/{proc.pid}/status", "NoNewPrivs") == "1"
    assert proc.poll() is None


WL_CLIENT = Path(__file__).parent.resolve() / "wl_client.py"
VM_OUT = Path(f"/tmp/scroll-test-vm-context.{os.getpid()}.json")  # per xdist worker


def _seccontext(env: dict, out: Path, engine: str) -> dict:
    subprocess.run([sys.executable, "-I", str(WL_CLIENT), "seccontext", str(out), engine],
                   env=env, check=True, timeout=10)
    return json.loads(out.read_text())


def test_unprivileged_vm_context_refused(
    no_ipc_compositor: tuple[subprocess.Popen, Path, dict], tmp_path: Path
) -> None:
    # The shipped build: a host app can't claim the "scroll-vm" engine
    proc, _, env = no_ipc_compositor
    res = _seccontext(env, tmp_path / "vm.json", "scroll-vm")
    assert res["result"] == "error" and "protocol error 3" in res["error"], res
    assert _seccontext(env, tmp_path / "fp.json", "flatpak")["result"] == "ok"
    assert proc.poll() is None



@pytest.mark.parametrize(
    "no_ipc_compositor",
    [f"exec_privileged {sys.executable} -I {WL_CLIENT} seccontext {VM_OUT} scroll-vm\n"],
    indirect=True, ids=["exec_privileged"],  # VM_OUT differs per xdist worker
)
def test_privileged_vm_context(
    no_ipc_compositor: tuple[subprocess.Popen, Path, dict]
) -> None:
    try:
        assert _wait(VM_OUT.exists)
        res = json.loads(VM_OUT.read_text())
        assert res["result"] == "ok", res
        assert "wl_compositor" in res["child_globals"]
    finally:
        VM_OUT.unlink(missing_ok=True)
