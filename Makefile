# Makefile for gtree (Linux / macOS)
#
#   make             build ./gtree
#   make test        build and run the whole test suite (tests/run.sh)
#   make debug       build with symbols and no optimisation
#   make install     build and copy to PREFIX/bin (default /usr/local/bin)
#                     may need: sudo make install
#   make uninstall   remove the installed copy
#   make clean       remove build artifacts
#
# On Windows, build.bat builds and `powershell -File tests\run.ps1` tests.

CC      ?= cc
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra
SRC     := src/main.c src/gitignore.c src/treeio.c
HDR     := src/gitignore.h src/treeio.h
BIN     := gtree
PREFIX  ?= /usr/local

.PHONY: all test test-original debug install uninstall clean

all: $(BIN)

$(BIN): $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $(BIN) $(SRC)

# The suite builds its own binaries into a temporary directory, so it does not
# depend on $(BIN) and never leaves anything behind in the project.
test:
	sh tests/run.sh

# The same suite compiled against the sources as they were before this
# project's cleanup pass, to confirm the regression tests still fail there.
# Needs a git checkout; see the header of tests/run.sh.
test-original:
	sh tests/run.sh --original

debug: $(SRC) $(HDR)
	$(CC) -std=c99 -O0 -g -Wall -Wextra -o $(BIN) $(SRC)

install: $(BIN)
	mkdir -p "$(PREFIX)/bin"
	cp "$(BIN)" "$(PREFIX)/bin/$(BIN)"
	chmod 755 "$(PREFIX)/bin/$(BIN)"
	@echo "Installed to $(PREFIX)/bin/$(BIN)"
	@echo "Make sure $(PREFIX)/bin is on your PATH (see README.md)."

uninstall:
	rm -f "$(PREFIX)/bin/$(BIN)"

clean:
	rm -f $(BIN)
