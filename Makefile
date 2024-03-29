
app=uinth

CFLAGS+=-Wall

prefix?=/usr/local
exec_prefix?=$(prefix)
bindir?=$(exec_prefix)/bin
sysconfdir?=/etc/

CFLAGS+=-DSYSCONFDIR=\"$(sysconfdir)\"

-include Makefile.local

all: $(app)

$(app): uinth.c

install: $(app) Makefile
	install -d "$(DESTDIR)/$(bindir)"
	install -m 0755 uinth "$(DESTDIR)/$(bindir)"

clean:
	rm -rf *.o $(app)
