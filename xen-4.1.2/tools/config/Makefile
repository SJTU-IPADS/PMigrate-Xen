XEN_ROOT	= $(CURDIR)/../..
include $(XEN_ROOT)/tools/Rules.mk

CFLAGS += -Werror
PROGRAMS = para-config.o read_config.o

.PHONY: all
all: build

.PHONY: build
build: $(PROGRAMS)

read_config.o: read_config.c

para-config.o: para-config.c read_config.c

.PHONY: install
install: build

.PHONY: clean
clean:
	rm *.o 
