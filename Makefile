CC      = cc
CFLAGS  = -std=c89 -Wall -O2
PREFIX  = /usr/local
BINDIR  = $(DESTDIR)$(PREFIX)/bin

pdfview: pdfview.c
	$(CC) $(CFLAGS) -o $@ pdfview.c -lX11

install: pdfview
	mkdir -p $(BINDIR)
	install -m 755 pdfview $(BINDIR)/pdfview

uninstall:
	rm -f $(BINDIR)/pdfview

clean:
	rm -f pdfview

.PHONY: install uninstall clean
