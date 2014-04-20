CC = gcc
CPPFLAGS = -g -I.
all: chatter
chatter: chatter.o
chatter.o: chatter.c
clean:
	rm chatter
	rm *.o
