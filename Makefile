CXX ?= g++
CC ?= cc
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++17 -fPIC -DIMGUI_IMPL_VULKAN_NO_PROTOTYPES -Iimgui
SOURCES = dialogue_affinity.cpp layer.cpp wayland_input.cpp wine_input_hooks.cpp logging.cpp game_entities.cpp radar_map.cpp local_map.cpp control_memory.cpp game_control.cpp game_ui.cpp game_combat.cpp game_workshop.cpp settings.cpp imgui/imgui.cpp imgui/imgui_draw.cpp imgui/imgui_tables.cpp imgui/imgui_widgets.cpp imgui/backends/imgui_impl_vulkan.cpp
OBJECTS = $(SOURCES:.cpp=.o) protocols/relative-pointer-protocol.o
LIBRARY = libfo4hud.so
MANIFEST = VK_LAYER_FO4HUD_overlay.json
LAYER_DIR = $(HOME)/.local/share/vulkan/implicit_layer.d

all: $(LIBRARY)

$(LIBRARY): $(OBJECTS) layer_exports.map
	$(CXX) -shared -Wl,--version-script=layer_exports.map -o $@.new $(OBJECTS) -ldl -pthread -lwayland-client -lxkbcommon -lz -ljson-c
	mv -f $@.new $@

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

dialogue_affinity.o: dialogue_affinity.h control_memory.h control_json.h
layer.o: dialogue_affinity_overlay.h dialogue_affinity.h workshop_selection.h radar_map.h local_map.h game_entities.h settings.h lockpick_overlay.h hacking_overlay.h companion_overlay.h trap_overlay.h
local_map.o: local_map.h radar_map.h game_entities.h logging.h
settings.o: settings.h
game_entities.o game_control.o game_combat.o game_workshop.o radar_map.o: game_entities.h
radar_map.o game_entities.o: radar_map.h
game_entities.o game_combat.o: loot_categories.h loot_playability.h
layer.o game_combat.o: loot_safety.h
layer.o game_combat.o: game_combat.h
game_control.o control_memory.o: control_json.h control_memory.h game_control.h
layer.o wayland_input.o: game_control.h
game_control.o game_ui.o: game_ui.h game_control.h control_json.h control_memory.h
wine_input_hooks.o: wine_input_hooks.h

setup-mcp:
	uv venv --allow-existing .venv-mcp
	uv pip install --python .venv-mcp/bin/python -r mcp_server/requirements.txt

prepare-radar-maps:
	python3 tools/extract_radar_maps.py

protocols/relative-pointer-protocol.o: protocols/relative-pointer-protocol.c protocols/relative-pointer-client-protocol.h
	$(CC) -O2 -fPIC -c -o $@ $<

$(MANIFEST): $(LIBRARY) VK_LAYER_FO4HUD_overlay.json.in
	sed 's|@LIBRARY_PATH@|$(abspath $(LIBRARY))|' VK_LAYER_FO4HUD_overlay.json.in > $@

install-layer: $(MANIFEST)
	install -d $(LAYER_DIR)
	install -m 644 $(MANIFEST) $(LAYER_DIR)/$(MANIFEST)

test-control-memory: tests/control_memory_test.cpp control_memory.o
	$(CXX) $(CXXFLAGS) -o /tmp/fo4hud-control-memory-test tests/control_memory_test.cpp control_memory.o -ljson-c
	/tmp/fo4hud-control-memory-test

test-control-input: tests/control_input_test.cpp wine_input_hooks.cpp logging.o protocols/relative-pointer-protocol.o
	$(CXX) $(CXXFLAGS) -o /tmp/fo4hud-control-input-test tests/control_input_test.cpp logging.o protocols/relative-pointer-protocol.o -ldl -pthread -lwayland-client
	/tmp/fo4hud-control-input-test

control-bridge-fixture: tests/control_bridge_fixture.cpp $(filter-out layer.o,$(OBJECTS))
	$(CXX) $(CXXFLAGS) -o /tmp/fo4hud-control-bridge-fixture tests/control_bridge_fixture.cpp $(filter-out layer.o,$(OBJECTS)) -ldl -pthread -lwayland-client -lxkbcommon -lz -ljson-c

test-mcp: test-control-memory test-control-input control-bridge-fixture
	.venv-mcp/bin/python -m pytest -q tests/test_mcp_server.py

test-radar-fade: tests/radar_fade_test.cpp layer.cpp $(filter-out layer.o,$(OBJECTS))
	$(CXX) $(CXXFLAGS) -o /tmp/fo4hud-radar-fade-test tests/radar_fade_test.cpp $(filter-out layer.o,$(OBJECTS)) -ldl -pthread -lwayland-client -lxkbcommon -lz -ljson-c
	/tmp/fo4hud-radar-fade-test

test-radar-vulkan: tests/radar_texture_vulkan_test.cpp layer.cpp $(filter-out layer.o,$(OBJECTS))
	$(CXX) $(CXXFLAGS) -o /tmp/fo4hud-radar-vulkan-test tests/radar_texture_vulkan_test.cpp $(filter-out layer.o,$(OBJECTS)) -ldl -pthread -lwayland-client -lxkbcommon -lz -ljson-c
	FO4HUD_MAP_DIR=$(abspath assets/maps) /tmp/fo4hud-radar-vulkan-test

test-radar-map: tests/radar_map_test.cpp radar_map.cpp radar_map.h game_entities.cpp game_entities.h
	$(CXX) $(CXXFLAGS) -o /tmp/fo4hud-radar-map-test tests/radar_map_test.cpp radar_map.cpp game_entities.cpp -ldl -lz
	/tmp/fo4hud-radar-map-test

test-local-map: tests/local_map_test.cpp local_map.cpp local_map.h radar_map.h logging.o
	$(CXX) $(CXXFLAGS) -o /tmp/fo4hud-local-map-test tests/local_map_test.cpp logging.o -pthread
	/tmp/fo4hud-local-map-test

.PHONY: test-local-map

test-radar-ownership: tests/radar_ownership_test.cpp game_combat.cpp game_combat.h game_entities.o game_workshop.o logging.o
	$(CXX) $(CXXFLAGS) -o /tmp/fo4hud-radar-ownership-test tests/radar_ownership_test.cpp game_entities.o game_workshop.o logging.o -pthread
	/tmp/fo4hud-radar-ownership-test

.PHONY: test-radar-ownership

test-cap-stash: tests/cap_stash_test.cpp game_combat.cpp game_entities.o game_workshop.o logging.o
	$(CXX) $(CXXFLAGS) -o /tmp/fo4hud-cap-stash-test tests/cap_stash_test.cpp game_entities.o game_workshop.o logging.o -pthread
	/tmp/fo4hud-cap-stash-test

test-loot-playability: tests/loot_playability_test.cpp game_combat.cpp loot_categories.h loot_playability.h game_entities.o game_workshop.o logging.o
	$(CXX) $(CXXFLAGS) -o /tmp/fo4hud-loot-playability-test tests/loot_playability_test.cpp game_entities.o game_workshop.o logging.o -pthread
	/tmp/fo4hud-loot-playability-test

.PHONY: test-loot-playability

test-inventory-loot-batch: tests/inventory_loot_batch_test.cpp game_combat.cpp game_combat.h loot_playability.h game_entities.o game_workshop.o logging.o
	$(CXX) $(CXXFLAGS) -o /tmp/fo4hud-inventory-loot-batch-test tests/inventory_loot_batch_test.cpp game_entities.o game_workshop.o logging.o -pthread
	/tmp/fo4hud-inventory-loot-batch-test

.PHONY: test-inventory-loot-batch

test-auto-harvest: tests/auto_harvest_test.cpp game_combat.cpp game_entities.o game_workshop.o logging.o
	$(CXX) $(CXXFLAGS) -o /tmp/fo4hud-auto-harvest-test tests/auto_harvest_test.cpp game_entities.o game_workshop.o logging.o -pthread
	/tmp/fo4hud-auto-harvest-test

test-instant-kill: tests/instant_kill_test.cpp game_combat.cpp game_entities.o game_workshop.o logging.o
	$(CXX) $(CXXFLAGS) -o /tmp/fo4hud-instant-kill-test tests/instant_kill_test.cpp game_entities.o game_workshop.o logging.o -pthread
	/tmp/fo4hud-instant-kill-test

test-invulnerability: tests/invulnerability_test.cpp game_combat.cpp game_entities.o game_workshop.o logging.o
	$(CXX) $(CXXFLAGS) -o /tmp/fo4hud-invulnerability-test tests/invulnerability_test.cpp game_entities.o game_workshop.o logging.o -pthread
	/tmp/fo4hud-invulnerability-test

test-settings: tests/settings_test.cpp settings.cpp settings.h
	$(CXX) $(CXXFLAGS) -o /tmp/fo4hud-settings-test tests/settings_test.cpp settings.cpp
	/tmp/fo4hud-settings-test


test-loot-categories: tests/loot_categories_test.cpp loot_categories.h
	$(CXX) $(CXXFLAGS) -o /tmp/fo4hud-loot-categories-test tests/loot_categories_test.cpp
	/tmp/fo4hud-loot-categories-test

test-workshop-guard: tests/workshop_guard_test.cpp game_entities.cpp game_entities.h
	$(CXX) $(CXXFLAGS) -o /tmp/fo4hud-workshop-guard-test tests/workshop_guard_test.cpp game_entities.cpp
	/tmp/fo4hud-workshop-guard-test

test-ownership-guard: tests/ownership_guard_test.cpp game_entities.cpp game_entities.h
	$(CXX) $(CXXFLAGS) -o /tmp/fo4hud-ownership-guard-test tests/ownership_guard_test.cpp game_entities.cpp
	/tmp/fo4hud-ownership-guard-test

test-workshop-selection: tests/workshop_selection_test.cpp workshop_selection.h
	$(CXX) $(CXXFLAGS) -o /tmp/fo4hud-workshop-selection-test tests/workshop_selection_test.cpp
	/tmp/fo4hud-workshop-selection-test

test-loading-transition: tests/loading_transition_test.cpp game_entities.cpp game_entities.h
	$(CXX) $(CXXFLAGS) -o /tmp/fo4hud-loading-transition-test tests/loading_transition_test.cpp game_entities.cpp
	/tmp/fo4hud-loading-transition-test

test-entity-refresh: tests/entity_refresh_test.cpp game_entities.cpp game_entities.h loot_categories.h loot_playability.h
	$(CXX) $(CXXFLAGS) -o /tmp/fo4hud-entity-refresh-test tests/entity_refresh_test.cpp
	/tmp/fo4hud-entity-refresh-test

test-lockpick: tests/lockpick_test.cpp game_entities.cpp game_entities.h lockpick_overlay.h imgui/imgui.o imgui/imgui_draw.o imgui/imgui_tables.o imgui/imgui_widgets.o
	$(CXX) $(CXXFLAGS) -o /tmp/fo4hud-lockpick-test tests/lockpick_test.cpp imgui/imgui.o imgui/imgui_draw.o imgui/imgui_tables.o imgui/imgui_widgets.o
	/tmp/fo4hud-lockpick-test

test-hacking: tests/hacking_test.cpp game_entities.cpp game_entities.h hacking_overlay.h imgui/imgui.o imgui/imgui_draw.o imgui/imgui_tables.o imgui/imgui_widgets.o
	$(CXX) $(CXXFLAGS) -o /tmp/fo4hud-hacking-test tests/hacking_test.cpp imgui/imgui.o imgui/imgui_draw.o imgui/imgui_tables.o imgui/imgui_widgets.o
	/tmp/fo4hud-hacking-test

.PHONY: test-hacking

test-entity-reads: tests/entity_reads_test.cpp game_entities.cpp game_entities.h loot_categories.h loot_playability.h
	$(CXX) $(CXXFLAGS) -o /tmp/fo4hud-entity-reads-test tests/entity_reads_test.cpp
	/tmp/fo4hud-entity-reads-test

clean:
	rm -f $(OBJECTS) $(LIBRARY) $(LIBRARY).new $(MANIFEST)

.PHONY: all clean install-layer test-mcp test-control-memory test-control-input control-bridge-fixture setup-mcp test-cap-stash test-auto-harvest test-radar-fade test-radar-vulkan test-radar-map prepare-radar-maps test-instant-kill test-invulnerability test-settings test-loot-categories test-workshop-guard test-ownership-guard test-workshop-selection test-loading-transition test-entity-reads test-entity-refresh test-lockpick



test-companion: tests/companion_test.cpp game_entities.cpp game_entities.h companion_overlay.h imgui/imgui.o imgui/imgui_draw.o imgui/imgui_tables.o imgui/imgui_widgets.o
	$(CXX) $(CXXFLAGS) -o /tmp/fo4hud-companion-test tests/companion_test.cpp imgui/imgui.o imgui/imgui_draw.o imgui/imgui_tables.o imgui/imgui_widgets.o -pthread
	/tmp/fo4hud-companion-test

.PHONY: test-companion

test-traps: tests/trap_test.cpp game_entities.cpp game_entities.h trap_overlay.h imgui/imgui.o imgui/imgui_draw.o imgui/imgui_tables.o imgui/imgui_widgets.o
	$(CXX) $(CXXFLAGS) -o /tmp/fo4hud-trap-test tests/trap_test.cpp imgui/imgui.o imgui/imgui_draw.o imgui/imgui_tables.o imgui/imgui_widgets.o -pthread
	/tmp/fo4hud-trap-test

.PHONY: test-traps



test-game-ui: tests/game_ui_test.cpp game_ui.cpp game_ui.h game_control.h control_memory.o logging.o
	$(CXX) $(CXXFLAGS) -o /tmp/fo4hud-game-ui-test tests/game_ui_test.cpp control_memory.o logging.o -ljson-c -pthread
	/tmp/fo4hud-game-ui-test

.PHONY: test-game-ui

test-dialogue-affinity: tests/dialogue_affinity_test.cpp dialogue_affinity.cpp dialogue_affinity.h dialogue_affinity_overlay.h control_memory.o logging.o imgui/imgui.o imgui/imgui_draw.o imgui/imgui_tables.o imgui/imgui_widgets.o
	$(CXX) $(CXXFLAGS) -o /tmp/fo4hud-dialogue-affinity-test tests/dialogue_affinity_test.cpp control_memory.o logging.o imgui/imgui.o imgui/imgui_draw.o imgui/imgui_tables.o imgui/imgui_widgets.o -pthread -ldl -ljson-c
	/tmp/fo4hud-dialogue-affinity-test
	python3 tests/dialogue_affinity_extract_test.py

.PHONY: test-dialogue-affinity

test-loot-safety: tests/loot_safety_test.cpp loot_safety.h game_entities.o
	$(CXX) $(CXXFLAGS) -o /tmp/fo4hud-loot-safety-test tests/loot_safety_test.cpp game_entities.o
	/tmp/fo4hud-loot-safety-test

.PHONY: test-loot-safety
