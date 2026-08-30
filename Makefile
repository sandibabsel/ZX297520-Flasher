CC      ?= gcc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -Wall -Wextra -Wshadow -Wno-unused-parameter
LDFLAGS ?=

PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin

SRCDIR  := src
OBJDIR  := build
BIN     := zxdl
SIM     := simdev

SRCS := $(wildcard $(SRCDIR)/*.c)
OBJS := $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d)

# objects shared with the simulator (everything except main.o)
LIBOBJS := $(filter-out $(OBJDIR)/main.o,$(OBJS))

.PHONY: all clean install uninstall strict test sim

all: $(BIN)

sim: $(SIM)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(SIM): $(OBJDIR)/simdev.o $(LIBOBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(OBJDIR)/simdev.o: tools/simdev.c | $(OBJDIR)
	$(CC) $(CFLAGS) -I$(SRCDIR) -MMD -MP -c -o $@ $<

test: $(BIN) $(SIM)
	@./tests/run.sh

$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

$(OBJDIR):
	mkdir -p $(OBJDIR)

# Build with everything turned up; useful before sending patches.
strict: CFLAGS += -Werror -Wconversion -Wsign-conversion -Wpedantic
strict: clean all

clean:
	rm -rf $(OBJDIR) $(BIN) $(SIM)

install: $(BIN)
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(BIN) $(DESTDIR)$(BINDIR)/$(BIN)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(BIN)

-include $(DEPS)
