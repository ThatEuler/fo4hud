import asyncio
import base64
import io
import json
import os
from pathlib import Path
import subprocess
import sys

import pytest
from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "mcp_server"))
from fo4hud_server import Bridge, BridgeError


@pytest.fixture
def game(tmp_path):
    env = {**os.environ, "XDG_RUNTIME_DIR": str(tmp_path), "XDG_STATE_HOME": str(tmp_path),
           "FO4HUD_SAVE_DIR": str(tmp_path / "saves")}
    process = subprocess.Popen(["/tmp/fo4hud-control-bridge-fixture"], stdin=subprocess.PIPE,
                               stdout=subprocess.PIPE, text=True, env=env)
    line = process.stdout.readline()
    assert line, "native bridge fixture failed to start"
    metadata = json.loads(line)
    yield process, metadata
    process.stdin.write("quit\n")
    process.stdin.flush()
    assert process.wait(timeout=5) == 0
    assert not Path(metadata["socket"]).exists()


def pause(process, enabled):
    process.stdin.write("pause\n" if enabled else "resume\n")
    process.stdin.flush()
    assert process.stdout.readline().strip() == ("paused" if enabled else "resumed")


def content(result):
    assert not result.isError, result.content
    return result.structuredContent or json.loads(result.content[0].text)


@pytest.mark.asyncio
async def test_mcp_protocol_and_memory(game):
    _, info = game
    parameters = StdioServerParameters(command=sys.executable,
                                      args=[str(ROOT / "mcp_server/fo4hud_server.py")],
                                      env={"FO4HUD_SOCKET": info["socket"]})
    async with stdio_client(parameters) as (read, write):
        async with ClientSession(read, write) as session:
            await session.initialize()
            catalog = await session.list_tools()
            names = {tool.name for tool in catalog.tools}
            assert {"move", "look", "select_dialogue", "activate_reference", "screenshot",
                    "read_memory", "write_memory", "teleport_player", "stop_actions",
                    "ui_state", "list_saves", "continue_game", "load_save",
                    "pause_game", "resume_game", "quit_game"} <= names
            status = content(await session.call_tool("game_status"))
            assert status["pid"] > 0 and not status["input_ready"]
            original = content(await session.call_tool("read_memory", {"address": info["address"], "size": 4}))
            assert original["data_hex"] == "01020000"
            written = content(await session.call_tool("write_memory", {
                "address": info["address"], "data_hex": "aa bb cc dd", "expected_hex": "01020000"}))
            assert written["status"] == "completed"
            assert written["result"]["verified"]
            assert written["result"]["before_hex"] == "01020000"
            assert written["result"]["after_hex"] == "aabbccdd"
            mismatch = await session.call_tool("write_memory", {
                "address": info["address"], "data_hex": "00000000", "expected_hex": "01020000"})
            assert mismatch.isError
            unchanged = content(await session.call_tool("read_memory", {"address": info["address"], "size": 4}))
            assert unchanged["data_hex"] == "aabbccdd"
            unavailable = await session.call_tool("move", {"direction": "forward", "duration_ms": 100})
            assert unavailable.isError
            assert "not ready" in unavailable.content[0].text
            assert (await session.call_tool("read_memory", {"address": "0x1", "size": 16})).isError
            assert content(await session.call_tool("stop_actions"))["cancelled"]


@pytest.mark.asyncio
async def test_screenshot_protocol_and_bgra_conversion(game):
    _, info = game
    parameters = StdioServerParameters(command=sys.executable,
                                      args=[str(ROOT / "mcp_server/fo4hud_server.py")],
                                      env={"FO4HUD_SOCKET": info["socket"]})
    async with stdio_client(parameters) as (read, write):
        async with ClientSession(read, write) as session:
            await session.initialize()
            result = await session.call_tool("screenshot", {})
            assert not result.isError, result.content
            block = next(block for block in result.content if block.type == "image")
            image = Image.open(io.BytesIO(base64.b64decode(block.data)))
            assert image.size == (2, 2)
            assert image.getpixel((0, 0)) == (255, 0, 0, 255)
            assert image.getpixel((0, 1)) == (0, 0, 255, 255)
            assert not list(Path(info["socket"]).parent.glob("*.capture-*.png"))


@pytest.mark.asyncio
async def test_cancel_expiry_and_no_late_writes(game):
    process, info = game
    bridge = Bridge(Path(info["socket"]))
    pause(process, True)
    queued = await bridge.request("write_memory", address=info["address"], data_hex="ff", queue_timeout_ms=100)
    await asyncio.sleep(0.15)
    expired = await bridge.request("command_result", command_id=queued["command_id"])
    assert expired["status"] == "cancelled"
    queued2 = await bridge.request("write_memory", address=info["address"], data_hex="ee")
    await bridge.request("stop")
    cancelled = await bridge.request("command_result", command_id=queued2["command_id"])
    # The cancellation generation is checked by the native event loop or tick.
    if cancelled["status"] == "queued":
        await asyncio.sleep(0.03)
        cancelled = await bridge.request("command_result", command_id=queued2["command_id"])
    assert cancelled["status"] == "cancelled"
    pause(process, False)
    await asyncio.sleep(0.05)
    assert (await bridge.request("read_memory", address=info["address"], size=1))["data_hex"] == "01"


@pytest.mark.asyncio
async def test_game_unavailable_is_explicit(tmp_path):
    bridge = Bridge(tmp_path / "missing.sock")
    with pytest.raises(BridgeError):
        await bridge.request("status")


@pytest.mark.asyncio
async def test_save_listing_and_ui_errors(game):
    _, info = game
    saves = Path(info["socket"]).parent.parent / "saves"
    saves.mkdir()
    (saves / "valid.fos").write_bytes(b"FO4_SAVEGAME\0")
    (saves / "ignore.f4se").write_bytes(b"cosave")
    (saves / "link.fos").symlink_to(saves / "valid.fos")
    bridge = Bridge(Path(info["socket"]))
    listed = await bridge.request("list_saves")
    assert [s["save_name"] for s in listed["saves"]] == ["valid.fos"]
    with pytest.raises(BridgeError, match="Menu control hooks unavailable"):
        await bridge.request("ui_action", action="continue")
    parameters = StdioServerParameters(command=sys.executable,
                                      args=[str(ROOT / "mcp_server/fo4hud_server.py")],
                                      env={"FO4HUD_SOCKET": info["socket"]})
    async with stdio_client(parameters) as (read, write):
        async with ClientSession(read, write) as session:
            await session.initialize()
            for name in ("../valid.fos", "valid.fos\n", "valid.f4se"):
                result = await session.call_tool("load_save", {"save_name": name})
                assert result.isError and "save_name must" in result.content[0].text
            listed = content(await session.call_tool("list_saves"))
            assert listed["count"] == 1
