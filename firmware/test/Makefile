# Host unit tests for the firmware's portable core (components/hifi/core).
#   make -C firmware/test        builds and runs them all
# Plain C11 with gcc; AddressSanitizer and UBSan catch memory and UB bugs
# that would only show on the board as a crash.

CORE := ../components/hifi/core
CC ?= cc
CFLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-unused-function -I$(CORE) \
	  -fsanitize=address,undefined -fno-sanitize-recover=all
LDLIBS := -lm

SRCS := $(wildcard $(CORE)/*.c)
BIN := build
TESTS := test_json_pd test_power test_gov_tone test_vol_ir_meta test_api test_remote test_core

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
	@fail=0; for t in $(abspath $^); do $$t || fail=1; done; exit $$fail

clean:
	rm -rf $(BIN)

.PHONY: all run clean
