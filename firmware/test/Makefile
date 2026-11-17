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
TESTS := test_json_pd test_power test_gov_tone test_vol_ir_meta test_api test_remote test_core test_line test_tap

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
	  $(MAKE) --no-print-directory upstream || fail=1; exit $$fail

# Regressions for the bring-up bugs fixed in upstream's code (patches/):
# upstream/upstream.py fetches the pinned files once (from $$HIFI_UPSTREAM or
# GitHub), applies our patches, and pulls the patched functions out, which
# the tests build against stubs. With no copy of upstream reachable they are
# skipped, loudly; UPSTREAM_REQUIRED=1 makes that a failure.
UP := $(BIN)/upstream
UP_FILES := components/squeezelite/controls.c components/driver_bt/bt_app_sink.c \
	components/driver_bt/bt_app_core.c components/spotify/Shim.cpp components/spotify/cspot/src/LoginBlob.cpp
UP_CFLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-unused-function -Wno-unused-parameter \
	-Wno-unused-variable -Wno-unused-const-variable -fsanitize=address,undefined -fno-sanitize-recover=all -I$(UP)/gen
upstream:
	@mkdir -p $(UP)/gen; python3 upstream/upstream.py files $(UP) $(UP_FILES); rc=$$?; \
	if [ $$rc = 3 ]; then echo "SKIPPED upstream regressions: no copy of upstream (set HIFI_UPSTREAM)"; \
	  exit $(if $(UPSTREAM_REQUIRED),1,0); fi; [ $$rc = 0 ] || exit 1; \
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

clean:
	rm -rf $(BIN)

.PHONY: all run clean coverage upstream
