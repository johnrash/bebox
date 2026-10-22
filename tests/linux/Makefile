# Linux build of the portable emulator core and its tests.
#   make            build everything (ASan + UBSan)
#   make check      build and run the test suites
#   make FAST=1     optimized build without sanitizers (benchmarks)
#   make TSAN=1     ThreadSanitizer build (threaded mode: cpu-unit-tests,
#                   bebox-headless --threads)

ROOT := ../..
CORE := $(ROOT)/src/core
OUT := out

CXX ?= g++
CXXFLAGS := -std=c++17 -Wall -Wextra -Wno-unused-parameter -g -I$(ROOT)/src
ifeq ($(FAST),1)
	CXXFLAGS += -O2 -DNDEBUG
	OUT := out/fast
else ifeq ($(TSAN),1)
	CXXFLAGS += -O1 -fsanitize=thread -fno-omit-frame-pointer
	LDFLAGS += -fsanitize=thread
	OUT := out/tsan
else
	CXXFLAGS += -O1 -fsanitize=address,undefined -fno-omit-frame-pointer
	LDFLAGS += -fsanitize=address,undefined
endif

LDLIBS := -lz

# User mode networking (libslirp), when installed.
ifeq ($(shell pkg-config --exists slirp && echo yes),yes)
	CXXFLAGS += -DHAVE_SLIRP
	LDLIBS += $(shell pkg-config --libs slirp)
endif

CORE_SRCS := $(shell find $(CORE) -name '*.cpp')
CORE_OBJS := $(patsubst $(ROOT)/%.cpp,$(OUT)/obj/%.o,$(CORE_SRCS))

TESTS := ppc-exec cpu-unit-tests cpu-bench qcow2-tests device-tests bfs-tests \
	hobbit-tests
PPC_BINS := $(OUT)/systest.bin $(OUT)/smptest.bin $(OUT)/bench.bin
TEST_BINS := $(addprefix $(OUT)/,$(TESTS))

all: $(TEST_BINS) $(PPC_BINS)

$(OUT)/obj/%.o: $(ROOT)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(OUT)/libcore.a: $(CORE_OBJS)
	@rm -f $@
	ar rcs $@ $^

$(OUT)/%: %.cpp $(OUT)/libcore.a
	$(CXX) $(CXXFLAGS) $< $(OUT)/libcore.a $(LDFLAGS) $(LDLIBS) -o $@

# PowerPC test programs (cross toolchain: cross-powerpc-linux-gnu).
PPC_AS := powerpc-linux-gnu-as -mppc -m603
PPC_LD := powerpc-linux-gnu-ld
PPC_OBJCOPY := powerpc-linux-gnu-objcopy

$(OUT)/%.bin: ppc/%.S
	@mkdir -p $(OUT)
	$(PPC_AS) -o $(OUT)/$*.o $<
	$(PPC_LD) -Ttext=0 -e 0 --no-warn-rwx-segments -o $(OUT)/$*.elf $(OUT)/$*.o
	$(PPC_OBJCOPY) -O binary $(OUT)/$*.elf $@

$(OUT)/bebox-headless: $(ROOT)/src/headless/main.cpp $(OUT)/libcore.a
	$(CXX) $(CXXFLAGS) $< $(OUT)/libcore.a $(LDFLAGS) $(LDLIBS) -o $@

$(OUT)/bebox-img: $(ROOT)/tools/bebox-img/main.cpp $(OUT)/libcore.a
	$(CXX) $(CXXFLAGS) $< $(OUT)/libcore.a $(LDFLAGS) $(LDLIBS) -o $@

all: $(OUT)/bebox-headless $(OUT)/bebox-img

check: all
	./$(OUT)/cpu-unit-tests $(OUT)
	./run-ppc-oracle.sh $(OUT)
	@mkdir -p $(OUT)/qcow2-work
	./$(OUT)/qcow2-tests $(OUT)/qcow2-work
	./$(OUT)/device-tests
	@mkdir -p $(OUT)/bfs-work
	./$(OUT)/bfs-tests $(OUT)/bfs-work
	./$(OUT)/hobbit-tests

clean:
	rm -rf out

-include $(shell find $(OUT)/obj -name '*.d' 2>/dev/null)

.PHONY: all check clean
