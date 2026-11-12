CXX ?= clang++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror
CPPFLAGS ?=
CPPFLAGS += -Iinclude

ifeq ($(shell uname -s),Darwin)
MACOS_SDK := $(shell xcrun --show-sdk-path)
CPPFLAGS += -isysroot $(MACOS_SDK) -isystem $(MACOS_SDK)/usr/include/c++/v1
LDFLAGS += -isysroot $(MACOS_SDK)
endif

BUILD_DIR := build
LIB_OBJECT := $(BUILD_DIR)/lsm.o
TOOL := $(BUILD_DIR)/lsm_tool
TEST := $(BUILD_DIR)/test_lsm

ifeq ($(shell uname -s),Darwin)
SHARED_EXT := dylib
else
SHARED_EXT := so
endif
PIC_DIR := $(BUILD_DIR)/pic
SHARED_LIB := $(BUILD_DIR)/liblsm.$(SHARED_EXT)

.PHONY: all shared test crash-test powercut-test benchmark java-test server smoke-test clean

all: $(TOOL)

shared: $(SHARED_LIB)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(PIC_DIR):
	mkdir -p $(PIC_DIR)

$(LIB_OBJECT): src/lsm.cpp include/lsm/lsm.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(PIC_DIR)/lsm.o: src/lsm.cpp include/lsm/lsm.h | $(PIC_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -fPIC -c $< -o $@

$(PIC_DIR)/lsm_c.o: bindings/lsm_c.cpp bindings/lsm_c.h include/lsm/lsm.h | $(PIC_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -fPIC -c $< -o $@

$(SHARED_LIB): $(PIC_DIR)/lsm.o $(PIC_DIR)/lsm_c.o
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -shared $^ -o $@

$(TOOL): src/main.cpp $(LIB_OBJECT)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS) $^ -o $@

$(TEST): tests/test_lsm.cpp $(LIB_OBJECT)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS) $^ -o $@

test: $(TEST)
	./$(TEST)

crash-test: $(TOOL)
	python3 scripts/crash_torture.py --binary ./$(TOOL) \
		--rounds $${CRASH_ROUNDS:-100}

benchmark: $(TOOL)
	./$(TOOL) benchmark $(BUILD_DIR)/benchmark.db 20000

POWERCUT_LIB := $(BUILD_DIR)/powercut.so
POWERCUT_SNAPSHOT := $(BUILD_DIR)/powercut_snapshot

$(POWERCUT_LIB): scripts/powercut/powercut.c | $(BUILD_DIR)
	$(CC) -O2 -Wall -Wextra -Werror -shared -fPIC $< -o $@ -ldl

$(POWERCUT_SNAPSHOT): scripts/powercut/powercut.c | $(BUILD_DIR)
	$(CC) -O2 -Wall -Wextra -Werror -DPOWERCUT_MAIN $< -o $@

# Linux only: simulated power loss via an LD_PRELOAD fsync shim.
powercut-test: $(TOOL) $(POWERCUT_LIB) $(POWERCUT_SNAPSHOT)
	python3 scripts/powercut_torture.py --binary ./$(TOOL) \
		--preload ./$(POWERCUT_LIB) --snapshot ./$(POWERCUT_SNAPSHOT) \
		--rounds $${POWERCUT_ROUNDS:-100}

java-test: $(SHARED_LIB)
	cd server && mvn -B -q verify

server: $(SHARED_LIB)
	cd server && mvn -B -q spring-boot:run

smoke-test:
	docker build -t lsm-tree-server .
	bash scripts/smoke_test.sh lsm-tree-server

clean:
	rm -rf $(BUILD_DIR) server/target
