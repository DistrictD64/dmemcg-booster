CC = gcc
CFLAGS = -Wall -Wextra -g `pkg-config --cflags dbus-1`
LDFLAGS = `pkg-config --libs dbus-1`

SRCS = src/main.c src/cgroup.c
OBJS = $(SRCS:.c=.o)
TARGET = dmemcg-booster

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
