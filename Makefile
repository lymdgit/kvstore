
CC = gcc
CFLAGS = -mcmodel=medium -g -O3

# Flags: NtyCo headers + pthread + io_uring
FLAGS = -I ./NtyCo/core/ -L ./NtyCo/ -lntyco -lpthread -ldl -luring $(CFLAGS)

# Source files
SRCS = kvstore.c ntyco_entry.c epoll_entry.c iouring_entry.c \
       kvstore_array.c kvstore_rbtree.c kvstore_hash.c kvstore_btree.c \
       kvstore_skiptable.c kvstore_mp.c

# WAL persistence (pure C, io_uring based)
WAL_SRCS = wal_buffer.c wal_flusher.c kvstore_persist.c

TARGET = kvstore
SUBDIR = ./NtyCo/

OBJS = $(SRCS:.c=.o)
WAL_OBJS = $(WAL_SRCS:.c=.o)

# Benchmark tool
BENCHMARK_SRCS = kv_benchmark.c
BENCHMARK = kv_benchmark

all: $(SUBDIR) $(TARGET) $(BENCHMARK)

$(SUBDIR): ECHO
	make -C $@

ECHO:
	@echo $(SUBDIR)

$(TARGET): $(OBJS) $(WAL_OBJS)
	$(CC) -o $@ $(OBJS) $(WAL_OBJS) $(FLAGS)

$(BENCHMARK): $(BENCHMARK_SRCS)
	$(CC) -o $@ $^ -lpthread $(CFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -I ./NtyCo/core/ -c $^ -o $@

clean: 
	rm -rf $(OBJS) $(WAL_OBJS) $(TARGET) $(BENCHMARK)

.PHONY: all clean ECHO
