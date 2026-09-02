CC := clang
LDFLAGS := -no-pie
PROGRAMS := hello sum fib

.PHONY: all run clean

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

clean:
	rm -f $(PROGRAMS) *.o
