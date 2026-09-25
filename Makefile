CC ?= gcc
CFLAGS ?= -Wall -Wextra -std=c99 -pedantic -D_GNU_SOURCE -Iinclude -O2
LDFLAGS ?=

# Check for pkg-config and libsystemd
HAS_LIBSYSTEMD := $(shell pkg-config --exists libsystemd 2>/dev/null && echo 1 || echo 0)
ifeq ($(HAS_LIBSYSTEMD), 1)
    CFLAGS += -DHAVE_LIBSYSTEMD=1 $(shell pkg-config --cflags libsystemd)
    LDFLAGS += $(shell pkg-config --libs libsystemd)
endif

SRC = src/cJSON.c \
      src/config.c \
      src/unit_gen.c \
      src/dbus_systemd.c \
      src/log_viewer.c \
      src/tui.c \
      src/main.c

OBJ = $(SRC:.c=.o)
TARGET = bin/fdash
TEST_TARGET = bin/test_core

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

.PHONY: all clean install uninstall debug test

all: $(TARGET)

$(TARGET): $(OBJ)
	@mkdir -p bin
	$(CC) $(OBJ) -o $@ $(LDFLAGS)
	@echo "Build complete: $(TARGET)"

test: $(TEST_TARGET)
	@./$(TEST_TARGET)

$(TEST_TARGET): tests/test_core.o src/cJSON.o src/config.o src/unit_gen.o
	@mkdir -p bin
	$(CC) $^ -o $@ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

debug: CFLAGS += -g -O0 -DDEBUG
debug: clean all

install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/fdash
	ln -sf fdash $(DESTDIR)$(BINDIR)/fire-dash
	@echo "Installed fdash (and fire-dash alias) to $(DESTDIR)$(BINDIR)/"

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/fdash $(DESTDIR)$(BINDIR)/fire-dash
	@echo "Removed fdash and fire-dash from $(DESTDIR)$(BINDIR)/"

clean:
	rm -f src/*.o $(TARGET)
