# fo4hud

A Dear ImGui window drawn into a Vulkan swapchain. On native Wayland, the
layer opens separate pointer and keyboard resources on the game's Wayland
display and passes their events to ImGui. Press Shift+F8 to show or hide the
window. Its shown/hidden state is saved and restored at startup; a fresh
configuration starts hidden.

Fallout 4 under Proton 10.0 uses DXVK (`d3d11.dll` and `dxgi.dll`) here, so the
layer renders after DXVK and before Vulkan presents the image.

## Source checkout and publication

Generated Vulkan manifests contain the checkout's absolute library path; build
and install them locally from `VK_LAYER_FO4HUD_overlay.json.in`. Vulkan does not
expand `$HOME` in manifest library paths. The generated manifest is ignored by Git.

The virtual environment, object/shared-library files, local ImGui settings,
debugger databases, downloaded `version-*.bin` address libraries, extracted radar
maps, and generated dialogue-affinity index are also ignored. The dialogue index
records absolute paths to the installed game files to detect changes; regenerate
it locally rather than replacing those paths with `$HOME`. Keep generated caches
and crash evidence local when sharing the source.

Before the first commit, use `git status --short --untracked-files=all` and
`git diff --cached` to review the files being published. This source tree includes
Dear ImGui's license at `imgui/LICENSE.txt`; a license for fo4hud's own source has
not yet been selected.

The GDB crash watcher (`tools/watch_game_crashes.py`) requires
`FO4HUD_CRASH_DIR` to select an evidence directory. It copies the Proton log from
`$PROTON_LOG_DIR/steam-377160.log`, or `$HOME/steam-377160.log` when
`PROTON_LOG_DIR` is unset. Set `FO4HUD_PROTON_LOG` to override the full log path.
These watcher paths expand `~` and environment variables. For example, launch
GDB with:

```sh
FO4HUD_CRASH_DIR="$HOME/.local/state/fo4hud/crash-investigation-$(date -u +%Y%m%dT%H%M%SZ)" \
FO4HUD_PROTON_LOG="$HOME/steam-377160.log" gdb -p GAME_PID
```

Then run `source /path/to/fo4hud/tools/watch_game_crashes.py` in GDB. Enable
Proton logging for the game with `PROTON_LOG=1` to produce the log being copied.

## Build and install

```sh
make -j4
make prepare-radar-maps
make install-layer
```

The radar map cache is extracted from your installed game archives by
`make prepare-radar-maps` (Python 3 with Pillow is required). The default Data
folder is `~/.local/share/Steam/steamapps/common/Fallout 4/Data`. For another
installation, run `python3 tools/extract_radar_maps.py --data '/path/to/Data'`.
The generated `assets/maps/*.fmap` files must stay beside the library's directory;
`FO4HUD_MAP_DIR` can point to another map cache. Re-extract after changing the
installed map textures. The extractor reads game files without modifying them.
Building the C++ layer also requires zlib development headers and library.

The manifest is installed at
`~/.local/share/vulkan/implicit_layer.d/VK_LAYER_FO4HUD_overlay.json` and points
to the absolute path of `libfo4hud.so` in this checkout. Reinstall if the checkout
moves. The layer is enabled only with `FO4HUD_ENABLE=1`; set
`FO4HUD_DISABLE=1` to suppress it. `FO4HUD_TRACE=1` enables startup logs.

Overlay checkboxes, sliders, and the actor name filter are saved to
`$XDG_CONFIG_HOME/fo4hud/settings.ini` (or `~/.config/fo4hud/settings.ini`).
The ImGui window position and size are saved to `imgui.ini` in the same
directory. Changes are written after a short pause and when the overlay closes.
The next game launch restores these preferences, including cheat toggles.
Perk points, caps, carry weight, speed, and jump/fall values are read from the
current game; their dialogs do not overwrite another save on startup.

The cheat tab's **Loot vendor chests (including locked)** toggle transfers
playable vendor stock within the auto loot range, independently of the normal
auto loot, inventory, theft, and category toggles. It bypasses chest locks and
ownership without unlocking or activating the chest. Transfers run every player
update when queued, with up to 128 object types per 4 ms batch; large quantities
transfer together. Identity, range, menu state, scripts, and quest-linked stacks
are checked during transfers. The toggle defaults off and is saved with the
other preferences.

Vendor markers show the vendor chest name only in the hover tooltip,
including script-created settlement stock containers. Recognition
checks plugin namespaces and stock base IDs, including DLC/light plugins and
winning overrides. `vendor_chest_records.h` contains the generated identities
and names from installed FACT.VENC, REFR.NAME, and vendor CONT records. After
changing plugins, regenerate with `make prepare-vendor-chests` (or run
`python3 tools/extract_vendor_chests.py --data /path/to/Data --language en`),
then rebuild and restart the game.


For Steam's Fallout 4 launch options:

```sh
FO4HUD_ENABLE=1 %command%
```

The additional layer path above makes the installed user manifest visible inside it.
The game must be launched after these variables are set. A layer cannot attach
to an already running Vulkan instance.

For keyboard and mouse input, use a Proton build with Wine's native Wayland
driver and add `PROTON_USE_WAYLAND=1`. The installed official Proton 10.0 build
on this machine has only `winex11.so`; `GE-Proton10-28` includes
`winewayland.so`. Select that GE build under Steam's Compatibility settings and
use these launch options:

```sh
FO4HUD_ENABLE=1 PROTON_USE_WAYLAND=1 %command%
```

The log line `fo4hud: native Wayland input initialized` confirms the layer
registered keyboard and pointer resources. Wine can lock the pointer; the
layer also registers for relative motion so ImGui coordinates continue to
move. Shift+F8 shows or hides the window.

The overlay incrementally scans the game's four actor process lists, the
player's current cell, and loaded exterior cells in the same worldspace.
Enabled features determine which lists, ranges, names, and inventories are
needed. Distant references and actors in unrelated spaces are rejected before
expensive metadata reads.
This read-only view uses object layouts from F4SE and CommonLibF4 for Fallout 4
1.11.240. It rejects other executable versions and refreshes after changing
cells. It logs a short cell transition summary when the player enters a new cell.

Entity reads copy related dynamic fields in one checked `process_vm_readv`
call and unpack the local copy. Only a small whitelist of fixed globals in
the verified executable (player/process pointers and projection data) uses
direct reads. Pointer targets, linked lists, and inventories retain checked
reads and their existing identity, bounds, and consistency checks.
`make test-entity-reads` covers the direct-read whitelist, inaccessible memory,
batched reads at page boundaries, pooled strings, and corrupt inventories.

Overlay refresh work uses a 1.5 ms soft budget per frame. Actor lists refresh
after 250 ms, world lists after 500 ms with auto loot/harvest or 1000 ms
otherwise. These intervals start when a pass completes; a large pass may take
several frames. Completed lists remain visible while replacements are built.
Actor positions, target identity, theft permission, and loot eligibility are
still checked live where they are used. Cell/worldspace/player changes cancel
unfinished work and clear old lists immediately.

Stable names and form classifications use bounded caches. Their source
identities are checked on use, with five-second name and two-second form
expiry. Container/corpse inventory hints refresh after one second nearby or
five seconds farther away. Inventory walks yield between entries/stack nodes
and confirm the reference and inventory header before publishing a result;
uncertain results remain unknown. Explicit MCP observations keep their full
synchronous snapshot behavior. `make test-entity-refresh` tests filtering,
cache invalidation, cell/worldspace transitions, changing inventories, and a
large incremental scan, printing blocking and incremental timing measurements.

Corpse/container colors keep the last confirmed inventory result while a new
check is pending or temporarily unreadable. Confirmed loot additions/removals
update the color. Expired or uncertain inventory data remains unknown for
auto-loot decisions; presentation hints do not suppress fresh loot checks.

The **config** tab's **Highlight terminal password** option enables a hacking
guide beside the terminal: a copy of the two-column board with the correct
password highlighted in yellow, plus its word, column, row, and character.
This saved setting defaults to enabled and works with the main HUD hidden.
The guide only appears during active hacking and disappears on success,
lockout, or menu closure. It reads game state without selecting words or
changing attempts. Checked reads validate the active TerminalMenu, hacking
mode, word array, password index, string, and board contents. For 1.11.240,
`ValidateHackingWord` at RVA `0x00B124F0` uses password index `0x02F2E19C`;
word records are at `0x0328FCC0`, and `0x00B12DB0` maps their positions to the
board text. `make test-hacking` checks these reads and overlay geometry.

Toggle **Show correct lockpick angle** in the HUD's **config** tab to control
the lockpick guide. The setting is saved and defaults to enabled.
When enabled, the guide appears near the top center while the
supported game's lockpicking menu is open and initialized. A green arc marks
the full sweet-spot range; the white needle follows the current pick angle
and turns green when aligned. A left/right hint guides the pick toward the
range. The guide uses foreground ImGui drawing without capturing input and
remains available when the main HUD window is hidden. It disappears when
the menu closes or the lock has been unlocked.

This is read-only: it does not move the pick, apply torque, or change lock
difficulty. For Fallout4.exe 1.11.240, it verifies UI's vtable and active menu
array, then checks LockpickingMenu's vtable, visibility, initialization, and
finite angle bounds. IDA's `UpdateSweetSpot` at RVA `0x0106BFD0` writes the
center at menu `+0x1A0` and full width at `+0x1A4`; the torque handler at
`0x0106D740` accepts half that width on each side of the center. Current pick
angle is at `+0x188`. `make test-lockpick` checks menu detection, invalid/stale
memory, full-width interpretation, direction mapping, and ImGui geometry at
640x480, 1920x1080, and 3440x1440.

## Trap warnings

**Trap warnings** in **config** defaults to enabled, with a saved range of
2,000 world units. Nearby traps get pulsing warning diamonds, a type label,
and distance even when the main HUD is hidden. Mines/explosives are red,
tripwires/pressure plates yellow, and other confirmed active traps pink.
A nearby warning also appears at the top of the screen within 400 units,
including when the trap is behind the player. Warnings never capture input.

Detection covers deployed proximity mines, tripwires/tension triggers,
pressure plates, gun traps, flamethrowers, electrical/radiation traps,
grenade bouquets, IEDs, gas/oil traps, alarms and other recognized trap scripts,
including triggers attached to containers and doors.
Cymbal monkeys are labeled **MONKEY TRAP**. When the head is destroyed they
remain marked **SPRUNG** in muted gray and stop producing a nearby hazard
alert. The invisible monkey trigger volume is omitted so the label sits on
the physical monkey. A game reset restores the active label.
Loose mine weapons are loot and are not marked as active mines. Disabled,
deleted, destroyed, disarmed/retired projectiles and script-disarmed/spent traps
are filtered (destroyed monkeys retain their sprung label). Collapsed breakable walkways in their script’s `Done` state are
hidden, and become eligible again after a reset. Mines remain marked during a
pending explosion until retired.
Recognized traps whose script state is not verified get an amber
**status unknown** label. Custom scripts inheriting the recognized base classes
are covered; unrelated custom trap systems may require additional bindings.

The bounded cell scanner supplies candidates independently of radar and loot.
Native script inspection runs on the game thread through the VM's own bound
object iterator; it reads script state and inherited variables without running
scripts or changing traps. Render-time checks remove invalid references and
respect cell/worldspace and distance boundaries. `make test-traps test-settings
test-entity-refresh` verifies detection, the VM visitor ABI, inherited armed
state, stale/disabled references, projectile-vs-loot handling, drawing and scan
integration. The native bindings are for Fallout4.exe 1.11.240.

## Dialogue affinity predictions

**Dialogue companion affinity** in **config** enables passive labels beside
choices that can affect supported companions within 2,500 game units in the
player's space. Labels such as **Cait likes** or **Cait dislikes** appear outside
the corresponding text and arrow: left of the left choice, right of the right
choice, above the up choice, and below the down choice. Green indicates likes
or loves; red indicates dislikes or hates. Conditional script branches are
marked conditional. Unverified choices have no label; this does not imply
neutrality. Dogmeat has no affinity system.

Button bounds are sampled from the dialogue movie on its UI thread and mapped
through its actual viewport, including ultrawide and safe-area positioning.
Stale or unavailable bounds suppress labels. Labels never take keyboard or
mouse input and work with the main HUD hidden.

The local index is generated from installed plugin INFO records and the winning
affinity dispatcher scripts, including the Unofficial Patch's script overrides.
FormID ownership is resolved through the winning file's master table, since
the runtime source-file list can contain only the overriding patch.
It currently covers explicit `CA_DB_Event` dialogue dispatch (575 INFO records
in this installation), with base-game, Old Longfellow and Porter Gage reactions.
It does not predict generic trait events, arbitrary mod scripts, delayed quest
stage rewards, speech-check outcomes, or the final numeric affinity change.
Witness eligibility, repeated lines, quest conditions and affinity limits still
apply. Unindexed record overrides are rejected. Source changes invalidate the
index instead of silently retaining old predictions. Normal plugin-list rewrites
on game exit are compared by active plugin names and order, so formatting and
comment changes do not invalidate the data.

Build [Champollion](https://github.com/Orvid/Champollion), then regenerate after
changing installed plugins or affinity scripts:

```sh
python3 tools/extract_dialogue_affinity.py --champollion /path/to/Champollion
```

The generated `assets/dialogue_affinity.json` stays beside the overlay library;
`FO4HUD_DIALOGUE_AFFINITY_INDEX` can select another local index. No downloaded
wiki tables or game scripts are bundled into the source. Restart the game after
regenerating. `make test-dialogue-affinity test-settings` verifies capture ABI,
option identity, shared responses, override rejection, stale data, extraction,
menu/target transitions, settings persistence and passive rendering.

Research: [Affinity mechanics on the Fallout Wiki](https://fallout.fandom.com/wiki/Affinity),
[the vanilla companion script published with F4SE](https://github.com/rux616/fallout4-f4se-original/blob/master/scripts/vanilla/CompanionActorScript.psc),
[XDI's dialogue engine research](https://github.com/reg2k/xdi/blob/master/src/DialogueEx.cpp),
and [Show Dialogue Affinity's coverage notes](https://www.nexusmods.com/fallout4/mods/97983).
The engine capture is independently verified in IDA for Fallout4.exe 1.11.240:
`BGSSceneActionPlayerDialogue` option construction calls `0x4168E0` at
`0x416466` with the selected player INFO, native option index, NPC response INFO
and owning quest. fo4hud captures those arguments and forwards the original call
once; it does not evaluate Papyrus scripts or choose any dialogue option.

## Actor information

The **Actor information nearby / during dialogue** toggle in **config** defaults to
on and retains the saved `companion_information` preference. A passive window
appears when looking directly at a nearby living actor within the game's
activation range, or while speaking with an actor. The game's own activation
target selects the actor, with the direct view pick also covering creatures
that have no talk prompt; other menus suppress the proximity display.
It shows race/type, companion affinity when applicable, current/maximum health and AP,
all seven current SPECIAL attributes, current/maximum carry weight, damage resistance, and
energy resistance. It also appears when the main HUD is hidden and never
captures dialogue input. The affinity bar uses the vanilla 1000-point target;
the numeric value remains visible for negative or higher affinity. It does
not imply that an affinity conversation, romance, or companion perk is unlocked.
Dogmeat's affinity is shown as not applicable. Missing stats are unavailable.
The panel also shows **Synth: Confirmed** or **Synth: No synth markers detected**. Confirmation uses a synth
race, the `ActorTypeSynth` keyword on the NPC or race, or the NPC's explicit
synth death-loot record (including the human-race synth records). A normal
human race or missing markers displays **No synth markers detected**. Inventory components and
faction allegiance are not treated as proof; custom synths without these
markers may display **No synth markers detected**.

Affinity uses the engine form lookup because `CA_Affinity` is absent from the
built-in actor-value array. Current carry weight uses the engine inventory
calculation, including stack/equipment effects, and preserves fractional and
over-capacity loads.

The supported 1.11.240 build is required. Detection verifies the active,
visible DialogueMenu and TESTopicManager's conversation target, or the HUD's
activation target with distance and cell checks. Both paths validate reference
handle generations. Companion faction membership (including runtime faction
additions) controls whether affinity is shown. Dogmeat is recognized separately. Custom companions using
the game's companion factions are supported. Native actor-value getters run
on the game thread every 200 ms; the renderer only reads a guarded snapshot.
Race/type labels include Human, Super mutant, Feral ghoul, Ghoul, and Synth.
Confirmed synth markers override human-looking race records; unknown custom
races use their in-game race name. The window disappears on target change,
looking away or leaving activation range (unless in dialogue), and samples expire
after one second without updates. `make test-companion test-settings` covers
identity/layout rejection, runtime factions, missing/nonfinite values,
health/AP maxima, stale snapshots, settings persistence, and passive UI layout.

## MCP game control

The layer includes a local game control bridge for Fallout 4 **1.11.240**.
Build the layer, install the Python server dependencies, and restart the game:

```sh
make -j4
make setup-mcp
codex mcp add fo4hud -- $HOME/src/fo4hud/mcp_server/run.sh
```

Use the absolute path to `mcp_server/run.sh` in your checkout. Reload your MCP
client after adding the server. Other stdio MCP clients can run that script
directly. Python dependencies are installed with `uv` into `.venv-mcp`.

The tools expose game discovery, player and nearby entity observations,
inventory reads, game-frame screenshots, timed movement, mouse look, key and
mouse button input, reference activation, dialogue selection, player position
and facing changes, memory mappings, and raw memory reads/writes. Start with
`list_games`, `connect_game`, `observe_game`, and `screenshot`. Entity addresses
and form IDs are hexadecimal strings; use the current observation when
activating an NPC or object. Activation runs the game's normal interaction
path, including scripts. Dialogue choices use the default number keys 1–4;
screenshots provide the visible dialogue and menu text.

Menu controls work at the main menu and in the pause menu, where the player's
simulation update is stopped. `ui_state` lists active menus without requiring
a player update. `list_saves` lists local `.fos` filenames across characters,
ordered by filesystem modification time. `continue_game` runs the game's
Continue action for its current character; this may differ from the newest
file across every character. `load_save(save_name)` loads an exact filename
from the list through the native load-menu path, preserving F4SE's load hooks.
It replaces unsaved progress. Filenames cannot contain directories, and
symlinks, missing files and invalid save headers are rejected. The standard
Steam/Proton save directory is used; set `FO4HUD_SAVE_DIR` in the game's
environment if the game's save location has been customized.

`pause_game` sends Escape only from ordinary gameplay and verifies that the
pause menu opened. `resume_game` uses the native Resume action.
`quit_game(destination="desktop")` and `quit_game(destination="main_menu")`
request the normal menu actions. They do not force-kill the process or save
automatically. Read a screenshot before answering any game confirmation with
`press_keys`. A receipt distinguishes dispatch from the resulting UI state;
loading, menu animations, missing-mod warnings and quit confirmations may still
be pending. Use `ui_state`, screenshots and `observe_game` to verify afterward.
The client allows up to 90 seconds for an executing native menu action to
return; queued actions retain the normal short expiry and cancellation rules.

The menu update hooks check the MainMenu and PauseMenu vtable slots against
Fallout 4 1.11.240 and refuse installation if another component replaced them.
The UI action paths validate native signatures and reject actions behind a
message box, console or loading screen. Restart Fallout 4 after rebuilding the
overlay and reconnect/reload the MCP server to expose the new tools.

Movement and mouse input require the native Wine Wayland driver, game focus,
and a keyboard event after launch. Close the overlay before sending input.
Movement uses default WASD/Shift bindings; `press_keys` supports other bindings.
Each key hold is limited to five seconds. Physical key/button presses, losing
focus, **Shift+F9**, and `stop_actions` cancel automation. Pending native
commands expire after two seconds by default, so a paused game cannot execute
an old action when resumed. A command already running cannot be undone by a
stop request. Screenshots can work in paused menus while frames are presented.
Position changes stay in the current worldspace; they do not load a destination
cell or travel between interiors and exteriors.

The bridge uses a same-user Unix socket with mode `0600` in a private `0700`
directory, and exposes no TCP listener. Steam container runtime paths are
discovered automatically through `/proc/PID/root`. Set `FO4HUD_SOCKET` on the
Python server to select an explicit socket, or `FO4HUD_CONTROL=0` on the game
to disable the bridge. Memory tools address only the game process, accept at
most 64 KiB per request, and report the bytes before and after a write.
`expected_hex` provides a comparison before writing; `make_writable` can
temporarily change page protections and restores them afterward. Invalid
addresses return errors. Raw writes can still crash or corrupt the game.

`make test-game-ui test-mcp` checks save-file validation, menu-state guards,
memory access, input scheduling/cancellation, and a real
stdio MCP session against a native bridge fixture. Running these tests also
requires `pytest` and `pytest-asyncio` in `.venv-mcp`.

```sh
uv pip install --python .venv-mcp/bin/python pytest pytest-asyncio
make test-mcp
```

## Fallout 4 internal function addresses

The function addresses below are for **Fallout4.exe 1.11.240.0** (PE timestamp
`0x6A727FCC`, image size `0x0425A000`). They are **RVAs**, so the runtime
address is the mapped `Fallout4.exe` base plus the listed value. The
executable inspected for this list has SHA-256
`fdcef37ac1230af6d0b0050eb2142b139ef3a867b37b9211fb6edfcc646072f8`.
The layer checks the PE timestamp, preferred image base, and image size before
using these addresses. Some call sites also check function bytes or vtable
entries. These offsets should be rechecked after a game update.

| Internal function | RVA | Used for |
| --- | ---: | --- |
| `Actor::HandleHealthDamage` | `0x0CD2AE0` | Instant kill hook and the normal health damage path |
| Player damage adjustment (Actor slot `0x131`) | `0x0D75170` | Block health/limb damage and radiation gain, including direct combat and fall damage |
| Player `ActorValueOwner::Mod` | `0x0CC9B40` | Action points, carry weight, and speed changes |
| `PlayerCharacter::Update` | `0x0D58F50` | Game-thread work for cheats, loot, recoil, hostility, and visibility |
| Player `ActorValueOwner::GetValue` | `0x0CC93D0` | Read carry weight and speed multiplier |
| Player `ActorValueOwner::GetModifier` | `0x0CC9E00` | Read the spent action-point modifier |
| `PlayerCharacter::SetPerkCount` | `0x0D75FF0` | Change available perk points |
| `PlayerCharacter::AddObjectToContainer` | `0x0C7AA20` | Add caps to inventory |
| `PlayerCharacter::RemoveItem` | `0x0C9ACD0` | Remove caps from inventory |
| `Actor::GetGoldAmount` | `0x0501D40` | Read the game's authoritative caps total |
| `Actor::IsHostileToActor` | `0x0C7BBA0` | Confirm aim candidates are hostile to the player |
| `TESObjectREFR::Activate` | `0x05144B0` | Trigger the cap stash loot script through normal activation |
| `TESObjectREFR::IsAnOwner` | `0x0562C90` | Use the game's `Actor.WouldBeStealing` ownership decision for auto loot |
| `bhkPickData` initializer | `0x02DC570` | Initialize an aim visibility ray cast |
| `TESObjectCELL::Pick` | `0x04CAB60` | Ray cast against the cell's collision world |
| Pick result `HasHit` | `0x1878C50` | Check whether the ray cast hit something |
| `TESObjectWEAP::Fire` projectile launch call | `0x047A096` | Redirect ordinary player weapon shots through a checked jump island |
| `Projectile::Launch` | `0x0E421E0` | Continue the original launch after adjusting shot angles |
| Direction-to-angles helper | `0x0EFAD10` | Convert the direction to the selected body point into firing angles |
| `Workshop::FindNearestValidWorkshop` | `0x0385720` | Identify the settlement near the player |
| Workshop scrap eligibility | `0x0384910` | Check the game's normal scrap rules |
| Workshop build-area check | `0x0384810` | Keep candidate references within the selected workshop |
| `Workshop::ContextData` initializer | `0x03897F0` | Prepare the game's scrap context |
| `Workshop::ScrapReference` | `0x0393F30` | Scrap one eligible reference through the normal game path |
| `TESObjectREFR::CreateRefHandle` | `0x05194C0` | Make workshop and reference handles for scrapping |
| Reference handle resolver | `0x022D060` | Keep each reference alive during the scrap call |

Some game functions are called through the object's vtable. The code reads
these slots at runtime because different object types can have different
implementations:

| Function | Vtable slot | Address information |
| --- | ---: | --- |
| `PlayerCharacter::PickUpObject` | `0xEC` | Player vtable slot points to RVA `0x0D62E60` in this build |
| Source reference `RemoveItem` | `0x6D` | Resolved from each corpse or container's vtable |
| Source reference `CreateInventory` | `0xA7` | Resolved from each corpse or container's vtable |
| `TESFlora::Activate` | `0x40` | Resolved from the flora base form's vtable |
| Havok cast-world destructor | `0x03` | Resolved from the cast world's vtable |

The fixed player vtables used to verify and install hooks are at RVAs
`0x256C938` (`PlayerCharacter`) and `0x256D450` (the player's
`ActorValueOwner` subobject). The actor damage hook uses the `Actor` vtable
at RVA `0x25683B0`. Other addresses in the source identify game data,
globals, and vtables rather than functions.

The `radar` tab plots nearby actors, loading entrances, containers, placed items, and
harvestable flora from the player's cell and loaded exterior cells in the same
worldspace. The player
arrow points up; the compass and
markers rotate with the player's facing. Hover a marker for its name, form ID,
and distance. Scroll over the radar circle to zoom from 100 to 100000 world
units. The `radar config` tab controls the five categories independently.
The Pip-Boy world map is drawn beneath the markers by default, using the live
worldspace's texture path and map bounds. Its zoom and rotation use the same
coordinate transform as the markers. The background is clipped to the radar
circle and texture bounds. When the radar tab is selected, moving the cursor
outside the window fades its frame, controls, and count to fully
transparent over 0.25 seconds; the minimap circle, N/S/W/E compass letters,
and markers stay visible. The letters sit inside the rim and rotate with the
map's heading; north is highlighted in yellow.
Moving the cursor back over the window fades the frame in over 0.15 seconds.
During gameplay with no menu cursor, only the minimap remains visible. Other
tabs remain opaque. `Map background (world / interior)` and `Map opacity` in
`radar config` control its visibility. The local cache includes the installed
Commonwealth, Far Harbor, and Nuka-World maps. Interiors use the game's local-map
renderer, with a private camera and the matching rendered NW/NE/SW corners.
This accounts for cell north-marker rotations. Its camera covers the loaded
interior scene's full world bound, including a margin, rather than the default
player-centered map area. Exploration fog is omitted for the radar capture;
the game's fog setting is restored immediately after that render pass, and
the player's exploration data is untouched. The radar's saved zoom still
controls which part of the captured floor plan fits inside the circle. Geometry
the engine has not loaded cannot be included until it becomes available.
The radar requests a new image
after moving 256 world units, changing elevation by 128 units, or after three
seconds; attempts are limited to once per 750 ms. Opening a game menu suspends
capture. Cell changes and elevation changes hide the previous image immediately.
No Pip-Boy interaction or extracted interior assets are needed. Areas without a
map retain the plain radar background. Missing map textures are logged to `radar.log` and do not hide
radar markers. Harvested plants use muted gray dots and outlines; ready plants
stay green. This reads the live harvested state, so regrown plants turn green
again. Plant tooltips also show whether they are harvested or ready.
Loot that the player would have to steal has a magenta ring and diagonal slash
around its normal category marker. Hover text identifies the theft restriction;
the radar configuration tab includes the badge legend. Empty inventories and
harvested plants remain muted. The classification uses the same native
player-permission decision as auto-loot, including faction/relationship access,
and the explicit `ExtraOwnership` no-crime exception used by harvesting and
pickup (for example, Graygarden crops). An owned reference with that exception
is permitted and does not receive a theft badge. This classification
is independent of the `Steal items` toggle. Ownership checks run on the game
thread in batches of at most 16 per 25 ms for the nearest 512 visible loot
references. Results refresh after 750 ms and expire after three seconds; unknown
results stay unmarked and say `Ownership check pending` on hover. Reference,
cell and player identities are checked before using results.
`Entrances` shows modeled doors with non-instant teleport data leading to another
cell. Model-less invisible placeholder doors (such as `0024A1A7`) are excluded,
even when they retain teleport links. A display name is not required.
The destination can be a cached cell or a linked-door handle; the latter
is resolved through the game's handle table with generation and object-identity
checks. A live nearby Trapdoor (`00191F25`) uses that handle path with a null
cached cell. Ordinary doors and instant teleports are omitted. Script-only travel
activators without teleport data are not detected. The existing `radar_doors`
setting is retained for saved configuration compatibility.
Visibility choices and separate exterior/interior zoom ranges are saved in
`settings.ini` (`radar_range` / `radar_interior_range`). Interiors default to a
2000-unit radius, independently of an existing exterior zoom. Scrolling changes
the current space's range for both the background and entity markers. Radar uses the
incremental refresh described above, and actor positions are read live.
Actor markers, world labels, skeletons, and shot redirect use cell identity to
exclude actors in unrelated interiors. In exterior cells, actors must share
the player's worldspace. Opening the NPC actor table requests actors in the
current space within 100000 world units, including their names.
Cell transition summaries are appended to
`$XDG_STATE_HOME/fo4hud/entities.log` (or `~/.local/state/fo4hud/entities.log`
when `XDG_STATE_HOME` is unset). Full automatic dumps are omitted from the
presentation path. The log is kept after the game exits.

World labels use each loaded actor's animated head position, or its torso
position when the head node is unavailable. Actors without a readable
skeleton use the reference position plus a fixed height. This keeps labels
attached as NPCs walk and sway.
The NPCs and Enemies tab has a `Skeletons` toggle, off by default. It follows
the loaded model's bone hierarchy and draws projected lines between animated
bone positions. The existing range, maximum ESP actors, and hide-dead controls
also apply to skeletons. Actors without loaded or recognized skeletons are
skipped.
The `Hide dead actors` checkbox in the NPCs and Enemies tab is on by default;
it removes dying and dead actors from world labels as their life state changes.
It does not hide actors in bleedout or essential-down states, and it does not
filter the actor table.
ESP label text scales with live distance: nearby actors use up to 22-pixel
text, while distant actors use 11-pixel text. Font sizes are rounded to whole
pixels to keep the ImGui font atlas bounded.

Auto loot is off by default. Its checkbox and 50–1000 world-unit range slider
are in the `auto loot` tab and are saved in `settings.ini`. While enabled, the
overlay selects loose item references within the full 3D range. The player
update hook rechecks each candidate and calls the game's `Actor::PickUpObject`
on the game thread, one item at a time. Requests and processing are spaced
50 ms apart (up to 20 attempts per second, depending on frame rate). Repeated
attempts on the same loose reference wait at least 150 ms. It pauses while a game
menu is open.
The separate `Auto loot corpses and containers` checkbox transfers inventory
stacks from nearby dead actors and containers into the player inventory using
the game's `RemoveItem` path. Each attempt transfers up to eight distinct objects,
with a two-millisecond soft time limit between native transfers. Chest/corpse
attempts can repeat every 50 ms. A rotating inventory cursor lets other items
proceed when a transfer is refused. Each transfer rechecks the source and
player cell, distance, and theft permission; inventory pointers are read again
after native calls. It is off by default and uses the same range.
Quest-linked references and inventory stacks are excluded, regardless of the
stealing setting. Both ExtraAliasInstanceArray and ExtraFromAlias attachments
keep an object manual, even if the attachment is currently empty, except for
three explicitly permitted corpse aliases: `Trappers` from `DLCCoast.esm`
(alias 13 of `DLC03MQ02` / Walk in the Park, local FormID `001B40`),
`Enemies_Wave2` (alias 9 of `DLC03MQ02Fight`, local FormID `043822`), and
`MirelurkCollection` (alias 3 of `DLC03_ADV011`, local FormID `032D34`).
These exceptions check the originating plugin, quest ID, alias ID and name.
A separate exception permits collection aliases in any quest when every bound
script is exactly `recollectionaliasscript` or `DefaultCollectionAliasOnLoad`,
with the reviewed parent hierarchy and default state. Either script alone or
both together is allowed, including `WorkshopRaiderAttack01` and
`WorkshopFeralGhoulAttack01`; unreviewed siblings/subclasses and empty or
unreadable binding sets remain manual. Inspection uses the VM's locked iterator
on the game thread; the renderer consumes its cached verdict.
Both kinds of exception apply only to dead/dying actors. Every attached alias must be permitted;
additional unknown aliases, empty/malformed arrays, and ExtraFromAlias still
reject autoloot. These exceptions do not apply to individual inventory stacks.
The reviewed `MirelurkCollection` death script and installed collection-alias
parent count deaths; stage 50 enables and activates the queen's marker at
Northwood Ridge Quarry. They do not handle looting or item removal, so taking
ordinary meat from a corpse leaves that encounter logic intact.
The settlement attack's `Raiders` alias runs `recollectionaliasscript` with
death tracking and hit stage 50, plus `DefaultCollectionAliasOnLoad` with load
stage 20. These scripts track the attack without handling loot or item removal;
the script-based exception applies across quests rather than to one alias ID.
If any stack of an inventory object is protected, the entire object is skipped because the
native transfer can choose any stack. The source is rechecked before inventory
creation and before every transfer, so a quest attachment added during a batch
stops further transfers.
Lock-bearing references (ExtraLock/ExtraLockList), activation-linked references,
and blocked activations stay manual. This deliberately includes containers
whose lock has already been unlocked; fo4hud does not infer activation safety
from a key or lock level. Containers with bound Papyrus scripts stay manual
unless every binding passes the reviewed-script allowlist. Allowed scripts are
the exact `lockExtraLootSCRIPT` class in its completed `donothing` state and
`containerOpenIfEmpty` in its default state, both with only the native
ObjectReference/Form/ScriptObject parent chain. Subclasses, unreviewed
co-attached scripts, pending initialization and unreadable state stay manual.
The bonus-loot script adds items in OnLoad according to lock level, then enters
an empty state. `containerOpenIfEmpty` only plays Open/Close animations in
OnLoad/OnClose based on whether the container is empty. Direct auto loot can
leave its visual state unchanged until it next loads or closes normally.
These exceptions do not bypass quest aliases, lock metadata, ownership or item
category checks. Same-name script replacements by mods require a fresh review.
Script inspection uses the VM's locked bound-object iterator on the
game thread, without executing scripts. Unavailable inspection and unreadable
or malformed extra data reject autoloot. Radar hover text explains known
reference exclusions; these hints never replace fresh checks before actions.
Direct transfers still do not activate a source. Quest aliases and currently
bound scripts cannot identify every possible quest dependency, including
scripts elsewhere that watch an ordinary object. These safeguards reduce risk
but cannot guarantee compatibility with every quest or mod.
The category checkboxes apply to both loose items and inventory stacks.
Non-playable weapons, armor, and ammunition are always excluded, including
creature attacks, turret guns, robot equipment, and Super Mutant armor.
The checks mirror this build's `IsPlayable` implementations (WEAP RVA
`0x004816C0`, ARMO `0x00262590`, AMMO `0x004468F0`). Weapon instance overrides
are also checked; unreadable flags or extra-data lists are skipped. Inventory
transfers skip the entire object if any weapon stack is non-playable, since
the transfer can choose any stack. `make test-loot-playability` covers the
type-specific flags, mixed stacks, unreadable data, and normal loot.

The saved categories are:
Junk, Weapons, Apparel, Aid, Ammo, Books, Ingredients, Keys, Notes, and Other
misc. Notes includes holotapes. Junk means a miscellaneous form with scrap
components; caps, loose mods, and other miscellaneous forms use Other misc.
Books, Keys and Notes default to disabled so their read/play/activation steps
remain manual. Other categories start enabled. Existing settings are migrated
once to disable those three categories, using `quest_loot_defaults=1` in saved
files; later explicit opt-ins are preserved. Enabling a category never overrides
the quest, lock, activation or scripted-container protections. Selections are
saved in `settings.ini`. Crop harvesting has its own switch and is unaffected by these
item categories. Cap stashes are included under `Other misc` with ordinary
auto loot; the corpses/containers option is not required. Their normal
`ObjectReference.Activate(player, false)` path dispatches the stash's loot
script to award its leveled caps amount and remove the stash. The installed
`Fallout4.esm` binds `loot_capsStash_tin` (0x0003000C) to
`bobbyPinBoxSCRIPT`: its `LL_BobbyPinBox` property points to
`Container_Loot_Pouch_Caps_Raider` (0x0006D4AE), whose nested money lists
award `Caps001` (0x0000000F). The script calls `akActionRef.AddItem` with
that leveled list, then disables/deletes the stash. The stash activator never
passes through the loose-object pickup path.
Harvestable mirelurk eggs (0x001715CD) also use normal scripted activation,
under `Aid`, without requiring crop harvesting or container looting.
`MirelurkHarvestableScript` awards one `MirelurkEgg` (0x0023E9D4) to the
activating actor and disables the egg reference. Hatching eggs (0x0016579B)
remain excluded. Only these known scripted loot activators are eligible. These use
the same range, theft check, and menu pause as other loot, with a five-second
retry delay while their script completes.
Settlement workshop workbenches and their resource containers are excluded by
base form and workshop keywords, with a final check before each transfer.
With `Steal items` off, auto loot calls the same ownership check as the game's
`Actor.WouldBeStealing` for each loose item, container, corpse, or crop. Owned
references the player may take because of faction or relationship permission
are included. References that would count as theft, or whose check is
unavailable, are skipped. Inventory objects with explicitly owned or unreadable
stacks are also skipped because the reference check cannot resolve their
individual ownership and the transfer does not specify a stack ID.
The saved `Steal items` checkbox allows those theft cases; it is off by default.
The old `auto_loot_owned` setting is read as its equivalent and saved as
`auto_loot_steal`. Settlement workshop workbenches and their resource containers
remain excluded. Direct inventory transfers may bypass the game's normal theft
prompt or crime event.
`Auto harvest nearby crops` is a separate, saved checkbox in the `auto loot` tab.
It uses the same range slider, selects harvestable FLOR references with a
produce item, and invokes the flora activation path on the game thread.
It applies the same theft setting and pauses in menus. The game's harvested
flag determines whether a plant is ready: depleted plants are skipped, and
regrown plants become eligible again. Failed activations can retry after two
seconds instead of suppressing the plant for ten minutes. Actual activation
results are logged as `auto_harvest_success` or `auto_harvest_failed` in
`combat.log`.
This covers wild flora as well as planted crops; trees are excluded.
Proximity does not check line of sight, so items behind nearby walls can be
collected.

On the installed GE-Proton10-28 build, `fo4hud: Wine Wayland input capture
initialized` confirms that the layer found Wine's listener tables. When ImGui
wants the mouse or keyboard, the layer filters Wine's matching button, motion,
wheel, and key callbacks. Releases for captured presses are filtered too. If
Wine's listener symbols or layout differ, the window shows `Wine input capture
unavailable` and the layer leaves Wine input untouched.

The `cheat` tab has `instant kill` and `invulnerable` checkboxes, both off on
the first run. When `instant kill` is enabled,
player-attributed health damage to an NPC reduces its remaining health before
the game's normal death handler runs. A second check after queued weapon-hit
processing covers player hits that damage only a limb and skip the health
callback, including robot limbs. Protected actors are eligible, matching
vanilla's rule that the player can kill them. Actors marked essential or
invulnerable in their runtime or base NPC flags are excluded; if an excluded
actor's health reaches zero from a player hit, the hook restores one health
before that handler decides whether to kill it. The hook is installed only
for the verified Fallout4.exe 1.11.240 build. The tab shows hook status and
hit counters. Installation and instant-hit events are appended to
`$XDG_STATE_HOME/fo4hud/combat.log` (or `~/.local/state/fo4hud/combat.log`).
When `invulnerable` is enabled, the player's damage adjustment virtual
(slot `0x131`) blocks negative Health and body-part condition changes, plus
positive Rads changes, before `Actor::AlterActorValue` applies them. This covers
combat and fall damage calls that bypass the `ActorValueOwner::Mod` virtual.
The tab counts blocked health/limb damage and radiation gains separately.
While enabled, the player update clears accumulated radiation, health and
limb damage through the native actor-value modifier, restoring health and
injured limbs and removing the radiation health cap. Healing, radiation
removal and other actor values retain the game's normal handling. This does
not enable the global god-mode or immortal-mode flags.
Explicit scripted kills that do not modify Health are outside this hook.
The Cheat tab also displays the player's available perk points. The
`Change perk points...` button opens a dialog for values from 0 to 127. Applying a value calls
the game's `PlayerCharacter::SetPerkCount` routine, checks the result, and logs
the old and new values to `combat.log`. The control is available only while
the supported game and player are loaded.

The Cheat tab also has a `Change jump and fall...` dialog. It reads the live
`fJumpHeightMin` and `fJumpFallHeightMin` game settings and displays the defaults
from the supported Fallout4.exe build: 64 and 600 world units. `Use defaults`
fills both fields; `Apply` queues the changes for the player's game update.
These settings apply game-wide while the game is running.
The `Change SpeedMult...` dialog reads the player's current movement speed
multiplier and shows 100 as the normal value. Changes are queued for the
player update hook, like carry weight changes.
The `Change caps...` dialog changes the player's caps through the game's
inventory functions. Large differences are applied in steps of at most 30000.
The Cheat tab shows both the raw inventory total and Fallout 4's own caps
getter result. Each step is checked against the game getter and logged to
`combat.log`, including the raw count, so a mismatch can be diagnosed.
The `Shot redirect` control in the Cheat tab is off by default. While right
mouse button is held, it selects an exposed body point on the nearest live
hostile actor within the adjustable screen-space radius. Head, chest, both
arms, and both legs are checked against the game's Havok collision world on
the player update thread. The head is chosen when exposed and within the aim
radius; otherwise the nearest exposed body point is used. One yellow ring marks
the selected point. No ring appears when the actor is fully obstructed. The
view and mouse input are not moved.
The `Aim distance` slider limits targets by 3D distance from the player, from
100 to 15000 world units; its default is 3000.
When the player fires an ordinary weapon, the layer verifies the target is
still alive and hostile, then checks the path from the weapon's launch origin
to the selected point. It aims the projectile directly at that point by
adjusting its launch angles. The check fails closed if the
target is stale, covered, or unavailable. Shot redirect pauses while the game
menu cursor makes the ImGui window interactive. When a skeleton is unavailable,
only the live head point is checked. Collision checks treat hits within 25
game units of a body point as part of the target. Thrown weapons and other
projectile launch paths outside `TESObjectWEAP::Fire` are not redirected.
For aim diagnostics, add `FO4HUD_AIM_TRACE=1` to the Steam launch options and
restart the game. The rate-limited trace is written to
`~/.local/state/fo4hud/aim.log` (or `$XDG_STATE_HOME/fo4hud/aim.log`). It records
aim gate states, target rejection counts, selected target coordinates, visible
body-point mask, right mouse transitions, and redirected shot angles.
The `disable weapon recoil` checkbox zeros the current player weapon's runtime
recoil values while enabled and restores them when turned off.

The `settlement scrap` tab's `Preview settlement scrap` button finds the nearest workshop
while the player stands inside its buildable area. It scans the currently
loaded cells, applies the same scrap eligibility and build-area checks used by
the game's `ScrapAll` path, and lists each eligible reference and position.
The workbench and actors are excluded. Crafting stations, power armor stations,
generators, water equipment, defenses, stores, storage, beds, crops, unnamed
objects, and newly placed objects start unchecked. The name-based categories
are conservative and can also leave ordinary objects unchecked. Cabinets start
checked unless they are unnamed or newly placed. Review the
list, check or uncheck individual rows, or use `Restore default selection`,
`Select all`, and `Select none`.
`Scrap selected references...` opens a confirmation dialog for the selected
count. After confirmation, the layer scraps four selected references per
player update using `Workshop::ScrapReference`, checking each reference again
before the call. `Stop scrap pass` cancels the remaining calls, and the pass
also stops if the player leaves the cell or build area. The preview has no
time limit; each reference is checked again before scrapping. Only loaded
references can be previewed, so a large or modded settlement may need another
pass after moving through it. The operation writes to `workshop.log` under
`~/.local/state/fo4hud/` (or `$XDG_STATE_HOME/fo4hud/`).

Wine locks the Wayland pointer in Fallout 4, so a hardware cursor cannot move
with the game's relative mouse input. The layer hides the Wayland cursor and
draws an ImGui cursor only while Fallout 4's menu cursor is registered and ImGui
captures the mouse. While the game menu cursor is inactive, a visible overlay
does not accept mouse or keyboard input, so looking around keeps working.
When the overlay is hidden, Fallout 4's software cursor remains visible by
itself. Mouse coordinates from
Wine's parent surface are passed through
without subsurface offset or scaling; they may need adjustment on some window
layouts. Xwayland still renders the window but has no ImGui input.

To smoke test on the desktop:

```sh
FO4HUD_ENABLE=1 vkcube --c 50
```

The layer currently supports one swapchain presented on a graphics queue. It
patches swapchain image usage to include color attachment when supported,
submits ImGui drawing after the application's rendering, then presents the
same image. Dear ImGui is vendored under `imgui/` with its license.

## LOD repair

LOD repair is handled by the external **Stuck LOD Fix - RD** F4SE plugin.
fo4hud contains no LOD refresh scheduler, highway triangle filtering, or LOD
render hooks. The former `bug fixes` tab and its LOD preferences were removed.

Install Stuck LOD Fix - RD and its **Runtime Database** dependency under the
game's `Data/F4SE/Plugins/` directory, then launch through a matching F4SE.
Runtime Database provides `f4rd-runtime.bin`; it is separate from Address Library.
The installed combination was selected for Fallout 4 1.11.240 / F4SE 0.7.9.
Plugin loading can be checked in `Documents/My Games/Fallout4/F4SE/f4se.log`.
`StuckLODFix.ini` enables the plugin's own startup diagnostics with `bLogging=1`.

## Radar map verification

`make test-radar-fade` checks hover fade transitions, persistent minimap
geometry, and restoring the window from fully transparent.
`make test-radar-map test-settings` checks coordinate transforms at different
headings and ranges, clipping at map edges, live map-data parsing and parent
inheritance, texture decoding, and saved settings. `make test-radar-vulkan`
requires a Vulkan GPU and the extracted Commonwealth map; it exercises the
actual texture registration/upload, map draw geometry, and repeated backend
shutdown/recreation without launching Fallout 4.
`make test-local-map` checks the interior request/render/readback pipeline with
a fake engine: rotated camera corners, padded image rows, format conversion,
capture throttling, expired requests, floor/loading/cell transitions, failed
captures, native resource cleanup, and forwarding the original render stage.
It also checks full-interior framing and fog-setting restoration after successful
renders, readback failures and render exceptions.
`make test-radar-ownership` checks permission-result classification, refresh and
expiry, stale references, cell transitions, abandoned requests, and bounded/fair
native batches. The radar fade test also checks that theft badges remain visible
with the markers when the window frame fades out.
The Vulkan test also checks interior texture updates, resize retirement and
backend shutdown. A live check in the workhouse (cell `000E3CC4`) confirmed
capture of its 3440x1440 local-map target and display beneath the radar markers.

The interior renderer is pinned to 1.11.240. A checked CALL at `0xC383F6`
runs capture on the game render thread before the original `0x21FA3B0`
cleanup stage. `0xADB360` constructs a private 688-byte renderer, `0x371350`
renders it, and `0x3712D0` destroys it. Camera axes/frustum are read exactly
as the Pip-Boy's corner routine at `0xADAE70` does. Logical render target 19
is resolved through `0x2F4D750`; the game’s DirectX capture/convert/release
functions at `0x184DF90`/`0x18473C0`/`0x1846DD0` copy the pixels into a bounded
CPU frame. The Vulkan layer owns and updates its own texture; no engine GPU
handles cross into ImGui.
The hook's jump island probes free pages immediately around the executable
before trying more distant addresses; Wine reserves much of the nearby heap
range. Allocation uses `MAP_FIXED_NOREPLACE`, preserving existing mappings.

The map reader was verified against Fallout4.exe 1.11.240:
`TESWorldSpace::GetMapData` at RVA `0x574440`, map texture lookup at `0x5744C0`,
and Pip-Boy corner setup at `0xB589D0`. The signed MNAM cell coordinates at
worldspace `+0x1B4` are multiplied by 4096 exactly as the Pip-Boy does; they are
not inferred from the image dimensions or the worldspace's terrain bounds.
Those corners do not span the full texture. `WorldMapHolder` in the installed
`Pipboy_MapPage.swf` applies image gutters through
`MapHolder.normalizedToMapSpace`: Commonwealth/default left/top 55/2048 and
right/bottom 58/2048; Far Harbor 0.00920067/0.03374173 left/right and
0.02515624/0.00958931 top/bottom; Nuka-World -0.045/-0.095 left/right and
0.0005/-0.155 top/bottom. The radar applies these after world normalization
and clips against the actual texture, including negative DLC gutters.
Regression coverage includes the reported roadside position and DLC calibration.

`make test-loot-safety` checks conservative extra-data exclusions and the native
VM inspection bridge, including missing VM data, invalid handles, and script
bindings, completed bonus-loot scripts, animation-only container scripts,
unfinished state, derived classes and
co-attached unknown scripts. `make test-inventory-loot-batch` covers protected sources, mixed quest
stacks, scripts or aliases added during transfers, and continued ordinary loot.
