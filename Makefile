CC      = gcc
CFLAGS  = -std=c11 -Wall -Wextra -O2 -g

RAYLIB_CFLAGS = $(shell pkg-config --cflags raylib 2>/dev/null || echo "-I/opt/homebrew/include")
RAYLIB_LIBS = $(shell pkg-config --libs raylib 2>/dev/null || echo "-L/opt/homebrew/lib -lraylib -framework OpenGL -framework Cocoa -framework IOKit -framework CoreAudio -framework CoreVideo")

all: liballocator.a test_basic benchmark test_buddy benchmark_buddy simulator gui_simulator

liballocator.a: allocator.c allocator.h buddy_allocator.c buddy_allocator.h
	$(CC) $(CFLAGS) -c allocator.c -o allocator.o
	$(CC) $(CFLAGS) -c buddy_allocator.c -o buddy_allocator.o
	ar rcs $@ allocator.o buddy_allocator.o
	rm -f allocator.o buddy_allocator.o

test_basic: test_basic.c allocator.c allocator.h
	$(CC) $(CFLAGS) -o $@ test_basic.c allocator.c

test: test_basic test_buddy
	./test_basic
	./test_buddy

asan: test_basic.c allocator.c allocator.h
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o test_asan test_basic.c allocator.c
	./test_asan

clean:
	rm -f liballocator.a *.o test_basic test_asan benchmark test_buddy benchmark_buddy simulator gui_simulator

.PHONY: all test asan clean

benchmark: benchmark.c allocator.c allocator.h buddy_allocator.c buddy_allocator.h
	$(CC) $(CFLAGS) -o $@ benchmark.c allocator.c buddy_allocator.c

test_buddy: test_buddy.c buddy_allocator.c buddy_allocator.h
	$(CC) $(CFLAGS) -o $@ test_buddy.c buddy_allocator.c

benchmark_buddy: benchmark_buddy.c buddy_allocator.c buddy_allocator.h
	$(CC) $(CFLAGS) -o $@ benchmark_buddy.c buddy_allocator.c

simulator: simulator.c allocator.c allocator.h
	$(CC) $(CFLAGS) -o $@ simulator.c allocator.c

gui_simulator: gui_simulator.c allocator.c allocator.h buddy_allocator.c buddy_allocator.h
	$(CC) $(CFLAGS) $(RAYLIB_CFLAGS) -o $@ gui_simulator.c allocator.c buddy_allocator.c $(RAYLIB_LIBS)
