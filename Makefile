SRCS := main.c rs232.c telnet.c match.c queue.c
PROG := siproxy
CC   := gcc
CFLAGS := -g -O0 -Wall
LDFLAGS:= -lrt -lpthread
OBJS   := $(SRCS:.c=.o)

CURDIR := $(shell pwd)

LIBS = paho-mqtt3a
LINKFLAGS = $(addprefix -l,$(LIBS))
LIBPATH = $(CURDIR)/paho-c/build/output/
LIBINCLUDE = $(CURDIR)/paho-c/src/
LIBPATHS = $(patsubst %,$(LIBPATH)/lib%.so, $(LIBS))


$(PROG): $(OBJS)
	@$(MAKE) -C paho-c
	$(CC) $(CFLAGS) -I$(LIBINCLUDE) $(LDFLAGS) $(LINKFLAGS) -L$(LIBPATH) -o $@ $^

-include $(SRCS:.c=.d)

$.d:%.c
	@$(CC) -MM $(CFLAGS) $< | sed 's#\(.*\)\.o:#\1.o\1\1.d:#g' > $@

.PHONY: all
all: $(PROG)

clean:
	@$(MAKE) -C paho-c clean
	@rm -rf $(PROG) *.d *.o

