# Host-side checks. Building the firmware for the board itself is `pio run`.

CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Iinclude
BUILD := .build
HOST_SRC := $(wildcard src/*.cpp) test/host/shim/host_runtime.cpp
HOST_FLAGS := -std=c++17 -O1 -Iinclude -Isrc -Itest/host/shim

.PHONY: test unit protocol firmware host portal assets clean

## Everything that can run without an ESP32.
test: unit protocol firmware

## Protocol, pulse timing, config rules and SHA-256, compiled natively.
unit: $(BUILD)/test_core
	@$(BUILD)/test_core

$(BUILD)/test_core: test/host/test_core.cpp $(wildcard include/*.h)
	@mkdir -p $(BUILD)
	@$(CXX) $(CXXFLAGS) -o $@ $<

## The wire protocol against a real in-process server-app.
protocol:
	@python -m pytest test/integration -q

## The real firmware (src/*.cpp) run as a process against a real server-app:
## claim the portal, configure it, and watch the switch pulse.
firmware: host
	@python -m pytest test/host/test_firmware.py -q

## Build src/*.cpp for this machine, using the Arduino shims in test/host/shim.
host: assets
	@mkdir -p $(BUILD)
	@$(CXX) $(HOST_FLAGS) -o $(BUILD)/firmware-host $(HOST_SRC)
	@echo "built $(BUILD)/firmware-host"

## Run that build so the portal can be opened in a browser:
##   make portal   ->  http://127.0.0.1:8095/
portal: host
	@TARDIS_HOST_STATE_DIR=$(BUILD) TARDIS_PORTAL_PORT=8095 $(BUILD)/firmware-host

## Regenerate include/web_assets.h from web/portal.html.
assets:
	@python tools/build_web_assets.py

clean:
	@rm -rf $(BUILD) .pio
