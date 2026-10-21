"""Minimal Wayland client (stdlib only) for the allowlist tests.

  wl_client.py globals OUT.json   # write the advertised interface names
  wl_client.py lock CTLDIR        # take the session lock, write CTLDIR/locked,
                                  # unlock once CTLDIR/unlock exists
  wl_client.py seccontext OUT.json ENGINE [CMD ...]
                                  # commit a security context with ENGINE
                                  # (app_id and instance_id "work") and write
                                  # {"result": "ok"|"error", ...}; on ok, also
                                  # the globals a client of it sees, then run
                                  # CMD as a client of it
  wl_client.py screenshot OUT     # screencopy the first output to OUT:
                                  # <u32 width, height, stride> + XRGB rows

Connects over WAYLAND_SOCKET (privileged spawn) or WAYLAND_DISPLAY.
"""
import json
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time


class ProtocolError(RuntimeError):
    pass


class Client:
    def __init__(self, display: str | None = None) -> None:
        fd = None if display else os.environ.pop("WAYLAND_SOCKET", None)
        if fd is not None:
            self.sock = socket.socket(fileno=int(fd))
        else:
            name = display or os.environ["WAYLAND_DISPLAY"]
            path = name if name.startswith("/") else os.path.join(
                os.environ["XDG_RUNTIME_DIR"], name)
            self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            self.sock.connect(path)
        self.buf = b""
        self.next_id = 2
        self.globals: dict[str, tuple[int, int]] = {}
        self.handlers: dict[int, object] = {}

    def new_id(self) -> int:
        i = self.next_id
        self.next_id += 1
        return i

    def send(self, obj: int, opcode: int, payload: bytes = b"", fds: tuple = ()) -> None:
        size = 8 + len(payload)
        data = struct.pack("<II", obj, (size << 16) | opcode) + payload
        if fds:
            anc = [(socket.SOL_SOCKET, socket.SCM_RIGHTS, struct.pack(f"{len(fds)}i", *fds))]
            self.sock.sendmsg([data], anc)
        else:
            self.sock.sendall(data)

    @staticmethod
    def string(s: str) -> bytes:
        b = s.encode() + b"\0"
        return struct.pack("<I", len(b)) + b + b"\0" * (-len(b) % 4)

    def read_event(self) -> tuple[int, int, bytes]:
        while len(self.buf) < 8 or len(self.buf) < (struct.unpack_from("<I", self.buf, 4)[0] >> 16):
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("compositor closed the connection")
            self.buf += chunk
        obj, word = struct.unpack_from("<II", self.buf)
        size = word >> 16
        msg, self.buf = self.buf[8:size], self.buf[size:]
        if obj == 1 and (word & 0xFFFF) == 0:  # wl_display.error
            _, code, length = struct.unpack_from("<III", msg)
            text = msg[12:12 + length - 1].decode(errors="replace")
            raise ProtocolError(f"protocol error {code}: {text}")
        return obj, word & 0xFFFF, msg

    def roundtrip(self) -> None:
        cb = self.new_id()
        self.send(1, 0, struct.pack("<I", cb))  # wl_display.sync
        while True:
            obj, opcode, msg = self.read_event()
            if obj == cb:
                return
            self.dispatch(obj, opcode, msg)

    def dispatch(self, obj: int, opcode: int, msg: bytes) -> None:
        if obj == self.registry and opcode == 0:  # wl_registry.global
            name, length = struct.unpack_from("<II", msg)
            iface = msg[8:8 + length - 1].decode()
            version = struct.unpack_from("<I", msg, 8 + length + (-length % 4))[0]
            self.globals[iface] = (name, version)
        elif obj in self.handlers:
            self.handlers[obj](opcode, msg)

    def get_registry(self) -> None:
        self.registry = self.new_id()
        self.send(1, 1, struct.pack("<I", self.registry))
        self.roundtrip()

    def bind(self, iface: str, version: int) -> int:
        name, _ = self.globals[iface]
        obj = self.new_id()
        self.send(self.registry, 0, struct.pack("<I", name) + self.string(iface)
                  + struct.pack("<II", version, obj))
        return obj


def cmd_globals(out: str) -> None:
    c = Client()
    c.get_registry()
    tmp = out + ".tmp"
    with open(tmp, "w") as f:
        json.dump(sorted(c.globals), f)
    os.rename(tmp, out)


def cmd_lock(ctl: str) -> None:
    open(os.path.join(ctl, f"started-{os.getpid()}"), "w").close()
    c = Client()
    c.get_registry()
    manager = c.bind("ext_session_lock_manager_v1", 1)
    lock = c.new_id()
    state = {"locked": False, "finished": False}

    def on_lock(opcode: int, msg: bytes) -> None:
        if opcode == 0:
            state["locked"] = True
        elif opcode == 1:
            state["finished"] = True

    c.handlers[lock] = on_lock
    c.send(manager, 1, struct.pack("<I", lock))  # ext_session_lock_manager_v1.lock
    c.roundtrip()
    if not state["locked"]:
        sys.exit("not locked")
    open(os.path.join(ctl, "locked"), "w").close()
    while not os.path.exists(os.path.join(ctl, "unlock")):
        time.sleep(0.05)
    c.send(lock, 2)  # ext_session_lock_v1.unlock_and_destroy
    c.roundtrip()
    open(os.path.join(ctl, "unlocked"), "w").close()


def write_json(out: str, data: object) -> None:
    tmp = out + ".tmp"
    with open(tmp, "w") as f:
        json.dump(data, f)
    os.rename(tmp, out)


def cmd_seccontext(out: str, engine: str, *cmd: str) -> None:
    c = Client()
    c.get_registry()
    manager = c.bind("wp_security_context_manager_v1", 1)
    path = os.path.join(tempfile.mkdtemp(prefix="wlsc"), "sock")  # short sun_path
    listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    listener.bind(path)
    listener.listen()
    close_r, close_w = os.pipe()  # the context lives until close_w is closed
    ctx = c.new_id()
    # wp_security_context_manager_v1.create_listener(new_id, fd, fd)
    c.send(manager, 1, struct.pack("<I", ctx), fds=(listener.fileno(), close_r))
    os.close(close_r)
    c.send(ctx, 1, c.string(engine))  # set_sandbox_engine
    c.send(ctx, 2, c.string("work"))  # set_app_id
    c.send(ctx, 3, c.string("work"))  # set_instance_id
    c.send(ctx, 4)  # commit
    try:
        c.roundtrip()
    except ProtocolError as e:
        write_json(out, {"result": "error", "error": str(e)})
        return
    child = Client(display=path)
    child.get_registry()
    write_json(out, {"result": "ok", "child_globals": sorted(child.globals)})
    if cmd:
        env = dict(os.environ, WAYLAND_DISPLAY=path)
        subprocess.run(cmd, env=env)
    os.close(close_w)


def cmd_screenshot(out: str) -> None:
    c = Client()
    c.get_registry()
    shm = c.bind("wl_shm", 1)
    output = c.bind("wl_output", 1)
    manager = c.bind("zwlr_screencopy_manager_v1", 3)
    frame = c.new_id()
    c.send(manager, 0, struct.pack("<IiI", frame, 0, output))  # capture_output
    st: dict = {}

    def on_frame(opcode: int, msg: bytes) -> None:
        if opcode == 0:  # buffer: format, width, height, stride
            st["buffer"] = struct.unpack_from("<IIII", msg)
        elif opcode in (2, 3, 6):  # ready, failed, buffer_done
            st[{2: "ready", 3: "failed", 6: "done"}[opcode]] = True

    c.handlers[frame] = on_frame
    while "done" not in st:
        c.roundtrip()
    fmt, width, height, stride = st["buffer"]
    size = stride * height
    fd = os.memfd_create("screenshot")
    os.ftruncate(fd, size)
    pool = c.new_id()
    c.send(shm, 0, struct.pack("<Ii", pool, size), fds=(fd,))  # create_pool
    buf = c.new_id()
    c.send(pool, 0, struct.pack("<IiiiiI", buf, 0, width, height, stride, fmt))
    c.send(frame, 0, struct.pack("<I", buf))  # copy
    while "ready" not in st and "failed" not in st:
        c.roundtrip()
    if "ready" not in st:
        sys.exit("screencopy failed")
    data = os.pread(fd, size, 0)
    tmp = out + ".tmp"
    with open(tmp, "wb") as f:
        f.write(struct.pack("<III", width, height, stride) + data)
    os.rename(tmp, out)


if __name__ == "__main__":
    {"globals": cmd_globals, "lock": cmd_lock, "seccontext": cmd_seccontext,
     "screenshot": cmd_screenshot}[sys.argv[1]](*sys.argv[2:])
