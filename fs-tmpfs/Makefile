CC      = clang
CFLAGS  = -Wall -Wextra -Wno-unused-parameter \
           -O2 -g \
           -D_QNX_SOURCE \
           -Iinclude

LDFLAGS =
LIBS    = -lc

SRCDIR  = src
TOOLDIR = tools

SRCS =  $(SRCDIR)/main.c      \
        $(SRCDIR)/memory.c    \
        $(SRCDIR)/inode.c     \
        $(SRCDIR)/dir.c       \
        $(SRCDIR)/file.c      \
        $(SRCDIR)/symlink.c   \
        $(SRCDIR)/mmap.c      \
        $(SRCDIR)/control.c   \
        $(SRCDIR)/mount.c     \
        $(SRCDIR)/resmgr.c

OBJS    = $(SRCS:.c=.o)

DRIVER  = fs-tmpfs
STAT    = tmpfs-stat

# Installation paths
INSTALL_BIN  = /usr/bin
INSTALL_STAT = /usr/bin

.PHONY: all clean install uninstall

all: $(DRIVER) $(STAT)

$(DRIVER): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

$(STAT): $(TOOLDIR)/tmpfs-stat.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

install: all
	@echo "Installing $(DRIVER) to $(INSTALL_BIN)/$(DRIVER)"
	cp $(DRIVER) $(INSTALL_BIN)/$(DRIVER)
	@echo "Creating $(INSTALL_BIN)/mount_tmpfs symlink"
	ln -sf $(INSTALL_BIN)/$(DRIVER) $(INSTALL_BIN)/mount_tmpfs
	@echo "Installing $(STAT) to $(INSTALL_STAT)/$(STAT)"
	cp $(STAT) $(INSTALL_STAT)/$(STAT)
	@echo "Install complete."
	@echo "  Use: mount -t tmpfs [-o size=N] none <mountpoint>"
	@echo "  Use: umount <mountpoint>"

uninstall:
	@echo "Removing installed files"
	rm -f $(INSTALL_BIN)/$(DRIVER)
	rm -f $(INSTALL_BIN)/mount_tmpfs
	rm -f $(INSTALL_STAT)/$(STAT)

clean:
	rm -f $(OBJS) $(TOOLDIR)/tmpfs-stat.o $(DRIVER) $(STAT)
