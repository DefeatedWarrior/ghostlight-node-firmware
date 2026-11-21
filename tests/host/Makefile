# Host-side unit tests and libFuzzer targets for the firmware components.
# Plain C, no ESP-IDF needed: stubs/ stands in for the ESP-IDF headers and
# fakes.c for the functions behind them. See docs/DEVELOPING.md ("Tests").
#
#   make test                 unit tests + fuzz seeds replayed (ASan + UBSan)
#   make fuzz                 build the libFuzzer targets (needs LLVM clang)
#   make fuzz-run FUZZ_TIME=60   run each fuzzer for FUZZ_TIME seconds
#   make seeds                regenerate the binary seed corpus
#   make clean

ROOT     := ../..
COMP     := $(ROOT)/components
BUILD    := build

CC       ?= cc
# libFuzzer is not in Apple clang: use Homebrew LLVM when it is there
FUZZ_CC  ?= $(firstword $(wildcard /opt/homebrew/opt/llvm/bin/clang /usr/local/opt/llvm/bin/clang) clang)
FUZZ_TIME ?= 60

INCLUDES := -Istubs -I. -Ithird_party/cJSON \
            -I$(COMP)/board/include \
            -I$(COMP)/config_manager/include \
            -I$(COMP)/storage_manager/include \
            -I$(COMP)/merge_engine/include \
            -I$(COMP)/artnet_receiver/include \
            -I$(COMP)/sacn_receiver/include

WARN     := -Wall -Wextra -Werror -Wno-unused-parameter -Wno-deprecated-declarations
SAN      := -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
CFLAGS_T := -std=gnu11 -g -O1 $(WARN) $(SAN) $(INCLUDES)
CFLAGS_F := -std=gnu11 -g -O1 $(WARN) -fno-omit-frame-pointer $(INCLUDES)

CJSON    := third_party/cJSON/cJSON.c
FAKES    := fakes.c
CONFIG_C := $(COMP)/config_manager/config_manager.c
MERGE_C  := $(COMP)/merge_engine/merge_engine.c

TESTS    := test_artnet test_sacn test_merge test_config
FUZZERS  := fuzz_artnet fuzz_sacn fuzz_config
HDRS     := $(wildcard stubs/*.h stubs/*/*.h) fakes.h test.h packets.h

.PHONY: all test fuzz fuzz-run seeds clean
all: test

# ---- unit tests -------------------------------------------------------------

$(BUILD)/test_artnet: test_artnet.c $(COMP)/artnet_receiver/artnet_receiver.c $(FAKES) $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS_T) -o $@ test_artnet.c $(FAKES)

$(BUILD)/test_sacn: test_sacn.c $(COMP)/sacn_receiver/sacn_receiver.c $(FAKES) $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS_T) -o $@ test_sacn.c $(FAKES)

$(BUILD)/test_merge: test_merge.c $(MERGE_C) $(FAKES) $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS_T) -o $@ test_merge.c $(MERGE_C) $(FAKES)

$(BUILD)/test_config: test_config.c $(CONFIG_C) $(CJSON) $(FAKES) $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS_T) -o $@ test_config.c $(CONFIG_C) $(CJSON) $(FAKES)

# The fuzz harnesses with a plain main() that runs every file given to it:
# replays the seed corpus under the unit-test compiler (no libFuzzer needed)
$(BUILD)/replay_artnet: fuzz/fuzz_artnet.c fuzz/replay_main.c $(COMP)/artnet_receiver/artnet_receiver.c $(FAKES) $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS_T) -o $@ fuzz/fuzz_artnet.c fuzz/replay_main.c $(FAKES)

$(BUILD)/replay_sacn: fuzz/fuzz_sacn.c fuzz/replay_main.c $(COMP)/sacn_receiver/sacn_receiver.c $(FAKES) $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS_T) -o $@ fuzz/fuzz_sacn.c fuzz/replay_main.c $(FAKES)

$(BUILD)/replay_config: fuzz/fuzz_config.c fuzz/replay_main.c $(CONFIG_C) $(CJSON) $(FAKES) $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS_T) -o $@ fuzz/fuzz_config.c fuzz/replay_main.c $(CONFIG_C) $(CJSON) $(FAKES)

test: $(addprefix $(BUILD)/,$(TESTS)) $(addprefix $(BUILD)/replay_,artnet sacn config)
	@set -e; for t in $(TESTS); do echo "== $$t"; ./$(BUILD)/$$t; done
	@set -e; for f in artnet sacn config; do \
	    echo "== replay fuzz/seeds/$$f"; ./$(BUILD)/replay_$$f fuzz/seeds/$$f/*; done
	@echo "All host tests passed."

# ---- fuzzers ----------------------------------------------------------------

$(BUILD)/fuzz_artnet: fuzz/fuzz_artnet.c $(COMP)/artnet_receiver/artnet_receiver.c $(FAKES) $(HDRS) | $(BUILD)
	$(FUZZ_CC) $(CFLAGS_F) -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all -o $@ fuzz/fuzz_artnet.c $(FAKES)

$(BUILD)/fuzz_sacn: fuzz/fuzz_sacn.c $(COMP)/sacn_receiver/sacn_receiver.c $(FAKES) $(HDRS) | $(BUILD)
	$(FUZZ_CC) $(CFLAGS_F) -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all -o $@ fuzz/fuzz_sacn.c $(FAKES)

$(BUILD)/fuzz_config: fuzz/fuzz_config.c $(CONFIG_C) $(CJSON) $(FAKES) $(HDRS) | $(BUILD)
	$(FUZZ_CC) $(CFLAGS_F) -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all -o $@ fuzz/fuzz_config.c $(CONFIG_C) $(CJSON) $(FAKES)

fuzz: $(addprefix $(BUILD)/,$(FUZZERS))

# New corpus entries go to build/corpus/<name>; fuzz/seeds stays read-only.
# Inputs are capped at the firmware's receive buffer (Art-Net 640, sACN 700
# bytes) and the config file limit (8 KiB).
fuzz-run: fuzz
	@set -e; for f in artnet:640 sacn:700 config:8192; do \
	    n=$${f%%:*}; len=$${f##*:}; mkdir -p $(BUILD)/corpus/$$n; \
	    echo "== fuzz_$$n for $(FUZZ_TIME) s"; \
	    ./$(BUILD)/fuzz_$$n -max_total_time=$(FUZZ_TIME) -max_len=$$len -print_final_stats=1 \
	        -artifact_prefix=$(BUILD)/crash-$$n- -dict=fuzz/$$n.dict \
	        $(BUILD)/corpus/$$n fuzz/seeds/$$n; \
	done

# ---- seed corpus --------------------------------------------------------------

$(BUILD)/make_seeds: fuzz/make_seeds.c packets.h | $(BUILD)
	$(CC) -std=gnu11 -Wall -Wextra -Werror -I. -o $@ fuzz/make_seeds.c

seeds: $(BUILD)/make_seeds
	./$(BUILD)/make_seeds fuzz/seeds

$(BUILD):
	mkdir -p $@

clean:
	rm -rf $(BUILD)
