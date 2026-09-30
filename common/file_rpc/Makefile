CC ?= cc
AR ?= ar
CFLAGS ?= -O2 -Wall -Wextra -Werror -std=c11
BUILD ?= build
SOURCES = np_file_wire.c np_file_server.c np_file_service.c
OBJECTS = $(SOURCES:%.c=$(BUILD)/%.o)

.PHONY: all test test-linux
all: $(BUILD)/libnativepipe-file-rpc.a
$(BUILD)/%.o: %.c np_file_rpc.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -pthread -c $< -o $@
$(BUILD)/libnativepipe-file-rpc.a: $(OBJECTS)
	$(AR) rcs $@ $^
test: $(BUILD)/file-rpc-test
	$(BUILD)/file-rpc-test
$(BUILD)/file-rpc-test: tests.c $(BUILD)/libnativepipe-file-rpc.a
	$(CC) $(CFLAGS) -pthread -I. tests.c $(BUILD)/libnativepipe-file-rpc.a -o $@

test-linux: test $(BUILD)/file-open-test
	$(BUILD)/file-open-test
$(BUILD)/file-open-test: tests_open.c np_file_server.c np_file_wire.c np_file_rpc.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -DNP_FILE_TEST_OPENAT2_ENOSYS -I. tests_open.c np_file_server.c np_file_wire.c -o $@
