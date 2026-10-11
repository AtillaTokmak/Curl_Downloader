CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2
PREFIX   ?= /usr/local

all: curldl

curldl: curldl.cpp
	$(CXX) $(CXXFLAGS) curldl.cpp -o $@

test_runner: test_curldl.cpp curldl.cpp
	$(CXX) $(CXXFLAGS) test_curldl.cpp -o $@

test: curldl test_runner
	./test_runner

install: curldl
	install -D -m 755 curldl $(DESTDIR)$(PREFIX)/bin/curldl

clean:
	rm -f curldl test_runner
	rm -rf test_target_dir

.PHONY: all install clean test
