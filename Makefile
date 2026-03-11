CC     ?= gcc
CPPFLAGS += -Ithird_party/expat -Ithird_party/expat/lib
OPT_CFLAGS ?= -O2 -flto
COMMON_CFLAGS = -Wall -Wextra -DNOCRYPT -DUSE_FILE32API -Wno-unused-parameter
CFLAGS += $(OPT_CFLAGS) $(COMMON_CFLAGS)
OPT_LDFLAGS ?= -flto
LDFLAGS += $(OPT_LDFLAGS) -lz
VPATH   = minizip-1.1 third_party/expat/lib

PREFIX ?= /usr/local/bin

OBJECTS = luna.o zip.o ioapi.o DES.o xmlparse.o xmltok.o xmlrole.o

OS := $(shell uname -s)
ifeq ($(OS),Windows_NT)
  EXEEXT = .exe
endif

BIN := luna$(EXEEXT)

all: $(BIN)

$(BIN): $(OBJECTS)
	$(CC) -o $@ $^ $(LDFLAGS)

test: $(BIN)
	./tests/test_cli.sh ./$(BIN)

sanitize:
	$(MAKE) clean
	$(MAKE) CC=clang OPT_CFLAGS='-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer' OPT_LDFLAGS='-fsanitize=address,undefined' test

install: $(BIN)
	mkdir -p $(PREFIX)
	install $(BIN) $(PREFIX)

dist: clean all
	mkdir -p dist/src
	find . -maxdepth 1 ! -name '$(BIN)' -a ! -name dist -a ! -name . -exec cp -r {} dist/src \;
	cp $(BIN) *.md LICENSE dist

clean:
	$(RM) -r $(OBJECTS) $(BIN) dist

.PHONY: all test sanitize install dist clean
