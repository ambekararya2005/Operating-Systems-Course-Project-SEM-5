# ReliefOS build. Binaries go to $(BUILD)/ (default build/).
#   make            build reliefd, camp, reliefstat, schedsim, linuxsched, racelab, rwlab (+ test helpers)
#   make test       tests/{smoke,camps,traffic,signals,status,cli}.sh + phase1_smoke.sh (gate M1)
#                   + tests/sched.sh (build/test_sched unit tests, schedsim)
#                   + tests/dispatch.sh (reliefd rescue-team threads, live scheduling)
#                   + tests/linuxsched.sh (real Linux scheduling classes, watchdog)
#                   + tests/phase2_check.sh (gate M2: every policy consistent and deterministic)
#                   + tests/racelab.sh (critical section: Peterson, mutex, spinlock, semaphore)
#                   + tests/sync.sh (test_sync: dock bounded buffer, rwsync; rwlab) and tests/dock.sh
#                     (reliefd loading dock: produce/consume/deliver, occupancy, report)
#                   + tests/banker.sh (test_banker textbook example; Banker's in reliefd: grants,
#                     releases, Available replay, reliefctl banker, crash release)
#                   + tests/deadlock.sh (deadlock.csv under none/prevent/avoid/detect, waitfor, mix)
#                   + tests/phase3_check.sh (gate M3: critical section, dock, Banker's, zero deadlocks in E3)
#   make tsan       ThreadSanitizer build in build/tsan/ + tests
#   make valgrind   tests with reliefd and camps under valgrind
#   make run        run reliefd in the foreground (RELIEF_HOME=./run)
#   make e1         experiment E1: policy comparison -> experiments/results/e1.csv + PNGs
#                   (figures also copied to docs/figures/e1/ for EXPERIMENTS.md)
#   make e2         experiment E2: real Linux scheduling (CFS/EEVDF+nice, FIFO, RR) vs simulated
#                   prio/mlfq -> experiments/results/e2_*.csv + PNGs (copied to docs/figures/e2/).
#                   fifo/rr need: sudo setcap cap_sys_nice+ep build/linuxsched
#   make e3         experiment E3: deadlock modes compared with build/dlsim (deterministic) ->
#                   experiments/results/e3*.csv + PNGs (copied to docs/figures/e3/)
#   make e4         experiment E4: critical-section protection, every racelab mode x 10 runs ->
#                   experiments/results/e4.csv + PNGs (copied to docs/figures/e4/), and the
#                   strace futex counts in docs/strace_futex.txt
#   make e4b        experiment E4b: readers-writers starvation, rwlab semaphore vs rwlock x 5 runs ->
#                   experiments/results/e4b.csv + summary table

CC       ?= gcc
BUILD    ?= build
SANFLAGS ?=
OPT      ?= -O2

CPPFLAGS := -Iinclude -D_POSIX_C_SOURCE=200809L -MMD -MP
CFLAGS   := -std=c11 -Wall -Wextra -Werror -pthread -g $(OPT) $(SANFLAGS)
LDFLAGS  := -pthread $(SANFLAGS)
LDLIBS   := -lrt -lm

OBJ        := $(BUILD)/obj
COMMON_SRC := src/common/log.c src/common/shm.c src/common/rwsync.c src/common/resconf.c
DEADLOCK_SRC := src/deadlock/banker.c src/deadlock/deadlock.c src/deadlock/waitfor.c
SCHED_POLICY_SRC := src/sched/sched.c src/sched/fcfs.c src/sched/sjf.c src/sched/srtf.c \
                    src/sched/rr.c src/sched/prio.c src/sched/mlfq.c
RELIEFD_SRC := src/reliefd/main.c src/reliefd/camps.c src/reliefd/dispatch.c src/reliefd/dock.c \
               src/reliefd/inventory.c src/reliefd/resmgr.c $(COMMON_SRC) $(DEADLOCK_SRC) \
               $(SCHED_POLICY_SRC)
CAMP_SRC    := src/camp/main.c $(COMMON_SRC)
RELIEFSTAT_SRC := src/reliefstat/main.c src/common/shm.c src/deadlock/banker.c src/deadlock/waitfor.c

RELIEFD_OBJ := $(RELIEFD_SRC:%.c=$(OBJ)/%.o)
CAMP_OBJ    := $(CAMP_SRC:%.c=$(OBJ)/%.o)
RELIEFSTAT_OBJ := $(RELIEFSTAT_SRC:%.c=$(OBJ)/%.o)
# Scheduler library (independent of IPC): policies + discrete-time simulator.
SCHED_SRC    := src/sched/sim.c $(SCHED_POLICY_SRC)
SCHEDSIM_SRC := src/sched/schedsim.c $(SCHED_SRC)
SCHEDSIM_OBJ := $(SCHEDSIM_SRC:%.c=$(OBJ)/%.o)
LINUXSCHED_SRC := src/linuxsched/main.c $(SCHED_SRC)
LINUXSCHED_OBJ := $(LINUXSCHED_SRC:%.c=$(OBJ)/%.o)
RACELAB_SRC := src/racelab/main.c
RACELAB_OBJ := $(RACELAB_SRC:%.c=$(OBJ)/%.o)
DLSIM_SRC := src/dlsim/dlsim.c src/common/resconf.c $(DEADLOCK_SRC)
DLSIM_OBJ := $(DLSIM_SRC:%.c=$(OBJ)/%.o)
RWLAB_SRC := src/rwlab/main.c src/common/rwsync.c
RWLAB_OBJ := $(RWLAB_SRC:%.c=$(OBJ)/%.o)
TEST_BANKER_SRC := tests/test_banker.c src/deadlock/banker.c src/deadlock/deadlock.c src/deadlock/waitfor.c
TEST_BANKER_OBJ := $(TEST_BANKER_SRC:%.c=$(OBJ)/%.o)
# Unit tests of the Phase 3 sync code: the loading dock's bounded buffer and both rwsync kinds.
TEST_SYNC_SRC := tests/test_sync.c src/reliefd/dock.c src/reliefd/inventory.c $(COMMON_SRC)
TEST_SYNC_OBJ := $(TEST_SYNC_SRC:%.c=$(OBJ)/%.o)
TEST_SCHED_SRC := tests/test_sched.c $(SCHED_SRC)
TEST_SCHED_OBJ := $(TEST_SCHED_SRC:%.c=$(OBJ)/%.o)
# Test helpers (built with the programs, used by tests/*.sh).
SHM_DIE_SRC := tests/helpers/shm_die_locked.c src/common/shm.c
SHM_DIE_OBJ := $(SHM_DIE_SRC:%.c=$(OBJ)/%.o)

TESTS := tests/smoke.sh tests/camps.sh tests/traffic.sh tests/signals.sh tests/status.sh tests/cli.sh \
         tests/phase1_smoke.sh tests/sched.sh tests/dispatch.sh tests/linuxsched.sh tests/phase2_check.sh \
         tests/racelab.sh tests/sync.sh tests/dock.sh tests/banker.sh tests/deadlock.sh \
         tests/phase3_check.sh

# --trace-children: camps exec'd by reliefd also run under valgrind.
VALGRIND := valgrind --quiet --error-exitcode=99 --leak-check=full \
            --errors-for-leak-kinds=definite,indirect \
            --trace-children=yes --child-silent-after-fork=yes

# $(call run_tests,ENV...) runs every test script with the given environment.
run_tests = for t in $(TESTS); do echo "== $$t"; $(1) $$t || exit 1; done

.PHONY: all reliefd camp reliefstat schedsim linuxsched racelab rwlab dlsim clean run test tsan valgrind e1 e2 e3 e4 e4b

all: reliefd camp reliefstat schedsim linuxsched racelab rwlab dlsim $(BUILD)/shm_die_locked $(BUILD)/test_sched \
     $(BUILD)/test_sync $(BUILD)/test_banker

reliefd: $(BUILD)/reliefd
camp: $(BUILD)/camp
reliefstat: $(BUILD)/reliefstat
schedsim: $(BUILD)/schedsim
linuxsched: $(BUILD)/linuxsched
racelab: $(BUILD)/racelab
rwlab: $(BUILD)/rwlab
dlsim: $(BUILD)/dlsim

$(BUILD)/reliefd: $(RELIEFD_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/camp: $(CAMP_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/reliefstat: $(RELIEFSTAT_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/shm_die_locked: $(SHM_DIE_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/schedsim: $(SCHEDSIM_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/linuxsched: $(LINUXSCHED_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/racelab: $(RACELAB_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/dlsim: $(DLSIM_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/rwlab: $(RWLAB_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/test_banker: $(TEST_BANKER_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/test_sync: $(TEST_SYNC_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/test_sched: $(TEST_SCHED_OBJ)
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
	$(call run_tests,BIN_DIR=$(BUILD) RELIEF_WRAP="$(VALGRIND)" TEST_TIMEOUT=30 PHASE1_CAMPS=2)

# Experiment E1 (deterministic: same seeds -> identical CSV). Plotting needs matplotlib.
E1_FIGS := e1_wait_by_severity.png e1_critical_response.png e1_starved.png e1_worst_wait.png \
           e1b_starved_vs_load.png e1b_critical_response_vs_load.png
e1: schedsim
	experiments/run_e1.sh
	python3 -I experiments/plot_e1.py
	@mkdir -p docs/figures/e1
	cp $(addprefix experiments/results/,$(E1_FIGS)) docs/figures/e1/

# Experiment E2 (real threads, so Linux timings vary slightly between runs).
E2_FIGS := e2_response.png e2_wait.png
e2: linuxsched schedsim
	experiments/run_e2.sh
	python3 -I experiments/plot_e2.py
	@mkdir -p docs/figures/e2
	cp $(addprefix experiments/results/,$(E2_FIGS)) docs/figures/e2/

# Experiment E3 (deterministic: same seeds -> identical CSV).
E3_FIGS := e3_completed.png e3_delay.png e3_vehicle_time.png e3_timeline_none.png
e3: dlsim
	experiments/run_e3.sh
	python3 -I experiments/plot_e3.py
	@mkdir -p docs/figures/e3
	cp $(addprefix experiments/results/,$(E3_FIGS)) docs/figures/e3/

# Experiment E4 (real threads: the racy modes' errors and all runtimes vary between runs).
E4_FIGS := e4_error.png e4_runtime.png
e4: racelab
	experiments/run_e4.sh
	python3 -I experiments/plot_e4.py
	@mkdir -p docs/figures/e4
	cp $(addprefix experiments/results/,$(E4_FIGS)) docs/figures/e4/

# Experiment E4b (real threads; a table, no plot).
e4b: rwlab
	experiments/run_e4b.sh

clean:
	rm -rf build

-include $(RELIEFD_OBJ:.o=.d) $(CAMP_OBJ:.o=.d) $(RELIEFSTAT_OBJ:.o=.d) $(SHM_DIE_OBJ:.o=.d) \
         $(SCHEDSIM_OBJ:.o=.d) $(TEST_SCHED_OBJ:.o=.d) $(LINUXSCHED_OBJ:.o=.d) \
         $(RACELAB_OBJ:.o=.d) $(RWLAB_OBJ:.o=.d) $(DLSIM_OBJ:.o=.d) $(TEST_SYNC_OBJ:.o=.d) $(TEST_BANKER_OBJ:.o=.d)
