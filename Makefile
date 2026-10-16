# ReliefOS build. Binaries go to $(BUILD)/ (default build/).
#   make            build reliefd and camp
#   make test       smoke test (tests/smoke.sh)
#   make tsan       ThreadSanitizer build in build/tsan/ + smoke test
#   make valgrind   smoke test with reliefd and camp under valgrind
#   make run        run reliefd in the foreground (RELIEF_HOME=./run)

CC       ?= gcc
BUILD    ?= build
SANFLAGS ?=
OPT      ?= -O2

CPPFLAGS := -Iinclude -D_POSIX_C_SOURCE=200809L -MMD -MP
CFLAGS   := -std=c11 -Wall -Wextra -Werror -pthread -g $(OPT) $(SANFLAGS)
LDFLAGS  := -pthread $(SANFLAGS)
LDLIBS   := -lrt

OBJ        := $(BUILD)/obj
COMMON_SRC := src/common/log.c
RELIEFD_SRC := src/reliefd/main.c $(COMMON_SRC)
CAMP_SRC    := src/camp/main.c $(COMMON_SRC)

RELIEFD_OBJ := $(RELIEFD_SRC:%.c=$(OBJ)/%.o)
CAMP_OBJ    := $(CAMP_SRC:%.c=$(OBJ)/%.o)

VALGRIND := valgrind --quiet --error-exitcode=99 --leak-check=full \
            --errors-for-leak-kinds=definite,indirect

.PHONY: all reliefd camp clean run test tsan valgrind

all: reliefd camp

reliefd: $(BUILD)/reliefd
camp: $(BUILD)/camp

$(BUILD)/reliefd: $(RELIEFD_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/camp: $(CAMP_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(OBJ)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

run: all
	RELIEF_HOME=./run ./$(BUILD)/reliefd

test: all
	BIN_DIR=$(BUILD) tests/smoke.sh

# setarch -R disables ASLR for the test processes only: GCC 13's TSan aborts with
# "unexpected memory mapping" under the high mmap entropy of recent (WSL2) kernels.
tsan:
	$(MAKE) BUILD=build/tsan SANFLAGS=-fsanitize=thread OPT=-O1 all
	BIN_DIR=build/tsan RELIEF_WRAP="setarch $$(uname -m) -R" tests/smoke.sh

valgrind: all
	BIN_DIR=$(BUILD) RELIEF_WRAP="$(VALGRIND)" tests/smoke.sh

clean:
	rm -rf build

-include $(RELIEFD_OBJ:.o=.d) $(CAMP_OBJ:.o=.d)
