import os
import shutil
import socket
import struct
import subprocess
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


@pytest.fixture
def no_ipc_compositor(
    scroll_compositor_binary: str, tmp_path: Path
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
    config.write_text(CONFIG)
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
def test_wayvnc_without_ipc(
    no_ipc_compositor: tuple[subprocess.Popen, Path, dict], tmp_path: Path
) -> None:
    proc, _, env = no_ipc_compositor
    port = 5917
    vnc_log = tmp_path / "wayvnc.log"
    with open(vnc_log, "w") as log:
        vnc = subprocess.Popen(
            ["wayvnc", "--config=/dev/null", "127.0.0.1", str(port)],
            env=env,
            stdout=log,
            stderr=subprocess.STDOUT,
        )
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
        assert vnc.poll() is None, vnc_log.read_text()
        assert "not supported" not in vnc_log.read_text().lower(), vnc_log.read_text()
        assert proc.poll() is None
    finally:
        vnc.terminate()
        vnc.wait(timeout=5)
