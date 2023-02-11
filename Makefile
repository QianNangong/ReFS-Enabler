ARCH:=x86_64
CC=$(ARCH)-w64-mingw32-gcc
WINDRES=$(ARCH)-w64-mingw32-windres
CFLAGS=-municode -O2
LDFLAGS=$(CFLAGS) -mconsole -lwinmm -ldismapi

all: enabler

%.res: %.rc
	$(WINDRES) -o $@ -i $<

%.o: %.c
	$(CC) -c -o $@ $< $(CFLAGS)

enabler: main.o resource.res
	$(CC) -o enabler.exe main.o resource.res $(LDFLAGS)

clean:
	$(RM) resource.res
	$(RM) main.o
	$(RM) enabler.exe

.PHONY: all
