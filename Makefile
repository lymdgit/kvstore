

CC = gcc
CFLAGS = -mcmodel=medium -g -O3
FLAGS = -I ./NtyCo/core/ -L ./NtyCo/ -lntyco -lpthread -ldl -luring $(CFLAGS)
SRCS = kvstore.c ntyco_entry.c epoll_entry.c iouring_entry.c kvstore_array.c kvstore_rbtree.c kvstore_hash.c kvstore_btree.c kvstore_skiptable.c kvstore_mp.c
TESTCASE_SRCS = testcase.c
TARGET = kvstore
SUBDIR = ./NtyCo/
TESTCASE = testcase

OBJS = $(SRCS:.c=.o)


BENCHMARK_SRCS = kv_benchmark.c
BENCHMARK = kv_benchmark

all: $(SUBDIR) $(TARGET) $(TESTCASE) $(BENCHMARK)

$(SUBDIR): ECHO
	make -C $@

ECHO:
	@echo $(SUBDIR)

$(TARGET): $(OBJS) 
	$(CC) -o $@ $^ $(FLAGS)

$(TESTCASE): $(TESTCASE_SRCS)
	$(CC) -o $@ $^

$(BENCHMARK): $(BENCHMARK_SRCS)
	$(CC) -o $@ $^ -lpthread $(CFLAGS)

%.o: %.c
	$(CC) $(FLAGS) -c $^ -o $@

clean: 
	rm -rf $(OBJS) $(TARGET) $(TESTCASE) $(BENCHMARK)
