# Makefile — SPD PSU controller (SPD1305X / SPD3303X-E)

CC      := gcc
CFLAGS  := -std=c11 -Wall -Wextra -Wpedantic -O2
LDFLAGS :=

TARGET  := psuCtrl
SRCDIR  := src
OBJDIR  := build

SRCS := $(SRCDIR)/main.c \
        $(SRCDIR)/scpi_transport.c \
        $(SRCDIR)/spd_driver.c

OBJS := $(patsubst $(SRCDIR)/%.c, $(OBJDIR)/%.o, $(SRCS))

# -----------------------------------------------------------------------

.PHONY: all clean install

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR):
	mkdir -p $@

clean:
	rm -rf $(OBJDIR) $(TARGET)

# Optional: install to ~/.local/bin
install: $(TARGET)
	install -Dm755 $(TARGET) $(HOME)/.local/bin/$(TARGET)
