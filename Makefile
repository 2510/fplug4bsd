all: fplug

clean:
	rm -f fplug

fplug: fplug.c
	$(CC) -o fplug fplug.c
