CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2
PREFIX   ?= /usr/local

curldl: curldl.cpp
	$(CXX) $(CXXFLAGS) curldl.cpp -o $@

install: curldl
	install -D -m 755 curldl $(DESTDIR)$(PREFIX)/bin/curldl

clean:
	rm -f curldl

.PHONY: install clean
