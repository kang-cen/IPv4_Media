
CFLAGS+=-I../include/ -Wall -pthread
LDFLAGS = -pthread

all:client
client:client.o stat_thr.o writer_thr.o recv_thr.o feedback_thr.o
	gcc $^ -o  $@  $(CFLAGS)

clean:
	rm -rf *.o client