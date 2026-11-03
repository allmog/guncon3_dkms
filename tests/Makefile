CFLAGS ?= -O2
CFLAGS += -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable -Wno-unused-function -fsanitize=address,undefined

test: test_proto
	./test_proto

test_proto: test_proto.c ref_decode.h guncon3_encrypt.h ../src/guncon3_proto.h
	$(CC) $(CFLAGS) -o $@ test_proto.c

clean:
	rm -f test_proto

.PHONY: test clean
