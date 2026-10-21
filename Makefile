all: 
	$(CC) -O2 -std=c2x -Werror -Wall -Wextra -pedantic $(CFLAGS) -o keylightb keylightb.c

install:
	install -m 755 -D -t $(DESTDIR)/usr/bin/ keylightb
	install -m 644 -D -t $(DESTDIR)/usr/lib/systemd/system/ keylightb.service
