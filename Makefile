CC := clang
LDFLAGS := -no-pie
PROGRAMS := hello sum fib

ARRAY_ADD_CFLAGS := -std=c17 -O3 -march=x86-64 -mtune=generic -fPIE \
	-D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wpedantic -Werror
ARRAY_ADD_LDFLAGS := -pie -Wl,-z,relro,-z,now -Wl,-z,noexecstack
ARRAY_ADD_SOURCES := array_add_dispatch.c array_add_scalar.s \
	array_add_zen5_avx512f.s array_add_xgetbv.s
ARRAY_ADD_HEADERS := array_add.h array_add_internal.h
ARRAY_ADD_STATS_SOURCE := array_add_stats.c
ARRAY_ADD_STATS_HEADER := array_add_stats.h
ARRAY_ADD_LIBS := -lm

.PHONY: all run array-add test-array-add test-array-add-stats \
	test-array-add-rss-analyze test-array-add-zen5 \
	array-add-test-sanitize array-add-stats-test-sanitize \
	array-add-rss-analyze-sanitize \
	test-array-add-rss-analyze-sanitize test-output-contract clean

all: $(PROGRAMS)

hello: hello.s
	$(CC) $(LDFLAGS) $< -o $@

sum: sum.s
	$(CC) $(LDFLAGS) $< -o $@

fib: fib.s
	$(CC) $(LDFLAGS) $< -o $@

run: all
	./hello
	./sum
	./fib

array-add: array-add-test array-add-stats-test array-add-bench \
	array-add-rss-analyze

array-add-test: array_add_test.c $(ARRAY_ADD_SOURCES) $(ARRAY_ADD_HEADERS)
	$(CC) $(ARRAY_ADD_CFLAGS) array_add_test.c $(ARRAY_ADD_SOURCES) \
		$(ARRAY_ADD_LDFLAGS) $(ARRAY_ADD_LIBS) -o $@

array-add-stats-test: array_add_stats_test.c $(ARRAY_ADD_STATS_SOURCE) \
		$(ARRAY_ADD_STATS_HEADER)
	$(CC) $(ARRAY_ADD_CFLAGS) array_add_stats_test.c \
		$(ARRAY_ADD_STATS_SOURCE) $(ARRAY_ADD_LDFLAGS) \
		$(ARRAY_ADD_LIBS) -o $@

array-add-bench: array_add_bench.c $(ARRAY_ADD_SOURCES) \
		$(ARRAY_ADD_HEADERS) $(ARRAY_ADD_STATS_SOURCE) \
		$(ARRAY_ADD_STATS_HEADER)
	$(CC) $(ARRAY_ADD_CFLAGS) array_add_bench.c $(ARRAY_ADD_SOURCES) \
		$(ARRAY_ADD_STATS_SOURCE) $(ARRAY_ADD_LDFLAGS) \
		$(ARRAY_ADD_LIBS) -o $@

array-add-rss-analyze: array_add_rss_analyze.c $(ARRAY_ADD_STATS_SOURCE) \
		$(ARRAY_ADD_STATS_HEADER)
	$(CC) $(ARRAY_ADD_CFLAGS) array_add_rss_analyze.c \
		$(ARRAY_ADD_STATS_SOURCE) $(ARRAY_ADD_LDFLAGS) \
		$(ARRAY_ADD_LIBS) -o $@

array-add-bench-contract-test: array_add_bench.c array_add_bench_test_clock.c \
		$(ARRAY_ADD_SOURCES) $(ARRAY_ADD_HEADERS) $(ARRAY_ADD_STATS_SOURCE) \
		$(ARRAY_ADD_STATS_HEADER)
	$(CC) $(ARRAY_ADD_CFLAGS) array_add_bench.c array_add_bench_test_clock.c \
		$(ARRAY_ADD_SOURCES) $(ARRAY_ADD_STATS_SOURCE) \
		$(ARRAY_ADD_LDFLAGS) -Wl,--wrap=clock_gettime \
		-Wl,--wrap=sched_getaffinity -Wl,--wrap=sched_getcpu \
		$(ARRAY_ADD_LIBS) -o $@

test-output-contract: all array-add-bench-contract-test output_contract_test.sh
	sh output_contract_test.sh ./array-add-bench-contract-test \
		./hello ./sum ./fib

test-array-add: array-add-test
	./array-add-test

test-array-add-stats: array-add-stats-test
	./array-add-stats-test

test-array-add-rss-analyze: array-add-rss-analyze \
		array_add_rss_analyze_test.sh
	sh array_add_rss_analyze_test.sh ./array-add-rss-analyze

test-array-add-zen5: array-add-test
	./array-add-test --require-vector

array-add-test-sanitize: array_add_test.c $(ARRAY_ADD_SOURCES) $(ARRAY_ADD_HEADERS)
	$(CC) $(ARRAY_ADD_CFLAGS) -O1 -g -fno-omit-frame-pointer \
		-fsanitize=address,undefined array_add_test.c $(ARRAY_ADD_SOURCES) \
		$(ARRAY_ADD_LDFLAGS) -fsanitize=address,undefined \
		$(ARRAY_ADD_LIBS) -o $@

array-add-stats-test-sanitize: array_add_stats_test.c \
		$(ARRAY_ADD_STATS_SOURCE) $(ARRAY_ADD_STATS_HEADER)
	$(CC) $(ARRAY_ADD_CFLAGS) -O1 -g -fno-omit-frame-pointer \
		-fsanitize=address,undefined array_add_stats_test.c \
		$(ARRAY_ADD_STATS_SOURCE) $(ARRAY_ADD_LDFLAGS) \
		-fsanitize=address,undefined $(ARRAY_ADD_LIBS) -o $@

array-add-rss-analyze-sanitize: array_add_rss_analyze.c \
		$(ARRAY_ADD_STATS_SOURCE) $(ARRAY_ADD_STATS_HEADER)
	$(CC) $(ARRAY_ADD_CFLAGS) -O1 -g -fno-omit-frame-pointer \
		-fsanitize=address,undefined array_add_rss_analyze.c \
		$(ARRAY_ADD_STATS_SOURCE) $(ARRAY_ADD_LDFLAGS) \
		-fsanitize=address,undefined $(ARRAY_ADD_LIBS) -o $@

test-array-add-rss-analyze-sanitize: array-add-rss-analyze-sanitize \
		array_add_rss_analyze_test.sh
	sh array_add_rss_analyze_test.sh ./array-add-rss-analyze-sanitize

clean:
	rm -f $(PROGRAMS) array-add-test array-add-stats-test array-add-bench \
		array-add-rss-analyze array-add-test-sanitize \
		array-add-stats-test-sanitize array-add-rss-analyze-sanitize \
		array-add-bench-contract-test *.o
