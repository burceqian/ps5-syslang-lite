PS5_HOST ?= ps5
PS5_PORT ?= 9021

PS5_PAYLOAD_SDK ?= /opt/ps5-payload-sdk
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

# ps5-syslang-lite
#
# The whole payload is one registry write -- no SceShellUI injection, no managed
# reload, no elfldr dependency (see README.md and loader.c).
VERSION ?= V0.21.1
ELF_PREFIX ?= ps5-syslang-lite_$(VERSION)

LANGUAGES ?= ja:0 en-US:1 fr-FR:2 es-ES:3 de:4 it:5 nl:6 pt-PT:7 ru:8 ko:9 zh-Hant:10 zh-Hans:11 fi:12 sv:13 da:14 no:15 pl:16 pt-BR:17 en-GB:18 tr:19 es-419:20 ar:21 fr-CA:22 cs:23 hu:24 el:25 ro:26 th:27 vi:28 in:29 uk:30

BUILD_DIR ?= build

lang_code = $(word 1,$(subst :, ,$(1)))
lang_id = $(word 2,$(subst :, ,$(1)))

ELFS := $(foreach lang,$(LANGUAGES),$(BUILD_DIR)/$(ELF_PREFIX)-$(call lang_code,$(lang)).elf)
PROBE_ELF := $(BUILD_DIR)/$(ELF_PREFIX)-probe.elf

CFLAGS := -Wall -Werror -g -O0 -DSYS_LANG_VERSION='"$(VERSION)"'
LDLIBS := -lSceSystemService -lSceRegMgr -lkernel_sys

.PHONY: all probe test test-probe test-host clean

all: $(ELFS) $(PROBE_ELF)

$(BUILD_DIR):
	mkdir -p $@

define BUILD_LANGUAGE
$(BUILD_DIR)/$(ELF_PREFIX)-$(call lang_code,$(1)).elf: loader.c | $(BUILD_DIR)
	$$(CC) $$(CFLAGS) -DLANGUAGE_ID=$(call lang_id,$(1)) -o $$@ $$< $$(LDLIBS)
endef

$(foreach lang,$(LANGUAGES),$(eval $(call BUILD_LANGUAGE,$(lang))))

# Read-only diagnostic (`make probe`): reports the language the system is using
# next to the registry value behind it, and scans the neighbouring ids. Writes
# nothing, so it is safe to run as often as needed.
probe: $(PROBE_ELF)

$(PROBE_ELF): probe.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -rf $(BUILD_DIR)

test: $(BUILD_DIR)/$(ELF_PREFIX)-zh-Hans.elf
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $^

test-probe: $(PROBE_ELF)
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $^

# `make test-host` runs the payload's decision logic on this PC against a fake
# registry, so the "already in that language -> report it and write nothing"
# rule is covered by a test instead of by hand:
#
#   PASS sceRegMgrSetInt was never called
#   PASS the registry was written even though sys==11
#   ...
HOST_CC ?= cc
HOST_TEST := $(BUILD_DIR)/lite_logic_test

test-host: $(HOST_TEST)
	$(HOST_TEST)

$(HOST_TEST): tests/lite_logic_test.c loader.c | $(BUILD_DIR)
	$(HOST_CC) -O0 -g -Wall -Wextra -Wno-unused-parameter -DLANGUAGE_ID=11 \
	    -DSETTLE_SECONDS=0 -DSYS_LANG_VERSION='"host-test"' -o $@ $<
