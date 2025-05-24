
CFLAGS+=-I../include/ -Wall
all:client
client:client.o stat_thr.o writer_thr.o recv_thr.o
	gcc $^ -o  $@  $(CFLAGS)

clean:
	rm -rf *.o client