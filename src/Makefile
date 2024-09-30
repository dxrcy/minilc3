CC=gcc
CFLAGS=-Wall -Wpedantic -Wextra

TARGET=minilc3
BINDIR = /usr/local/bin

.PHONY: install run watch clean

$(TARGET): main.c
	$(CC) $(CFLAGS) main.c -o $(TARGET)

install:
	sudo install -m 755 $(TARGET) $(BINDIR)

name=hello_world
run: $(TARGET)
	@laser -a examples/$(name).asm >/dev/null
	@./$(TARGET) examples/$(name).obj

watch:
	@clear
	@reflex --decoration=none -r '*.c|.*\.asm' -s -- zsh -c \
		'clear; sleep 0.2; $(MAKE) --no-print-directory run'

clean:
	rm -f ./$(TARGET)
	rm -f examples/*.{obj,sym,lc3}

