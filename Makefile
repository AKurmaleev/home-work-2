CC := clang
LDFLAGS := -no-pie
PROGRAMS := hello sum fib

ARRAY_ADD_CFLAGS := -std=c17 -O3 -march=x86-64 -mtune=generic -fPIE \
	-D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wpedantic -Werror
ARRAY_ADD_LDFLAGS := -pie -Wl,-z,relro,-z,now -Wl,-z,noexecstack
ARRAY_ADD_SOURCES := array_add_dispatch.c array_add_scalar.s \
	array_add_zen5_avx512f.s array_add_xgetbv.s
ARRAY_ADD_HEADERS := array_add.h array_add_internal.h
ARRAY_ADD_LIBS := -lm

.PHONY: all run array-add test-array-add test-array-add-zen5 \
	array-add-test-sanitize clean

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

array-add: array-add-test array-add-bench

array-add-test: array_add_test.c $(ARRAY_ADD_SOURCES) $(ARRAY_ADD_HEADERS)
	$(CC) $(ARRAY_ADD_CFLAGS) array_add_test.c $(ARRAY_ADD_SOURCES) \
		$(ARRAY_ADD_LDFLAGS) $(ARRAY_ADD_LIBS) -o $@

array-add-bench: array_add_bench.c $(ARRAY_ADD_SOURCES) $(ARRAY_ADD_HEADERS)
	$(CC) $(ARRAY_ADD_CFLAGS) array_add_bench.c $(ARRAY_ADD_SOURCES) \
		$(ARRAY_ADD_LDFLAGS) $(ARRAY_ADD_LIBS) -o $@

test-array-add: array-add-test
	./array-add-test

test-array-add-zen5: array-add-test
	./array-add-test --require-vector

array-add-test-sanitize: array_add_test.c $(ARRAY_ADD_SOURCES) $(ARRAY_ADD_HEADERS)
	$(CC) $(ARRAY_ADD_CFLAGS) -O1 -g -fno-omit-frame-pointer \
		-fsanitize=address,undefined array_add_test.c $(ARRAY_ADD_SOURCES) \
		$(ARRAY_ADD_LDFLAGS) -fsanitize=address,undefined \
		$(ARRAY_ADD_LIBS) -o $@

clean:
	rm -f $(PROGRAMS) array-add-test array-add-bench \
		array-add-test-sanitize *.o
