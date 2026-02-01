# Makefile for KVStore Persistence Library

CXX = g++
CC = gcc
AR = ar

CXXFLAGS = -std=c++17 -Wall -Wextra -O2 -g -fPIC
CFLAGS = -Wall -Wextra -O2 -g -fPIC
LDFLAGS = -luring -lpthread

# Directories
SRC_DIR = src
INC_DIR = include
BUILD_DIR = build
LIB_DIR = lib

# Source files
CXX_SRCS = $(SRC_DIR)/storage_format.cpp \
           $(SRC_DIR)/log_manager.cpp \
           $(SRC_DIR)/hint_file.cpp \
           $(SRC_DIR)/kvs_persistence.cpp

# Object files
CXX_OBJS = $(CXX_SRCS:$(SRC_DIR)/%.cpp=$(BUILD_DIR)/%.o)

# Library outputs
STATIC_LIB = $(LIB_DIR)/libkvs_persistence.a
SHARED_LIB = $(LIB_DIR)/libkvs_persistence.so

# Include paths
INCLUDES = -I$(INC_DIR)

# Default target
all: dirs $(STATIC_LIB) $(SHARED_LIB)

# Create directories
dirs:
	@mkdir -p $(BUILD_DIR) $(LIB_DIR)

# Static library
$(STATIC_LIB): $(CXX_OBJS)
	$(AR) rcs $@ $^
	@echo "Built static library: $@"

# Shared library
$(SHARED_LIB): $(CXX_OBJS)
	$(CXX) -shared -o $@ $^ $(LDFLAGS)
	@echo "Built shared library: $@"

# Compile C++ sources
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# Test program
TEST_SRC = test/test_persistence.cpp
TEST_BIN = test_persistence

test: all $(TEST_BIN)
	./$(TEST_BIN)

$(TEST_BIN): $(TEST_SRC) $(STATIC_LIB)
	@mkdir -p test
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $< -L$(LIB_DIR) -lkvs_persistence $(LDFLAGS)

# Clean
clean:
	rm -rf $(BUILD_DIR) $(LIB_DIR) $(TEST_BIN)

# Install headers and library
install: all
	@echo "Installing headers to /usr/local/include/kvs_persistence/"
	sudo mkdir -p /usr/local/include/kvs_persistence
	sudo cp $(INC_DIR)/*.h $(INC_DIR)/*.hpp /usr/local/include/kvs_persistence/
	sudo cp $(STATIC_LIB) $(SHARED_LIB) /usr/local/lib/
	sudo ldconfig

.PHONY: all dirs clean test install
