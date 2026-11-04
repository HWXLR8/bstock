CFLAGS  = -O2 -Wall -Wextra
LDLIBS  = $(shell pkg-config --libs libcurl) -lm

bstock: bstock.c
	$(CC) $(CFLAGS) -o $@ $< $(shell pkg-config --cflags libcurl) $(LDLIBS)

run: bstock
	./bstock

clean:
	rm -f bstock

.PHONY: run clean
