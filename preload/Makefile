CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -fPIC
LDFLAGS ?= -shared
LIBS    ?= -ldl -lpthread

TARGET  := qd2_remote_input.so
SRC     := qd2_remote_input.c
PREFIX  ?= /home/mks

.PHONY: all clean install

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIBS)

install: $(TARGET)
	install -m 0755 $(TARGET) $(PREFIX)/$(TARGET)

clean:
	rm -f $(TARGET)
