CFLAGS += -Wall

ptysd: ptysd.c
	gcc $(CFLAGS) $(CPPFLAGS) -lsocket -lnsl -o ptysd ptysd.c
