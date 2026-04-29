# Makefile for libgc - Conservative Garbage Collector
# Supports static library, shared library, test program, and installation.

# Compiler and tools
CC      = gcc
AR      = ar
RM      = rm -f
INSTALL = install

# Flags
#CFLAGS   = -Wall -Wextra -O2 -pthread -fPIC -DDEBUG
CFLAGS   = -g -Wall -Wextra -O2 -pthread -fPIC
LDFLAGS  = -pthread
LDLIBS   = -lpthread

# Installation paths (can be overridden)
PREFIX   ?= /usr/local
DESTDIR  ?=
BINDIR   = $(DESTDIR)$(PREFIX)/bin
LIBDIR   = $(DESTDIR)$(PREFIX)/lib
INCDIR   = $(DESTDIR)$(PREFIX)/include
MANDIR   = $(DESTDIR)$(PREFIX)/share/man/man3

# Files
HEADERS   = gc.h
SOURCES   = gc.core.c gc.init.c gc.mark.c gc.sweep.c gc.os.c gc.root.c gc.stw.c gc.thread.c gc.throw.c
OBJECTS   = gc.core.o gc.init.o gc.mark.o gc.sweep.o gc.os.o gc.root.o gc.stw.o gc.thread.o gc.throw.o
#SOURCES   = gc.c
#OBJECTS   = gc.o
LIB_STATIC = libgc.a
LIB_SHARED = libgc.so
TEST_SRC  = gc.test.c
TEST_BIN  = gc.test
MAN_PAGE  = gc.3

# Targets
all: $(LIB_STATIC) $(LIB_SHARED) $(TEST_BIN)

# Static library
$(LIB_STATIC): $(OBJECTS)
	$(AR) rcs $@ $^

# Shared library
$(LIB_SHARED): $(OBJECTS)
	$(CC) -shared $(LDFLAGS) -o $@ $^ $(LDLIBS)

# Object files
%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

# Test program (renamed from test_gc to gc.test)
$(TEST_BIN): $(TEST_SRC) $(HEADERS) $(LIB_STATIC)
	$(CC) $(CFLAGS) -o $@ $(TEST_SRC) -L. -lgc $(LDLIBS)

# Clean build artifacts
clean:
	$(RM) $(OBJECTS) $(LIB_STATIC) $(LIB_SHARED) $(TEST_BIN)

# Install
install: all
	$(INSTALL) -d $(BINDIR) $(LIBDIR) $(INCDIR) $(MANDIR)
	$(INSTALL) -m 755 $(TEST_BIN) $(BINDIR)/
	$(INSTALL) -m 644 $(LIB_STATIC) $(LIBDIR)/
	$(INSTALL) -m 755 $(LIB_SHARED) $(LIBDIR)/
	$(INSTALL) -m 644 $(HEADERS) $(INCDIR)/
	$(INSTALL) -m 644 $(MAN_PAGE) $(MANDIR)/
	@echo "Installation complete. Run 'sudo ldconfig' if necessary."

# Uninstall
uninstall:
	$(RM) $(BINDIR)/$(TEST_BIN)
	$(RM) $(LIBDIR)/$(LIB_STATIC)
	$(RM) $(LIBDIR)/$(LIB_SHARED)
	$(RM) $(INCDIR)/$(HEADERS)
	$(RM) $(MANDIR)/$(MAN_PAGE)
	@echo "Uninstallation complete."

# Phony targets
.PHONY: all clean install uninstall
