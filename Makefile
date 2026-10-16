# ReliefOS build. Binaries go to $(BUILD)/ (default build/).
#   make            build reliefd and camp
#   make test       tests/{smoke,camps,traffic,signals}.sh: IPC, supervision, paced traffic, signals
#   make tsan       ThreadSanitizer build in build/tsan/ + tests
#   make valgrind   tests with reliefd and camps under valgrind
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
RELIEFD_SRC := src/reliefd/main.c src/reliefd/camps.c $(COMMON_SRC)
CAMP_SRC    := src/camp/main.c $(COMMON_SRC)

RELIEFD_OBJ := $(RELIEFD_SRC:%.c=$(OBJ)/%.o)
CAMP_OBJ    := $(CAMP_SRC:%.c=$(OBJ)/%.o)

TESTS := tests/smoke.sh tests/camps.sh tests/traffic.sh tests/signals.sh

# --trace-children: camps exec'd by reliefd also run under valgrind.
VALGRIND := valgrind --quiet --error-exitcode=99 --leak-check=full \
            --errors-for-leak-kinds=definite,indirect \
            --trace-children=yes --child-silent-after-fork=yes

# $(call run_tests,ENV...) runs every test script with the given environment.
run_tests = for t in $(TESTS); do echo "== $$t"; $(1) $$t || exit 1; done

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
	$(call run_tests,BIN_DIR=$(BUILD))

# setarch -R disables ASLR for the test processes only: GCC 13's TSan aborts with
# "unexpected memory mapping" under the high mmap entropy of recent (WSL2) kernels.
tsan:
	$(MAKE) BUILD=build/tsan SANFLAGS=-fsanitize=thread OPT=-O1 all
	$(call run_tests,BIN_DIR=build/tsan RELIEF_WRAP="setarch $$(uname -m) -R")

valgrind: all
	$(call run_tests,BIN_DIR=$(BUILD) RELIEF_WRAP="$(VALGRIND)" TEST_TIMEOUT=30)

clean:
	rm -rf build

-include $(RELIEFD_OBJ:.o=.d) $(CAMP_OBJ:.o=.d)
