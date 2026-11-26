# ECCLES MEDIA - host build
CC      ?= gcc
CSTD    ?= -std=c99
WARN    := -Wall -Wextra
INC     := -Iinclude -Isrc
SRC     := $(wildcard src/*.c)
B       := build

.PHONY: all examples test size clean
all: examples

$(B):
	mkdir -p $(B)

examples: $(B)/example_host
$(B)/example_host: examples/host/main.c $(SRC) include/eccles_media.h | $(B)
	$(CC) $(CSTD) $(WARN) -O2 $(INC) examples/host/main.c $(SRC) -o $@

# needs gcc, python3 (numpy + Pillow), ffmpeg
test:
	bash test/run_all.sh

size: | $(B)
	@for f in $(SRC); do $(CC) $(CSTD) -Os $(INC) -c $$f -o $(B)/$$(basename $$f .c).o || exit 1; done
	@size -t $(B)/em_*.o

clean:
	rm -rf $(B)
