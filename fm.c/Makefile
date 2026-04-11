CC = cc
CFLAGS = -Wall -Wextra -O2
LDLIBS = -lncurses

fm: fm.c
	$(CC) $(CFLAGS) -o $@ fm.c $(LDLIBS)

clean:
	rm -f fm

.PHONY: clean
