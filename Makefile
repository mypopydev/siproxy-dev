SRCS := main.c rs232.c telnet.c match.c queue.c
PROG := siproxy
CC   := gcc
CFLAGS := -g -O0 -Wall
LDFLAGS:= -lrt -pthread
OBJS   := $(SRCS:.c=.o)

$(PROG): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

-include $(SRCS:.c=.d)

$.d:%.c
	@$(CC) -MM $(CFLAGS) $< | sed 's#\(.*\)\.o:#\1.o\1\1.d:#g' > $@

.PHONY: all
all: $(PROG)

clean:
	@rm -rf $(PROG) *.d *.o

