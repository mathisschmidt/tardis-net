# Host-side checks. Building the firmware itself is `pio run` (see README).

CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Iinclude
BUILD := .build

.PHONY: test unit integration portal assets clean

## Everything that can run without an ESP32.
test: unit integration

## Protocol, pulse timing, config rules and SHA-256, compiled natively.
unit: $(BUILD)/test_core
	@$(BUILD)/test_core

$(BUILD)/test_core: test/host/test_core.cpp $(wildcard include/*.h)
	@mkdir -p $(BUILD)
	@$(CXX) $(CXXFLAGS) -o $@ $<

## The firmware's flow driven against the real server-app API.
integration:
	@python -m pytest test/integration -q

## Serve the config portal on your laptop (Ctrl-C to stop).
portal:
	@python tools/mock_device.py --port 8090

## Regenerate include/web_assets.h from web/portal.html.
assets:
	@python tools/build_web_assets.py

clean:
	@rm -rf $(BUILD) .pio
