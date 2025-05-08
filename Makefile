CC=gcc
CFLAGS=-Wall -Wpedantic -Wextra

TARGET=minilc3
BINDIR = /usr/local/bin

.PHONY: install run watch clean

$(TARGET): main.c
	$(CC) $(CFLAGS) main.c -o $(TARGET)

install:
	sudo install -m 755 $(TARGET) $(BINDIR)

clean:
	rm -f ./$(TARGET)
	rm -f examples/*.{obj,sym,lc3}

