#!/usr/bin/env python3
"""Local Fallout 4 MCP tools backed by the fo4hud in-process bridge."""
from __future__ import annotations

import asyncio
import contextlib
import io
import json
import os
from pathlib import Path
import socket
import stat
import struct
import time
from typing import Literal

from mcp.server.fastmcp import FastMCP, Image
from mcp.types import ToolAnnotations
from PIL import Image as PillowImage


class BridgeError(RuntimeError):
    pass


class Bridge:
    def __init__(self, path: Path | None = None):
        self.path = path
        self.input_lock = asyncio.Lock()
        self.ui_lock = asyncio.Lock()

    @staticmethod
    def candidates() -> list[Path]:
        if explicit := os.environ.get("FO4HUD_SOCKET"):
            return [Path(explicit)]
        uid = os.getuid()
        directories = {Path(f"/run/user/{uid}/fo4hud"), Path(f"/tmp/fo4hud-control-{uid}")}
        if runtime := os.environ.get("XDG_RUNTIME_DIR"):
            directories.add(Path(runtime) / "fo4hud")
        # Steam's pressure-vessel may provide a private /run or /tmp. A
        # pathname through the same user's process root reaches that socket
        # without entering its mount namespace or changing the game launch.
        for process in Path("/proc").glob("[0-9]*"):
            try:
                if process.stat().st_uid != uid or process.joinpath("comm").read_text().strip().lower() != "fallout4.exe":
                    continue
                directories.add(process / "root" / f"run/user/{uid}/fo4hud")
                directories.add(process / "root" / f"tmp/fo4hud-control-{uid}")
            except (OSError, PermissionError):
                continue
        return sorted({p for directory in directories for p in directory.glob("game-*.sock")})

    @staticmethod
    def exchange(path: Path, op: str, args: dict, timeout: float = 2) -> dict:
        try:
            info = path.lstat()
            if not stat.S_ISSOCK(info.st_mode) or info.st_uid != os.getuid():
                raise BridgeError("Bridge must be a Unix socket owned by the current user")
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
                client.settimeout(timeout)
                client.connect(str(path))
                _, uid, _ = struct.unpack("3i", client.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12))
                if uid != os.getuid():
                    raise BridgeError("Bridge peer UID does not match this user")
                payload = json.dumps({"op": op, "args": args}, allow_nan=False, separators=(",", ":"))
                if len(payload.encode()) > 299_999:
                    raise BridgeError("Bridge request is too large")
                client.sendall(payload.encode() + b"\n")
                data = bytearray()
                while b"\n" not in data:
                    chunk = client.recv(65536)
                    if not chunk:
                        raise BridgeError("Game bridge disconnected before returning a result")
                    data.extend(chunk)
                    if len(data) > 1_048_576:
                        raise BridgeError("Bridge response exceeded 1 MiB")
                result = json.loads(data.split(b"\n", 1)[0])
                if not isinstance(result, dict):
                    raise BridgeError("Invalid bridge response")
                if "error" in result:
                    raise BridgeError(json.dumps(result))
                return result
        except (OSError, ValueError) as error:
            raise BridgeError(f"{path}: {error}") from error

    async def sessions(self) -> list[dict]:
        sessions = []
        seen = set()
        for path in self.candidates():
            try:
                result = await asyncio.to_thread(self.exchange, path, "status", {}, 0.5)
                if result["pid"] in seen:
                    continue
                seen.add(result["pid"])
                sessions.append({"socket": str(path), **result})
            except BridgeError:
                continue
        return sessions

    async def connect(self, pid: int | None = None) -> dict:
        sessions = await self.sessions()
        if pid is not None:
            sessions = [session for session in sessions if session["pid"] == pid]
        if len(sessions) != 1:
            raise BridgeError("No running game bridge; restart Fallout 4 with fo4hud enabled" if not sessions
                              else "Multiple games found; call connect_game with the desired PID")
        self.path = Path(sessions[0]["socket"])
        return sessions[0]

    async def request(self, op: str, **args) -> dict:
        if self.path is None:
            await self.connect()
        assert self.path is not None
        return await asyncio.to_thread(self.exchange, self.path, op, args)

    async def command(self, op: str, *, _completion_timeout: float = 12, **args) -> dict:
        receipt = await self.request(op, **args)
        command_id = receipt["command_id"]
        try:
            deadline = time.monotonic() + _completion_timeout
            while time.monotonic() < deadline:
                receipt = await self.request("command_result", command_id=command_id)
                if receipt["status"] == "completed":
                    if "error" in receipt["result"]:
                        raise BridgeError(json.dumps(receipt))
                    return receipt
                if receipt["status"] == "cancelled":
                    raise BridgeError(f"Command {command_id} cancelled or expired before execution; no action performed")
                await asyncio.sleep(0.025)
            cancelled = await self.request("cancel_command", command_id=command_id)
            raise BridgeError(f"Command timed out: {json.dumps(cancelled)}; a running action may already have executed")
        except asyncio.CancelledError:
            with contextlib.suppress(BridgeError):
                await asyncio.shield(self.request("cancel_command", command_id=command_id))
            raise

    async def input(self, keys: list[int], duration_ms: int, look_x: float = 0, look_y: float = 0) -> dict:
        async with self.input_lock:
            receipt = await self.request("input", keys=keys, duration_ms=duration_ms, look_x=look_x, look_y=look_y)
            try:
                deadline = time.monotonic() + duration_ms / 1000 + 3
                while time.monotonic() < deadline:
                    state = await self.request("status")
                    if state["generation"] != receipt["generation"]:
                        return {"status": "interrupted", "reason": "manual input, emergency stop, or stop tool"}
                    if not state["input_busy"]:
                        return {"status": "input_finished", "duration_ms": duration_ms}
                    await asyncio.sleep(0.025)
                await self.request("stop")
                raise BridgeError("Wine input queue did not finish; input cancelled")
            except asyncio.CancelledError:
                with contextlib.suppress(BridgeError):
                    await asyncio.shield(self.request("stop"))
                raise


bridge = Bridge()
mcp = FastMCP(
    "Fallout 4",
    instructions=("Control the user's local Fallout 4 game through fo4hud. Observe before actions and verify afterward. "
                  "Use short movement steps and screenshot() to read dialogue/menu text. NPC interaction uses activate_reference; "
                  "select_dialogue uses the game's default 1..4 bindings. move uses actual key input; teleport_player changes "
                  "coordinates within the current worldspace. Memory addresses and form IDs are hex strings. "
                  "Use ui_state and list_saves for menus; continue_game, load_save, resume_game and quit_game use native menu control. "
                  "Loading replaces unsaved progress. Quit requests may show the game's confirmation; inspect screenshot before confirming. "
                  "Shift+F9 and physical key/button presses stop automation. Native commands that expire are never executed later."),
)
read_only = ToolAnnotations(readOnlyHint=True, openWorldHint=False)
action = ToolAnnotations(readOnlyHint=False, openWorldHint=False)


@mcp.tool(annotations=read_only)
async def list_games() -> dict:
    """List running fo4hud bridges, PIDs, builds, and input readiness."""
    return {"games": await bridge.sessions()}


@mcp.tool(annotations=read_only)
async def connect_game(pid: int | None = None) -> dict:
    """Select one running Fallout 4 process; omit PID when exactly one is running."""
    return await bridge.connect(pid)


@mcp.tool(annotations=read_only)
async def game_status() -> dict:
    """Check bridge/hook/input readiness even while the game is paused or loading."""
    return await bridge.request("status")


@mcp.tool(annotations=read_only)
async def observe_game(radius: float = 5000, limit: int = 100, name_filter: str = "") -> dict:
    """Read player position, rotation, health, rads, AP, cell, and nearby actors/items/plants.

    radius is 1..100000 world units, limit 1..500. Name filtering is case sensitive.
    Targets include addresses and form IDs suitable for activate_reference.
    """
    return await bridge.command("observe", radius=radius, limit=limit, filter=name_filter)


@mcp.tool(annotations=read_only)
async def read_inventory(address: str | None = None, limit: int = 200) -> dict:
    """Read live inventory stacks and counts; omit address for the player. Does not open containers."""
    args = {"limit": limit}
    if address is not None:
        args["address"] = address
    return await bridge.command("inventory", **args)


@mcp.tool(annotations=read_only)
async def screenshot(max_width: int = 1600) -> Image:
    """Capture only Fallout 4's current rendered frame, including dialogue and menus.

    Works while the game is paused as long as it continues presenting frames.
    max_width is 320..2560; the image retains its aspect ratio.
    """
    if not 320 <= max_width <= 2560:
        raise BridgeError("max_width must be 320..2560")
    result = (await bridge.command("screenshot"))["result"]
    filename = Path(result["path"]).name
    assert bridge.path is not None
    if not filename.startswith(bridge.path.name + ".capture-") or not filename.endswith(".png"):
        raise BridgeError("Invalid screenshot file receipt")
    # Resolve through the selected socket's directory, including a Steam
    # /proc/PID/root path when its runtime directory is container-private.
    path = bridge.path.parent / filename
    try:
        fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
        with os.fdopen(fd, "rb") as file:
            info = os.fstat(file.fileno())
            if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or info.st_size > 128 * 1024 * 1024:
                raise BridgeError("Invalid screenshot file")
            image = PillowImage.open(file)
            image.load()
        image.thumbnail((max_width, max_width * 4))
        output = io.BytesIO()
        image.save(output, format="PNG")
        return Image(data=output.getvalue(), format="png")
    finally:
        path.unlink(missing_ok=True)



@mcp.tool(annotations=read_only)
async def ui_state() -> dict:
    """List open game menus and menu-control readiness, including while paused or at the main menu."""
    return await bridge.request("ui_state")


@mcp.tool(annotations=read_only)
async def list_saves() -> dict:
    """List local .fos save filenames, newest modification first, without opening menus.

    Pass an exact save_name to load_save. Continue uses the game's current character,
    which can differ from the newest file across all characters. Supports the standard
    Steam/Proton save directory or FO4HUD_SAVE_DIR in the game's environment.
    """
    return await bridge.request("list_saves")


async def _menu_action(action_name: str, **args) -> dict:
    async with bridge.ui_lock:
        before = await bridge.request("ui_state")
        if not before.get("ui_hook_active"):
            raise BridgeError("Menu control unavailable; restart Fallout 4 with the updated fo4hud build")
        if before.get("loading"):
            raise BridgeError("Wait for the loading screen to finish")
        if not (before.get("main_menu") or before.get("pause_menu")):
            raise BridgeError("Open the main or pause menu first; use pause_game during gameplay")
        if action_name == "continue" and not before.get("main_menu"):
            raise BridgeError("Continue is only available at the main menu")
        receipt = await bridge.command("ui_action", action=action_name,
                                       _completion_timeout=90, **args)
        # Dispatch and completion of a loading transition are separate facts.
        # Menus may animate, request confirmation, or display a missing-mod warning.
        try:
            receipt["ui_after"] = await bridge.request("ui_state")
        except BridgeError:
            if not action_name.startswith("quit_"):
                raise
            # Normal desktop shutdown can remove the socket immediately after
            # the completed receipt. Keep the receipt; do not retry Quit.
            receipt["ui_after"] = {"available": False, "reason": "bridge disconnected after quit request"}
        return receipt


@mcp.tool(annotations=action)
async def continue_game() -> dict:
    """Continue the current character's last save from the main menu.

    Requests the game's native Continue action. Inspect ui_state/screenshot afterward;
    a loading transition or game warning may still need to finish.
    """
    return await _menu_action("continue")


@mcp.tool(annotations=action)
async def load_save(save_name: str) -> dict:
    """Load one exact .fos filename from list_saves, at the main or pause menu.

    Replaces unsaved progress. Uses the native load-menu path, including F4SE hooks.
    Paths, symlinks and non-save files are rejected. A dispatch receipt does not
    guarantee loading has finished; inspect ui_state and observe_game afterward.
    """
    if (not save_name or len(save_name) > 240 or not save_name.endswith(".fos")
            or save_name in (".", "..") or any(c in save_name for c in "/\\:\r\n\0")):
        raise BridgeError("save_name must be an exact .fos filename from list_saves, without directories")
    return await _menu_action("load", save_name=save_name)


@mcp.tool(annotations=action)
async def pause_game() -> dict:
    """Open the pause menu with Escape during normal gameplay. Does not close other menus."""
    async with bridge.ui_lock:
        before = await bridge.request("ui_state")
        if before.get("pause_menu"):
            return {"status": "already_paused", "ui_after": before}
        allowed = {"HUDMenu", "CursorMenu", "FaderMenu", "ButtonBarMenu", "VignetteMenu", "WaterMenu",
                   "ScopeMenu", "PowerArmorHUDMenu"}
        if before.get("main_menu") or before.get("loading") or set(before.get("menus", [])) - allowed:
            raise BridgeError("Close other menus or finish loading before pausing")
        result = await bridge.input([1], 80)
        if result["status"] == "interrupted":
            return result
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            after = await bridge.request("ui_state")
            if after.get("pause_menu"):
                return {"status": "paused", "ui_after": after}
            await asyncio.sleep(0.05)
        raise BridgeError("Escape was sent, but the pause menu did not open")


@mcp.tool(annotations=action)
async def resume_game() -> dict:
    """Close the pause menu through the native Resume action."""
    return await _menu_action("resume")


@mcp.tool(annotations=ToolAnnotations(readOnlyHint=False, destructiveHint=True, openWorldHint=False))
async def quit_game(destination: Literal["desktop", "main_menu"] = "desktop") -> dict:
    """Request the game's normal Quit action from the main or pause menu.

    Does not force-kill the process or save automatically. The game may show its own
    confirmation: inspect screenshot and use press_keys only after reading it.
    A quit_requested receipt means dispatch, not that the game has exited.
    """
    if destination not in ("desktop", "main_menu"):
        raise BridgeError("destination must be desktop or main_menu")
    return await _menu_action("quit_" + destination)


@mcp.tool(annotations=action)
async def stop_actions() -> dict:
    """Cancel pending native commands and release automated game keys/buttons immediately."""
    return await bridge.request("stop")


DIRECTIONS = {"forward": [17], "backward": [31], "left": [30], "right": [32],
              "forward_left": [17, 30], "forward_right": [17, 32],
              "backward_left": [31, 30], "backward_right": [31, 32]}
KEYS = {"w": 17, "a": 30, "s": 31, "d": 32, "e": 18, "f": 33, "r": 19, "t": 20,
        "space": 57, "tab": 15, "escape": 1, "enter": 28, "shift": 42, "ctrl": 29,
        "alt": 56, "1": 2, "2": 3, "3": 4, "4": 5, "up": 103, "down": 108,
        "left": 105, "right": 106, "mouse_left": 272, "mouse_right": 273, "mouse_middle": 274}


@mcp.tool(annotations=action)
async def move(direction: Literal["forward", "backward", "left", "right", "forward_left", "forward_right", "backward_left", "backward_right"],
               duration_ms: int = 250, sprint: bool = False) -> dict:
    """Walk using default WASD bindings for 1..5000 ms, then release. Observe after short steps.

    sprint also holds Left Shift; custom in-game bindings require press_keys instead.
    """
    return await bridge.input(DIRECTIONS[direction] + ([42] if sprint else []), duration_ms)


@mcp.tool(annotations=action)
async def press_keys(keys: list[str], duration_ms: int = 100) -> dict:
    """Press game keys or mouse buttons for 1..5000 ms and release them.

    Supported names: w/a/s/d/e/f/r/t, space, tab, escape, enter, shift, ctrl, alt,
    1/2/3/4, up/down/left/right, mouse_left/mouse_right/mouse_middle.
    Physical input cancels this action. At most 8 keys per action.
    """
    try:
        codes = [KEYS[key.lower()] for key in keys]
    except KeyError as error:
        raise BridgeError(f"Unsupported key: {error.args[0]}") from error
    return await bridge.input(codes, duration_ms)


@mcp.tool(annotations=action)
async def look(delta_x: float, delta_y: float) -> dict:
    """Move the game mouse relatively; deltas are -2000..2000 input units, sensitivity dependent."""
    return await bridge.input([], 100, delta_x, delta_y)


@mcp.tool(annotations=action)
async def activate_reference(address: str, form_id: str | None = None, max_distance: float = 300) -> dict:
    """Interact with a nearby NPC or object through normal activation, including scripts.

    Supply form_id from observe_game to detect reused addresses. max_distance is
    1..5000 world units. Activation can start dialogue, open a container, harvest,
    or trigger a door. The receipt confirms dispatch, not the script's outcome.
    """
    args = {"address": address, "max_distance": max_distance}
    if form_id is not None:
        args["form_id"] = form_id
    return await bridge.command("activate", **args)


@mcp.tool(annotations=action)
async def select_dialogue(option: Literal[1, 2, 3, 4]) -> dict:
    """Select dialogue with the default 1..4 bindings. Read screenshot() first to identify the choice."""
    return await bridge.input([KEYS[str(option)]], 100)


@mcp.tool(annotations=action)
async def teleport_player(x: float, y: float, z: float) -> dict:
    """Request engine movement to coordinates in the current cell/worldspace; this is a teleport.

    Uses the same native path as Papyrus SetPosition. Observe again afterward
    because the engine may defer the actual relocation to a later update.
    """
    return await bridge.command("set_transform", x=x, y=y, z=z)


@mcp.tool(annotations=action)
async def face_direction(yaw_degrees: float, pitch_degrees: float | None = None) -> dict:
    """Request absolute player facing: yaw 0=north, 90=east; optional pitch -89..89 degrees.

    Uses the game's transform path rather than writing position/rotation fields.
    The engine may apply it during a later update; observe to verify.
    """
    args = {"yaw_degrees": yaw_degrees}
    if pitch_degrees is not None:
        args["pitch_degrees"] = pitch_degrees
    return await bridge.command("set_transform", **args)


@mcp.tool(annotations=read_only)
async def memory_maps() -> dict:
    """List the connected game process's mapped address ranges, permissions, and module paths."""
    return await bridge.request("memory_maps")


@mcp.tool(annotations=read_only)
async def read_memory(address: str, size: int = 64) -> dict:
    """Read 1..65536 bytes from any readable address in the connected game process; returns hex."""
    return await bridge.request("read_memory", address=address, size=size)


@mcp.tool(annotations=action)
async def write_memory(address: str, data_hex: str, expected_hex: str | None = None, make_writable: bool = False) -> dict:
    """Write 1..65536 bytes on the game thread, with before/after bytes and a verification receipt.

    expected_hex optionally compares the original bytes before writing.
    make_writable temporarily enables writes on readable protected mappings,
    then restores their original permissions and reports restoration status.
    """
    try:
        data_hex = bytes.fromhex(data_hex).hex()
        if expected_hex is not None:
            expected_hex = bytes.fromhex(expected_hex).hex()
    except ValueError as error:
        raise BridgeError("data and expected bytes must be hex") from error
    args = {"address": address, "data_hex": data_hex, "make_writable": make_writable}
    if expected_hex is not None:
        args["expected_hex"] = expected_hex
    return await bridge.command("write_memory", **args)


if __name__ == "__main__":
    mcp.run(transport="stdio")
