SRCS := main.c rs232.c telnet.c match.c
PROG := siproxy
CC   := gcc
CFLAGS := -g -O0 -Wall
OBJS   := $(SRCS:.c=.o)

$(PROG): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

-include $(SRCS:.c=.d)

$.d:%.c
	@$(CC) -MM $(CFLAGS) $< | sed 's#\(.*\)\.o:#\1.o\1\1.d:#g' > $@

.PHONY: all
all: $(PROG)

clean:
	@rm -rf $(PROG) *.d *.o

