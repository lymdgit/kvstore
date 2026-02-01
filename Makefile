
CC = gcc
CXX = g++
CFLAGS = -mcmodel=medium -g -O3
CXXFLAGS = -std=c++17 -mcmodel=medium -g -O3 -fPIC

# Include persistence headers
FLAGS = -I ./NtyCo/core/ -I ./persistence/include/ -L ./NtyCo/ -L ./persistence/lib/ -lntyco -lpthread -ldl -luring $(CFLAGS)

# Source files
SRCS = kvstore.c ntyco_entry.c epoll_entry.c iouring_entry.c kvstore_array.c kvstore_rbtree.c kvstore_hash.c kvstore_btree.c kvstore_skiptable.c kvstore_mp.c
PERSIST_SRCS = kvstore_skiptable_persist.c wal_buffer.c wal_flusher.c kvstore_persist.c

TESTCASE_SRCS = testcase.c
TARGET = kvstore
SUBDIR = ./NtyCo/
PERSIST_DIR = ./persistence/
TESTCASE = testcase

OBJS = $(SRCS:.c=.o)
PERSIST_OBJS = $(PERSIST_SRCS:.c=.o)

BENCHMARK_SRCS = kv_benchmark.c
BENCHMARK = kv_benchmark

# Persistence test
PERSIST_TEST = test_persistence

# Persistence library
PERSIST_LIB = $(PERSIST_DIR)lib/libkvs_persistence.a

all: $(SUBDIR) $(PERSIST_DIR) $(TARGET) $(TESTCASE) $(BENCHMARK) $(PERSIST_TEST)

$(SUBDIR): ECHO
	make -C $@

$(PERSIST_DIR): ECHO_PERSIST
	make -C $@

ECHO:
	@echo $(SUBDIR)

ECHO_PERSIST:
	@echo "Building persistence library..."

$(TARGET): $(OBJS) $(PERSIST_OBJS) $(PERSIST_LIB)
	$(CXX) -o $@ $(OBJS) $(PERSIST_OBJS) $(PERSIST_LIB) $(FLAGS) -lstdc++

$(TESTCASE): $(TESTCASE_SRCS)
	$(CC) -o $@ $^

$(BENCHMARK): $(BENCHMARK_SRCS)
	$(CC) -o $@ $^ -lpthread $(CFLAGS)

# Standalone persistence test (statically linked)
$(PERSIST_TEST): test_persistence.c $(PERSIST_LIB)
	$(CC) -o $@ test_persistence.c $(PERSIST_LIB) -I./persistence/include -luring -lstdc++ -lpthread $(CFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -I ./NtyCo/core/ -I ./persistence/include/ -c $^ -o $@

clean: 
	rm -rf $(OBJS) $(PERSIST_OBJS) $(TARGET) $(TESTCASE) $(BENCHMARK) $(PERSIST_TEST)
	make -C $(PERSIST_DIR) clean

.PHONY: all clean ECHO ECHO_PERSIST
