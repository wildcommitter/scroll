"""Dragging a tiled window by its own title bar (client-side decorations), as in
niri: the client asks for xdg_toplevel.move on a button press, and scroll starts the
same tiling drag as mod + drag, past tiling_drag_threshold."""
import subprocess
import sys
import time
from pathlib import Path

from test_privileged import WL_CLIENT, client_env, compositor  # noqa: F401
from test_utils import ScrollInstance, find_node_by_title_contains, wait_for_client_map

TEST_CLIENT = Path("./build/tests/wayland-test-client").resolve()
CONFIG = "privileged_clients all\nanimations enabled no\nlayout_default_width 0.3\n"
TITLES = ("csd-a", "csd-b", "csd-c")


def rect(inst: ScrollInstance, title: str) -> dict:
    node = find_node_by_title_contains(inst.get_tree(), title)
    assert node is not None, title
    return node["rect"]


def order(inst: ScrollInstance) -> list:
    return sorted(TITLES, key=lambda t: rect(inst, t)["x"])


def title_bar(r: dict) -> tuple:
    """A point in the client's 100x100 buffer at the window's top left: its "title bar"."""
    return r["x"] + 20, r["y"] + 10


class Pointer:
    """The virtual pointer (wl_client.py vpointer): real motion and button events, so
    a drag runs the same seatop code as a mouse (IPC "seat cursor" only warps)."""

    def __init__(self, inst: ScrollInstance) -> None:
        root = inst.get_tree()["rect"]
        self.proc = subprocess.Popen(
            [sys.executable, "-I", str(WL_CLIENT), "vpointer", str(root["width"]), str(root["height"])],
            env=client_env(inst), stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        assert self.proc.stdout.readline().strip() == "ok", "virtual pointer"

    def __call__(self, *cmd: object) -> None:
        self.proc.stdin.write(" ".join(map(str, cmd)) + "\n")
        self.proc.stdin.flush()
        assert self.proc.stdout.readline().strip() == "ok", cmd
        time.sleep(0.05)  # the client answers a press with xdg_toplevel.move


def setup(compositor) -> tuple:
    inst = compositor(CONFIG)
    pointer = Pointer(inst)
    env = client_env(inst)
    clients = []
    for t in TITLES:
        clients.append(subprocess.Popen([str(TEST_CLIENT), t, "test_app", "csd"], env=env))
        wait_for_client_map(inst, t)
    inst.wait_for_transactions()
    return inst, pointer, [pointer.proc, *clients]


def stop(procs: list) -> None:
    for p in procs:
        if p.stdin:
            p.stdin.close()
        p.terminate()
        p.wait(timeout=5)


def test_csd_drag_moves_tiled_window(compositor) -> None:
    inst, pointer, procs = setup(compositor)
    try:
        assert order(inst) == ["csd-a", "csd-b", "csd-c"]
        a, c = rect(inst, "csd-a"), rect(inst, "csd-c")
        pointer("abs", *title_bar(a))
        pointer("down")
        # past the threshold, then onto the drop zone at the right edge of csd-c
        for _ in range(4):
            pointer("rel", 10, 0)
        pointer("abs", c["x"] + c["width"] - 3, c["y"] + c["height"] // 2)
        pointer("up")
        inst.wait_for_transactions()
        assert order(inst) == ["csd-b", "csd-c", "csd-a"]
    finally:
        stop(procs)


def test_csd_click_stays_a_click(compositor) -> None:
    inst, pointer, procs = setup(compositor)
    try:
        pointer("abs", *title_bar(rect(inst, "csd-b")))
        pointer("down")
        pointer("up")
        inst.wait_for_transactions()
        assert order(inst) == ["csd-a", "csd-b", "csd-c"]
    finally:
        stop(procs)
