# Host unit tests for the firmware's portable core (components/hifi/core).
#   make -C firmware/test        builds and runs them all
# Plain C11 with gcc; AddressSanitizer and UBSan catch memory and UB bugs
# that would only show on the board as a crash.

CORE := ../components/hifi/core
CC ?= cc
CFLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-unused-function -I$(CORE) \
	  -fsanitize=address,undefined,float-cast-overflow -fno-sanitize-recover=all
LDLIBS := -lm

SRCS := $(wildcard $(CORE)/*.c)
BIN := build
TESTS := test_json_pd test_power test_gov_tone test_vol_ir_meta test_api test_remote test_core test_line test_tap test_diag test_lab

# test_remote signs and checks commands with real Ed25519 where the host has
# OpenSSL's libcrypto, and with a stand-in signature where it doesn't.
HAVE_OPENSSL := $(shell printf '\043include <openssl/evp.h>\nint main(void){return 0;}' | $(CC) -x c - -lcrypto -o /dev/null 2>/dev/null && echo 1)
ifeq ($(HAVE_OPENSSL),1)
$(BIN)/test_remote: CFLAGS += -DHAVE_OPENSSL
$(BIN)/test_remote: LDLIBS += -lcrypto
endif

all: run

$(BIN):
	mkdir -p $(BIN)

$(BIN)/%: %.c $(SRCS) $(wildcard $(CORE)/*.h) check.h | $(BIN)
	$(CC) $(CFLAGS) -o $@ $< $(SRCS) $(LDLIBS)

run: $(addprefix $(BIN)/,$(TESTS))
	@fail=0; for t in $(abspath $^); do $$t || fail=1; done; python3 test_otadata.py || fail=1; \
	  $(MAKE) --no-print-directory upstream || fail=1; \
	  $(MAKE) --no-print-directory fuzz || fail=1; \
	  $(MAKE) --no-print-directory qemu || fail=1; exit $$fail

# Regressions for the bring-up bugs fixed in upstream's code (patches/):
# upstream/upstream.py fetches the pinned files once (from $$HIFI_UPSTREAM or
# GitHub), applies our patches, and pulls the patched functions out, which
# the tests build against stubs. With no copy of upstream reachable they are
# skipped, loudly; UPSTREAM_REQUIRED=1 makes that a failure.
UP := $(BIN)/upstream
UP_FILES := components/squeezelite/controls.c components/driver_bt/bt_app_sink.c \
	components/driver_bt/bt_app_core.c components/spotify/Shim.cpp components/spotify/cspot/src/LoginBlob.cpp \
	components/wifi-manager/network_manager_handlers.c
UP_CFLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-unused-function -Wno-unused-parameter \
	-Wno-unused-variable -Wno-unused-const-variable -fsanitize=address,undefined -fno-sanitize-recover=all -I$(UP)/gen
upstream:
	@mkdir -p $(UP)/gen; python3 upstream/upstream.py files $(UP) $(UP_FILES); rc=$$?; \
	if [ $$rc = 3 ]; then echo "SKIPPED upstream regressions: no copy of upstream (set HIFI_UPSTREAM)"; \
	  exit $(if $(filter-out 0,$(UPSTREAM_REQUIRED)),1,0); fi; [ $$rc = 0 ] || exit 1; \
	python3 upstream/upstream.py extract $(UP)/components/squeezelite/controls.c \
	  connect_cli_socket cli_send_cmd > $(UP)/gen/controls_fns.c && \
	python3 upstream/upstream.py extract $(UP)/components/driver_bt/bt_app_sink.c \
	  IDLE_RELEASE_S IDLE_CHECK_S idle_check bt_disconnect bt_av_hdl_a2d_evt > $(UP)/gen/bt_fns.c && \
	$(CC) $(UP_CFLAGS) -o $(UP)/test_up_controls upstream/test_up_controls.c && \
	$(CC) $(UP_CFLAGS) -o $(UP)/test_up_bt upstream/test_up_bt.c || exit 1; \
	fail=0; $(UP)/test_up_controls || fail=1; $(UP)/test_up_bt || fail=1; \
	python3 upstream/test_up_source.py $(UP) || fail=1; exit $$fail

# Line, branch and function coverage of core/ (gcov): each test is built
# again with --coverage under build/cov and run there; `fw coverage` merges
# the counts (`fw coverage firmware --src components/hifi/core`).
COV := $(BIN)/cov
coverage:
	rm -rf $(COV) && mkdir -p $(COV)
	@fail=0; for t in $(TESTS); do \
	  extra=; [ $$t = test_remote ] && [ "$(HAVE_OPENSSL)" = 1 ] && extra="-DHAVE_OPENSSL -lcrypto"; \
	  (cd $(COV) && $(CC) -std=gnu11 -O0 -g --coverage -I$(abspath $(CORE)) -o $$t $(abspath $$t.c) \
	    $(abspath $(SRCS)) $(LDLIBS) $$extra && ./$$t >/dev/null) || fail=1; \
	done; exit $$fail

# libFuzzer harnesses (fuzz/): the console, the HTTP test API, the JSON
# reader and writer, the relay's signed envelope and its websocket client.
# Each is built with clang, ASan and UBSan; its corpus grows in build/fuzz/
# from the checked-in seeds in fuzz/seeds/. `fuzz` runs each for FUZZ_SECS
# (part of `run`); `fuzz-long` runs them all at once for FUZZ_LONG_SECS (10
# min), through `heavy` where it exists. A crash leaves its input in
# build/fuzz/crash-NAME-*; replay it with build/fuzz/NAME FILE. No clang with
# libFuzzer: skipped, loudly; FUZZ_REQUIRED=1 makes that a failure.
FUZZ := $(BIN)/fuzz
FUZZERS := fuzz_api fuzz_cli fuzz_json fuzz_remote fuzz_ws
FUZZ_SECS ?= 10
FUZZ_LONG_SECS ?= 600
CLANG ?= $(firstword $(wildcard $(HOME)/.local/opt/LLVM-*/bin/clang) $(shell command -v clang 2>/dev/null))
FUZZ_OK := $(if $(CLANG),$(shell printf 'int LLVMFuzzerTestOneInput(const char*d,long n){return 0;}' | \
	$(CLANG) -x c - -fsanitize=fuzzer -o /dev/null 2>/dev/null && echo 1))
FUZZ_CFLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-unused-function -I$(CORE) -Ifuzz \
	-fsanitize=fuzzer,address,undefined,float-cast-overflow -fno-sanitize-recover=all
FUZZ_RUN = $(FUZZ)/$(1) -dict=$(FUZZ)/hifi.dict -artifact_prefix=$(FUZZ)/crash-$(1)- \
	$(FUZZ)/corpus/$(1) fuzz/seeds/$(1)

$(FUZZ)/%: fuzz/%.c fuzz/fuzz_board.h $(SRCS) $(wildcard $(CORE)/*.h)
	@mkdir -p $(FUZZ)
	$(CLANG) $(FUZZ_CFLAGS) -o $@ $< $(SRCS) -lm

$(FUZZ)/hifi.dict: fuzz/dict.py $(SRCS)
	@mkdir -p $(FUZZ)
	python3 fuzz/dict.py $(SRCS) > $@

ifeq ($(FUZZ_OK),1)
fuzz-build: $(addprefix $(FUZZ)/,$(FUZZERS)) $(FUZZ)/hifi.dict
fuzz: fuzz-build
	@fail=0; for f in $(FUZZERS); do $(MAKE) --no-print-directory fuzz-one-$$f SECS=$(FUZZ_SECS) || fail=1; \
	done; exit $$fail
fuzz-long: fuzz-build
	@H=$$(command -v heavy >/dev/null && echo "heavy timeout $$(( $(FUZZ_LONG_SECS) + 900 ))"); \
	$$H $(MAKE) --no-print-directory -k -j$(words $(FUZZERS)) $(addprefix fuzz-one-,$(FUZZERS)) \
	  SECS=$(FUZZ_LONG_SECS)
fuzz-one-%:
	@mkdir -p $(FUZZ)/corpus/$* fuzz/seeds/$*
	@$(call FUZZ_RUN,$*) -max_total_time=$(SECS) -rss_limit_mb=1024 > $(FUZZ)/$*-$(SECS)s.log 2>&1 \
	  && echo "fuzz $* ($(SECS) s): $$(grep -o 'Done [0-9]* runs' $(FUZZ)/$*-$(SECS)s.log), no crash" \
	  || { echo "fuzz $*: FAILED (log $(FUZZ)/$*-$(SECS)s.log)"; grep -A30 '^==[0-9]*==ERROR\|runtime error\|FUZZ_ASSERT' \
	    $(FUZZ)/$*-$(SECS)s.log | head -50; exit 1; }
else
fuzz fuzz-long fuzz-build:
	@echo "SKIPPED fuzzing: no clang with libFuzzer (set CLANG)"; exit $(if $(filter-out 0,$(FUZZ_REQUIRED)),1,0)
endif

# The real Lite image booted in QEMU (`fw qemu`, from the hive's fw module),
# with the faults of fw.json qemu.scenarios: the self-test (part of `run`),
# and with `qemu-faults` a brown-out, a lost PD contract, an I2S stall, a full
# settings flash and a heap squeeze. Each boots its own image; logs go to
# the QEMU build dir's fw-qemu-NAME.log. They need the QEMU build, which
# takes a while: `fw build .. --qemu` (with HIFI_HIVE_PUBKEY set, through
# heavy). With no fw or no QEMU build: skipped, loudly; QEMU_REQUIRED=1
# makes that a failure.
FW ?= $(shell command -v fw 2>/dev/null)
QEMU_SCENARIOS ?= selftest
QEMU_BUILD := $(wildcard ../.fw-src/build-qemu/flasher_args.json)
qemu:
	@$(MAKE) --no-print-directory qemu-run SCEN="$(QEMU_SCENARIOS)"
qemu-faults:
	@$(MAKE) --no-print-directory qemu-run SCEN=all
qemu-run:
	@if [ -z "$(FW)" ] || [ -z "$(QEMU_BUILD)" ]; then \
	  echo "SKIPPED QEMU scenarios: $(if $(FW),no QEMU build (fw build .. --qemu),no fw on PATH)"; \
	  exit $(if $(filter-out 0,$(QEMU_REQUIRED)),1,0); fi; \
	if [ -n "$$(find ../components ../patches ../fw.json -newer "$(QEMU_BUILD)" -type f -print -quit)" ]; then \
	  echo "WARNING: the QEMU build is older than the sources: rebuild with fw build .. --qemu"; fi; \
	H=$$(command -v heavy >/dev/null && echo heavy); \
	$$H timeout 3000 "$(FW)" qemu .. $(foreach s,$(SCEN),--scenario '$(s)')

clean:
	rm -rf $(BIN)

.PHONY: all run clean coverage upstream fuzz fuzz-long fuzz-build qemu qemu-faults qemu-run
