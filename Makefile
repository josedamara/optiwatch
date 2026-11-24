# OptiWatch - optical transceiver manager
#
# Targets:
#   make            build library, daemon, CLI and unit tests
#   make test       run C unit tests + Python integration tests
#   make unit       run C unit tests only
#   make itest      run Python integration tests only
#   make kmod       build the emulated I2C adapter kernel module (needs kernel headers)
#   make asan       rebuild everything with AddressSanitizer/UBSan and run tests
#   make clean

CC      ?= gcc
CXX     ?= g++
PYTHON  ?= python3

BUILD   ?= build
WARN     = -Wall -Wextra -Wshadow -Wformat=2 -Wconversion -Wno-sign-conversion -Werror
CFLAGS  ?= -O2 -g
CXXFLAGS?= -O2 -g
ALL_CFLAGS   = -std=c11 $(WARN) -D_GNU_SOURCE -Iinclude -MMD -MP $(CFLAGS)
ALL_CXXFLAGS = -std=c++17 $(WARN) -D_GNU_SOURCE -Iinclude -MMD -MP $(CXXFLAGS)
LDLIBS  += -lm

LIB_SRC  = $(wildcard lib/*.c)
LIB_OBJ  = $(LIB_SRC:%.c=$(BUILD)/%.o)
LIB      = $(BUILD)/libxcvr.a

DAEMON_SRC = $(wildcard daemon/*.cpp)
DAEMON_OBJ = $(DAEMON_SRC:%.cpp=$(BUILD)/%.o)
DAEMON     = $(BUILD)/optiwatchd

CTL_SRC  = tools/optiwatch.c
CTL      = $(BUILD)/optiwatch

UNIT_SRC = $(wildcard tests/unit/*.c)
UNIT_OBJ = $(UNIT_SRC:%.c=$(BUILD)/%.o)
UNIT     = $(BUILD)/unit_tests

BINS = $(LIB)
ifneq ($(DAEMON_SRC),)
BINS += $(DAEMON)
endif
ifneq ($(wildcard $(CTL_SRC)),)
BINS += $(CTL)
endif
ifneq ($(UNIT_SRC),)
BINS += $(UNIT)
endif

.PHONY: all test unit itest kmod kmod-clean asan clean

all: $(BINS)

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(ALL_CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(ALL_CXXFLAGS) -c $< -o $@

$(LIB): $(LIB_OBJ)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

$(DAEMON): $(DAEMON_OBJ) $(LIB)
	$(CXX) $(ALL_CXXFLAGS) $(LDFLAGS) $^ -o $@ $(LDLIBS)

$(CTL): $(BUILD)/tools/optiwatch.o
	$(CC) $(ALL_CFLAGS) $(LDFLAGS) $^ -o $@

$(UNIT): $(UNIT_OBJ) $(LIB)
	$(CC) $(ALL_CFLAGS) $(LDFLAGS) $^ -o $@ $(LDLIBS)

unit: $(UNIT)
	./$(UNIT)

itest: all
	OPTIWATCH_BUILD=$(abspath $(BUILD)) $(PYTHON) -m unittest discover -s tests/integration -t . -v

test: unit itest

asan:
	$(MAKE) BUILD=build-asan \
	  CFLAGS="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" \
	  CXXFLAGS="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" \
	  LDFLAGS="-fsanitize=address,undefined" test

kmod:
	$(MAKE) -C kernel

kmod-clean:
	$(MAKE) -C kernel clean

clean:
	rm -rf $(BUILD) build-asan

-include $(LIB_OBJ:.o=.d) $(DAEMON_OBJ:.o=.d) $(UNIT_OBJ:.o=.d) $(BUILD)/tools/optiwatch.d
