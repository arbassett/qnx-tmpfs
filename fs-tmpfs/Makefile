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

.PHONY: all clean

all: $(DRIVER) $(STAT)

$(DRIVER): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

$(STAT): $(TOOLDIR)/tmpfs-stat.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TOOLDIR)/tmpfs-stat.o $(DRIVER) $(STAT)
