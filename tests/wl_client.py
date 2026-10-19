"""Minimal Wayland client (stdlib only) for the allowlist tests.

  wl_client.py globals OUT.json   # write the advertised interface names
  wl_client.py lock CTLDIR        # take the session lock, write CTLDIR/locked,
                                  # unlock once CTLDIR/unlock exists

Connects over WAYLAND_SOCKET (privileged spawn) or WAYLAND_DISPLAY.
"""
import json
import os
import socket
import struct
import sys
import time


class Client:
    def __init__(self) -> None:
        fd = os.environ.pop("WAYLAND_SOCKET", None)
        if fd is not None:
            self.sock = socket.socket(fileno=int(fd))
        else:
            name = os.environ["WAYLAND_DISPLAY"]
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

    def send(self, obj: int, opcode: int, payload: bytes = b"") -> None:
        size = 8 + len(payload)
        self.sock.sendall(struct.pack("<II", obj, (size << 16) | opcode) + payload)

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
        if obj == 1 and (word & 0xFFFF) == 0:
            raise RuntimeError(f"protocol error: {msg!r}")
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


if __name__ == "__main__":
    {"globals": cmd_globals, "lock": cmd_lock}[sys.argv[1]](sys.argv[2])
