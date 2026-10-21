"""VM windows: only privileged clients can create "scroll-vm" security
contexts, and border_color gives a window a frame color that no other state
replaces."""
import json
import subprocess
import sys
from pathlib import Path

from test_privileged import WL_CLIENT, client_env, compositor, wait_for  # noqa: F401
from test_utils import ScrollInstance, find_node_by_title_contains, wayland_client, wait_for_client_map

TEST_CLIENT = Path("./build/tests/wayland-test-client").resolve()


def plain_seccontext(inst: ScrollInstance, tmp_path: Path, engine: str) -> dict:
    out = tmp_path / f"sc-{engine}.json"
    subprocess.run(
        [sys.executable, "-I", str(WL_CLIENT), "seccontext", str(out), engine],
        env=client_env(inst), check=True, timeout=10,
    )
    return json.loads(out.read_text())


def test_unprivileged_cannot_claim_vm_engine(compositor, tmp_path: Path) -> None:
    inst = compositor()
    res = plain_seccontext(inst, tmp_path, "scroll-vm")
    assert res["result"] == "error", res
    assert "protocol error 3" in res["error"]  # invalid_metadata
    assert "Refused security context" in inst.read_log()
    assert inst.proc.poll() is None


def test_unprivileged_cannot_claim_vm_substring(compositor, tmp_path: Path) -> None:
    # Criteria are unanchored regexes, so "x-scroll-vm" would match a
    # sandbox_engine="scroll-vm" rule
    inst = compositor()
    assert plain_seccontext(inst, tmp_path, "x-scroll-vm")["result"] == "error"


def test_unprivileged_other_engine_works(compositor, tmp_path: Path) -> None:
    inst = compositor()
    res = plain_seccontext(inst, tmp_path, "flatpak")
    assert res["result"] == "ok", res
    globs = res["child_globals"]
    assert "wl_compositor" in globs
    # A sandboxed client can't nest: the manager is hidden from it (and
    # wlroots refuses a nested commit anyway)
    assert "wp_security_context_manager_v1" not in globs


def test_vm_engine_strict_with_privileged_clients_all(compositor, tmp_path: Path) -> None:
    inst = compositor("privileged_clients all\n")
    assert plain_seccontext(inst, tmp_path, "scroll-vm")["result"] == "error"


def test_privileged_creates_vm_context(compositor, tmp_path: Path) -> None:
    out = tmp_path / "vm.json"
    rule = ('for_window [sandbox_engine="scroll-vm" sandbox_app_id="work"] '
            'border_color #cc3333, border normal 3\n')
    inst = compositor(
        rule + f"exec_privileged {sys.executable} -I {WL_CLIENT} seccontext {out} "
        f"scroll-vm {TEST_CLIENT} vmapp vmapp\n")
    assert wait_for(out.exists), inst.read_log()
    res = json.loads(out.read_text())
    assert res["result"] == "ok", res
    assert "wp_security_context_manager_v1" not in res["child_globals"]
    assert "zwlr_screencopy_manager_v1" not in res["child_globals"]
    node = None

    def mapped() -> bool:
        nonlocal node
        node = find_node_by_title_contains(inst.get_tree(), "vmapp")
        return node is not None

    assert wait_for(mapped), inst.read_log()
    assert node["sandbox_engine"] == "scroll-vm"
    assert node["sandbox_app_id"] == "work"
    assert node["border_color"] == "#cc3333ff"
    assert node["border"] == "normal" and node["current_border_width"] == 3


def test_border_color_command(scroll_compositor: ScrollInstance) -> None:
    inst = scroll_compositor
    with wayland_client(inst, "bc1"):
        wait_for_client_map(inst, "bc1")
        node = find_node_by_title_contains(inst.get_tree(), "bc1")
        assert "border_color" not in node
        res = inst.cmd('[title="^bc1$"] border_color #ff0000')
        assert res[0]["success"] is True, res
        node = find_node_by_title_contains(inst.get_tree(), "bc1")
        assert node["border_color"] == "#ff0000ff"
        # Other states don't clear it
        assert inst.cmd('[title="^bc1$"] urgent enable')[0]["success"] is True
        assert inst.cmd('[title="^bc1$"] sticky enable')[0]["success"] is True
        assert inst.cmd("border_color #11223344")[0]["success"] is True
        node = find_node_by_title_contains(inst.get_tree(), "bc1")
        assert node["border_color"] == "#11223344"
        for bad in ("#ff00", "red", "#gg0000"):
            res = inst.cmd(f"border_color {bad}")
            assert res[0]["success"] is False, bad
        assert inst.cmd('[title="^bc1$"] border_color none')[0]["success"] is True
        node = find_node_by_title_contains(inst.get_tree(), "bc1")
        assert "border_color" not in node


def test_border_color_for_window(scroll_compositor_factory, tmp_path: Path) -> None:
    with scroll_compositor_factory(
            'workspace 1\nanimations enabled no\n'
            'for_window [app_id="test_app"] border_color #00ff00\n') as inst:
        with wayland_client(inst, "bc2"):
            wait_for_client_map(inst, "bc2")
            node = find_node_by_title_contains(inst.get_tree(), "bc2")
            assert node["border_color"] == "#00ff00ff"
