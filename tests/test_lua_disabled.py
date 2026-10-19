from pathlib import Path
import pytest
from test_utils import run_compositor


@pytest.mark.parametrize("value", ["1", "yes"])
def test_env_disables_lua(
    scroll_compositor_binary: str,
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    value: str,
) -> None:
    monkeypatch.setenv("SCROLL_DISABLE_LUA", value)
    script = tmp_path / "script.lua"
    marker = tmp_path / "ran"
    script.write_text(f'io.open("{marker}", "w"):close()\n')

    with run_compositor(scroll_compositor_binary, tmp_path) as inst:
        res = inst.ipc.lua_eval(f'io.open("{marker}", "w"):close()')
        assert res["success"] is False
        assert "Lua is disabled" in res["error"]

        for cmd in (f"lua {script}", f'lua_eval t io.open("{marker}","w"):close()'):
            out = inst.cmd(cmd)
            assert out[0]["success"] is False
            assert "Lua is disabled" in out[0]["error"]

        assert not marker.exists()
        assert inst.cmd("workspace 2")[0]["success"] is True
        assert inst.proc.poll() is None


@pytest.mark.parametrize("value", ["", "0"])
def test_env_keeps_lua(
    scroll_compositor_binary: str,
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    value: str,
) -> None:
    monkeypatch.setenv("SCROLL_DISABLE_LUA", value)
    with run_compositor(scroll_compositor_binary, tmp_path) as inst:
        assert inst.execute_lua("return 1 + 1") == 2
