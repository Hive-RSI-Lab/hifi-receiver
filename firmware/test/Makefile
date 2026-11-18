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
TESTS := test_json_pd test_power test_gov_tone test_boost test_vol_ir_meta test_api test_remote test_core test_line test_tap test_gate test_listen test_diag test_lab test_scan test_party test_party_group test_quiet test_source sim_party

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
	@fail=0; for t in $(abspath $^); do $$t || fail=1; done; python3 test_otadata.py || fail=1; python3 test_quiet_paths.py || fail=1; TEST_REMOTE=$(abspath $(BIN))/test_remote python3 test_sign_fw.py || fail=1; \
	  $(MAKE) --no-print-directory upstream || fail=1; \
	  $(MAKE) --no-print-directory mad || fail=1; \
	  $(MAKE) --no-print-directory fuzz || fail=1; \
	  $(MAKE) --no-print-directory qemu || fail=1; exit $$fail

# Regressions for the bring-up bugs fixed in upstream's code (patches/):
# upstream/upstream.py fetches the pinned files once (from $$HIFI_UPSTREAM or
# GitHub), applies our patches, and pulls the patched functions out, which
# the tests build against stubs. With no copy of upstream reachable they are
# skipped, loudly; UPSTREAM_REQUIRED=1 makes that a failure.
UP := $(BIN)/upstream
CSPOT := components/spotify/cspot
BELL := $(CSPOT)/bell/main
NANOPB := $(CSPOT)/bell/external/nanopb
UP_FILES = components/squeezelite/controls.c components/squeezelite/decode_external.c components/driver_bt/bt_app_sink.c \
	components/driver_bt/bt_app_core.c components/spotify/Shim.cpp components/spotify/cspot/src/LoginBlob.cpp \
	components/spotify/cspot_private.h components/spotify/cspot_sink.h $(CSPOT)/include/LoginBlob.h \
	$(CSPOT)/include/ConstantParameters.h $(CSPOT)/bell/external/cJSON/cJSON.c $(CSPOT)/bell/external/cJSON/cJSON.h \
	components/wifi-manager/network_manager_handlers.c components/raop/dmap_parser.c components/raop/dmap_parser.h main/esp_app_main.c \
	components/raop/rtp.c components/codecs/inc/mad/mad.h $(ALAC_FILES) \
	$(BELL)/io/TLSSocket.cpp $(BELL)/io/include/TLSSocket.h $(BELL)/io/include/BellSocket.h \
	$(BELL)/io/X509Bundle.cpp $(BELL)/io/include/X509Bundle.h \
	$(BELL)/utilities/BellLogger.cpp $(BELL)/utilities/include/BellLogger.h \
	$(CSPOT)/src/AuthChallenges.cpp $(CSPOT)/include/AuthChallenges.h \
	$(addprefix $(CSPOT)/,src/PlainConnection.cpp src/ShannonConnection.cpp src/Shannon.cpp include/PlainConnection.h \
	  include/ShannonConnection.h include/Shannon.h include/Packet.h include/Logger.h include/Utils.h) \
	$(CSPOT)/protobuf/keyexchange.proto $(CSPOT)/protobuf/keyexchange.options \
	$(CSPOT)/protobuf/authentication.proto $(CSPOT)/protobuf/authentication.options \
	$(BELL)/utilities/Crypto.cpp $(BELL)/utilities/include/Crypto.h $(BELL)/utilities/aes.c \
	$(BELL)/utilities/include/aes.h $(BELL)/utilities/NanoPBHelper.cpp $(BELL)/utilities/include/NanoPBHelper.h \
	$(addprefix $(NANOPB)/,pb.h pb_common.c pb_common.h pb_decode.c pb_decode.h pb_encode.c pb_encode.h \
	  generator/nanopb_generator.py generator/proto/__init__.py generator/proto/_utils.py \
	  generator/proto/nanopb.proto generator/proto/google/protobuf/descriptor.proto)
# The ALAC decoder as the board builds it (patch 0021), and Apple's encoder to make real packets.
# Address sanitizer only: Apple's code shifts signed values all over, which UBSan would stop on.
ALAC_DIR := components/spotify/cspot/bell/external/alac/codec
ALAC_C := ALACBitUtilities.c ag_dec.c dp_dec.c matrix_dec.c EndianPortable.c ag_enc.c dp_enc.c matrix_enc.c
ALAC_CPP := ALACDecoder.cpp ALACEncoder.cpp
ALAC_FILES := $(addprefix $(ALAC_DIR)/,$(ALAC_C) $(ALAC_CPP) ALACDecoder.h ALACEncoder.h ALACBitUtilities.h \
	ALACAudioTypes.h EndianPortable.h aglib.h dplib.h matrixlib.h) \
	components/codecs/alac_wrapper.cpp components/codecs/inc/alac/alac_wrapper.h
ALAC_FLAGS := -g -O1 -w -fsanitize=address -DTARGET_RT_LITTLE_ENDIAN=1 -I$(abspath $(UP))/$(ALAC_DIR) -I$(abspath $(UP))/components/codecs/inc/alac
ALAC_SRCS = $(addprefix $(UP)/$(ALAC_DIR)/,$(ALAC_CPP)) $(UP)/components/codecs/alac_wrapper.cpp
UP_CFLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-unused-function -Wno-unused-parameter \
	-Wno-unused-variable -Wno-unused-const-variable -fsanitize=address,undefined -fno-sanitize-recover=all -I$(UP)/gen
upstream:
	@mkdir -p $(UP)/gen; python3 upstream/upstream.py files $(UP) $(UP_FILES); rc=$$?; \
	if [ $$rc = 3 ]; then echo "SKIPPED upstream regressions: no copy of upstream (set HIFI_UPSTREAM)"; \
	  exit $(if $(filter-out 0,$(UPSTREAM_REQUIRED)),1,0); fi; [ $$rc = 0 ] || exit 1; \
	python3 upstream/upstream.py extract $(UP)/components/squeezelite/controls.c \
	  connect_cli_socket cli_send_cmd > $(UP)/gen/controls_fns.c && \
	python3 upstream/upstream.py extract $(UP)/components/driver_bt/bt_app_sink.c \
	  IDLE_RELEASE_S IDLE_CHECK_S idle_check LET_GO_MS HOLD_NO_RN_MS HOLD_MAX_MS let_go_check hold_cancel \
	  bt_sink_let_go bt_disconnect bt_sink_take hold_expired hold_check bt_av_hdl_a2d_evt \
	  bt_av_notify_evt_handler > $(UP)/gen/bt_fns.c && \
	python3 upstream/upstream.py extract $(UP)/components/raop/rtp.c \
	  rtp_take_from rtp_check_stalled > $(UP)/gen/rtp_fns.c && \
	python3 upstream/upstream.py extract $(UP)/components/squeezelite/decode_external.c \
	  source_decide phone_takeover sink_data_handler cspot_volume_apply cspot_cmd_handler \
	  decode_phones_yield > $(UP)/gen/ext_fns.c && \
	$(CC) $(UP_CFLAGS) -o $(UP)/test_up_controls upstream/test_up_controls.c && \
	$(CC) $(UP_CFLAGS) -o $(UP)/test_up_bt upstream/test_up_bt.c && \
	$(CC) $(UP_CFLAGS) -I$(UP)/components/raop -o $(UP)/test_up_dmap upstream/test_up_dmap.c && \
	$(CC) $(UP_CFLAGS) -o $(UP)/test_up_rtp upstream/test_up_rtp.c && \
	$(CC) $(UP_CFLAGS) -o $(UP)/test_up_ext upstream/test_up_ext.c -lm || exit 1; \
	mkdir -p $(UP)/alac && (cd $(UP)/alac && $(CC) $(ALAC_FLAGS) -c $(addprefix $(abspath $(UP))/$(ALAC_DIR)/,$(ALAC_C))) && \
	$(CXX) $(ALAC_FLAGS) -o $(UP)/test_up_alac upstream/test_up_alac.cpp $(ALAC_SRCS) $(UP)/alac/*.o || exit 1; \
	fail=0; $(UP)/test_up_controls || fail=1; $(UP)/test_up_bt || fail=1; $(UP)/test_up_dmap || fail=1; \
	$(UP)/test_up_rtp || fail=1; $(UP)/test_up_ext || fail=1; \
	$(UP)/test_up_alac || fail=1; \
	$(CC) -std=gnu11 -w -DPUBLIC -I$(UP)/components/codecs/inc/mad -o $(UP)/mad_layout_h mad/layout.c && \
	$(CC) -std=gnu11 -w -include mad/host_config.h -I$(MAD_SRC) -o $(UP)/mad_layout_lib mad/layout.c && \
	$(UP)/mad_layout_h > $(UP)/mad_layout_h.txt && $(UP)/mad_layout_lib > $(UP)/mad_layout_lib.txt && \
	diff $(UP)/mad_layout_h.txt $(UP)/mad_layout_lib.txt && \
	echo "mad_layout   libmad's structs match codecs/inc/mad/mad.h ($$(wc -l < $(UP)/mad_layout_h.txt) sizes and offsets)" || \
	{ echo "FAIL mad_layout: third_party/libmad's structs differ from components/codecs/inc/mad/mad.h"; fail=1; }; \
	if [ "$(FUZZ_OK)" = 1 ]; then $(MAKE) --no-print-directory fuzz-alac || fail=1; \
	else echo "SKIPPED fuzz_alac: no clang with libFuzzer"; fi; \
	python3 upstream/test_up_source.py $(UP) || fail=1; \
	python3 upstream/test_up_tls.py $(UP) || fail=1; python3 upstream/test_up_aphello.py $(UP) || fail=1; \
	python3 upstream/test_up_shannon.py $(UP) || fail=1; python3 upstream/test_up_blob.py $(UP) || fail=1; exit $$fail

# libmad as the board builds it (patch 0029: third_party/libmad, with
# Debian's md_size and length-check fixes), with AddressSanitizer: the
# crafted frames in mad/test_mad.c against that source (asserts on, as on
# the board), and against a copy with those two patches taken back out
# (stock 0.15.1b with the board's inline buffers; NDEBUG, so the overflow
# shows rather than an assert), which must overflow, so the test is known to
# catch it. Both must decode mad/sine.mp3 (a real LAME file that uses the
# bit reservoir) to the same PCM: the fix changes no samples. With FUZZ_OK,
# fuzz-mad then runs the decoder under libFuzzer for FUZZ_SECS.
MAD_SRC := ../third_party/libmad/src
MAD_PATCHES := ../third_party/libmad/patches
MAD_C := bit.c fixed.c frame.c huffman.c layer12.c layer3.c stream.c synth.c timer.c version.c
MAD := $(BIN)/mad
MAD_FLAGS := -std=gnu99 -O1 -g -w -fno-omit-frame-pointer -include $(abspath mad/host_config.h)
MAD_ASAN := ASAN_OPTIONS=detect_leaks=1:abort_on_error=0
mad:
	@rm -rf $(MAD) && mkdir -p $(MAD)/before && cp $(MAD_SRC)/*.c $(MAD_SRC)/*.h $(MAD_SRC)/*.dat $(MAD)/before/ && \
	rm -f $(MAD)/before/config.h && \
	patch -s -R -p1 --no-backup-if-mismatch -d $(MAD)/before < $(MAD_PATCHES)/03-length-check.patch && \
	patch -s -R -p1 --no-backup-if-mismatch -d $(MAD)/before < $(MAD_PATCHES)/02-md_size.patch && \
	$(CC) $(MAD_FLAGS) -fsanitize=address -I$(MAD_SRC) -o $(MAD)/after mad/test_mad.c \
	  $(addprefix $(MAD_SRC)/,$(MAD_C)) && \
	$(CC) $(MAD_FLAGS) -fsanitize=address -DNDEBUG -I$(MAD)/before -o $(MAD)/stock mad/test_mad.c \
	  $(addprefix $(MAD)/before/,$(MAD_C)) || exit 1; \
	fail=0; $(MAD_ASAN) $(MAD)/after || fail=1; \
	for c in "511 511" "0 511 0 0 0 0"; do \
	  if $(MAD_ASAN) $(MAD)/stock crafted $$c > $(MAD)/stock.log 2>&1 || \
	     ! grep -q 'heap-buffer-overflow' $(MAD)/stock.log; then \
	    echo "test_mad: stock libmad should overflow on crafted $$c, but:"; tail -20 $(MAD)/stock.log; fail=1; fi; \
	done; \
	a=$$($(MAD_ASAN) $(MAD)/after pcm mad/sine.mp3) && s=$$($(MAD_ASAN) $(MAD)/stock pcm mad/sine.mp3) || fail=1; \
	case "$$a" in "frames 0 "*|"") echo "test_mad: sine.mp3 didn't decode: $$a"; fail=1;; esac; \
	case "$$a" in *" errors 0 "*) ;; *) echo "test_mad: sine.mp3 decoded with errors: $$a"; fail=1;; esac; \
	[ "$$a" = "$$s" ] || { echo "test_mad: sine.mp3 decodes differently: fixed $$a, stock $$s"; fail=1; }; \
	[ $$fail = 0 ] && echo "mad          stock overflows on the crafted frames, fixed refuses them; sine.mp3 $$a"; \
	if [ "$(FUZZ_OK)" = 1 ]; then $(MAKE) --no-print-directory fuzz-mad || fail=1; \
	else echo "SKIPPED fuzz_mad: no clang with libFuzzer"; fi; exit $$fail
fuzz-mad:
	@mkdir -p $(FUZZ)/corpus/fuzz_mad && \
	$(CLANG) $(MAD_FLAGS) -fsanitize=fuzzer,address -DMAD_FUZZ -I$(MAD_SRC) -o $(FUZZ)/fuzz_mad mad/test_mad.c \
	  $(addprefix $(MAD_SRC)/,$(MAD_C)) && \
	$(FUZZ)/fuzz_mad -max_total_time=$(FUZZ_SECS) -max_len=8192 -rss_limit_mb=1024 \
	  -artifact_prefix=$(FUZZ)/crash-fuzz_mad- $(FUZZ)/corpus/fuzz_mad fuzz/seeds/fuzz_mad 2>$(FUZZ)/fuzz_mad.log && \
	echo "fuzz_mad     $$(grep -o 'Done [0-9]* runs' $(FUZZ)/fuzz_mad.log | tail -1), no crash" || \
	{ tail -40 $(FUZZ)/fuzz_mad.log; echo "FAIL fuzz_mad: see $(FUZZ)/crash-fuzz_mad-*"; exit 1; }

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

# The ALAC decoder fuzzer (test_up_alac.cpp built with ALAC_FUZZ), on the files `upstream` fetched.
comma := ,
ALAC_FUZZ_FLAGS = $(subst -fsanitize=address,-fsanitize=fuzzer$(comma)address,$(ALAC_FLAGS)) -DALAC_FUZZ
fuzz-alac:
	@mkdir -p $(FUZZ)/alac $(FUZZ)/corpus/fuzz_alac && \
	(cd $(FUZZ)/alac && $(CLANG) $(ALAC_FUZZ_FLAGS) -c $(addprefix $(abspath $(UP))/$(ALAC_DIR)/,$(ALAC_C))) && \
	$(CLANG)++ $(ALAC_FUZZ_FLAGS) -o $(FUZZ)/fuzz_alac upstream/test_up_alac.cpp $(ALAC_SRCS) $(FUZZ)/alac/*.o && \
	$(FUZZ)/fuzz_alac -max_total_time=$(FUZZ_SECS) -max_len=4096 -artifact_prefix=$(FUZZ)/crash-fuzz_alac- \
	  $(FUZZ)/corpus/fuzz_alac fuzz/seeds/fuzz_alac 2>$(FUZZ)/fuzz_alac.log && \
	echo "fuzz_alac    $$(grep -o 'Done [0-9]* runs' $(FUZZ)/fuzz_alac.log | tail -1), no crash" || \
	{ tail -40 $(FUZZ)/fuzz_alac.log; echo "FAIL fuzz_alac: see $(FUZZ)/crash-fuzz_alac-*"; exit 1; }

# libFuzzer harnesses (fuzz/): the console, the HTTP test API, the JSON
# reader and writer, the relay's signed envelope, its websocket client, and
# what the lab's radios receive (the ESP-NOW and BLE link frames).
# Each is built with clang, ASan and UBSan; its corpus grows in build/fuzz/
# from the checked-in seeds in fuzz/seeds/. `fuzz` runs each for FUZZ_SECS
# (part of `run`); `fuzz-long` runs them all at once for FUZZ_LONG_SECS (10
# min), through `heavy` where it exists. A crash leaves its input in
# build/fuzz/crash-NAME-*; replay it with build/fuzz/NAME FILE. No clang with
# libFuzzer: skipped, loudly; FUZZ_REQUIRED=1 makes that a failure.
FUZZ := $(BIN)/fuzz
FUZZERS := fuzz_api fuzz_ble fuzz_cli fuzz_espnow fuzz_json fuzz_remote fuzz_ws
FUZZ_SECS ?= 10
FUZZ_LONG_SECS ?= 600
CLANG ?= $(firstword $(wildcard $(HOME)/.local/opt/LLVM-*/bin/clang) $(shell command -v clang 2>/dev/null))
FUZZ_OK := $(if $(CLANG),$(shell printf 'int LLVMFuzzerTestOneInput(const char*d,long n){return 0;}' | \
	$(CLANG) -x c - -fsanitize=fuzzer -o /dev/null 2>/dev/null && echo 1))
FUZZ_CFLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-unused-function -I$(CORE) -Ifuzz \
	-fsanitize=fuzzer,address,undefined,float-cast-overflow -fno-sanitize-recover=all
FUZZ_RUN = $(FUZZ)/$(1) -dict=$(FUZZ)/hifi.dict -artifact_prefix=$(FUZZ)/crash-$(1)- \
	$(FUZZ)/corpus/$(1) fuzz/seeds/$(1)

$(FUZZ)/%: fuzz/%.c fuzz/fuzz_board.h fuzz/fuzz_link.h $(SRCS) $(wildcard $(CORE)/*.h)
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

.PHONY: all run clean coverage upstream mad fuzz-mad fuzz-alac fuzz fuzz-long fuzz-build qemu qemu-faults qemu-run
