CC = gcc
CFLAGS = -Wall -Wextra -O2 -std=gnu11
LDFLAGS =

all: kayles_server kayles_client

kayles_server: kayles_server.o kayles.o common.o err.o
	$(CC) $(LDFLAGS) -o $@ $^

kayles_client: kayles_client.o common.o err.o
	$(CC) $(LDFLAGS) -o $@ $^

%.o: %.c protocol.h kayles.h common.h err.h
	$(CC) $(CFLAGS) -c $<

clean:
	rm -f *.o kayles_server kayles_client