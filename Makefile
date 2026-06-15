CC      ?= gcc
SRCDIR   = src
BUILDDIR = build

CFLAGS  += -std=gnu11 -D_GNU_SOURCE
CFLAGS  += $(shell pkg-config --cflags glib-2.0 gio-2.0 gio-unix-2.0)
CFLAGS  += -DHAVE_INOTIFY -DHAVE_MALLOC_TRIM
CFLAGS  += -include $(SRCDIR)/config.h

LDFLAGS += $(shell pkg-config --libs glib-2.0 gio-2.0 gio-unix-2.0 libpcre2-8 icu-uc icu-io) -lm

SRCS = $(wildcard $(SRCDIR)/*.c)
OBJS = $(patsubst $(SRCDIR)/%.c, $(BUILDDIR)/%.o, $(SRCS))

TARGET = fsearch-mcp

.PHONY: all clean check

all: $(TARGET)

$(BUILDDIR)/%.o: $(SRCDIR)/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

$(TARGET): $(OBJS)
	$(CC) $^ $(LDFLAGS) -o $@

# Syntax check only — doesn't produce object files
check:
	@mkdir -p $(BUILDDIR)
	@errors=0; ok=0; \
	for f in $(SRCS); do \
		bn=$$(basename $$f); \
		if $(CC) $(CFLAGS) -fsyntax-only -c $$f 2>/dev/null; then \
			echo "  ✓ $${bn}"; ok=$$((ok+1)); \
		else \
			err=$$($(CC) $(CFLAGS) -fsyntax-only -c $$f 2>&1 | grep 'error:' | head -1 | sed 's/.*error: //'); \
			echo "  ✗ $${bn} → $${err}"; errors=$$((errors+1)); \
		fi; \
	done; \
	echo "---"; \
	echo "$${ok} OK, $${errors} failed / $$(echo $(SRCS) | wc -w)"

$(BUILDDIR)/main.o: $(SRCDIR)/main.c | $(BUILDDIR)

clean:
	rm -rf $(BUILDDIR) $(TARGET)
