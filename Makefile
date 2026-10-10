# Usage:  make            (build ./bpt)
#         make test       (self-tests: default 4 KB pages AND a tiny-page stress build)
#         make KEY_LEN=16 PAYLOAD_LEN=100   (if your CSV has longer keys / payloads)
CXX      ?= g++
KEY_LEN      ?= 8
PAYLOAD_LEN  ?= 64
PAGE_SIZE    ?= 4096
CXXFLAGS = -O2 -std=c++17 -Wall -Wextra -DBPT_KEY_LEN=$(KEY_LEN) -DBPT_PAYLOAD_LEN=$(PAYLOAD_LEN) -DBPT_PAGE_SIZE=$(PAGE_SIZE)

ifeq ($(OS),Windows_NT)
    LDFLAGS = -lpsapi
    TARGET  = bpt.exe
else
    LDFLAGS =
    TARGET  = bpt
endif

all: $(TARGET)

$(TARGET): src/main.cpp src/bptree.hpp src/stores.hpp
	$(CXX) $(CXXFLAGS) -o $(TARGET) src/main.cpp $(LDFLAGS)

# Second build uses 256-byte pages (leaf = 3 records, internal = 20 keys) so that
# the tree gets 6+ levels deep and every split path is exercised heavily.
test: $(TARGET)
	./$(TARGET) test
	$(CXX) -O1 -g -fsanitize=address,undefined -std=c++17 -DBPT_PAGE_SIZE=256 -DBPT_KEY_LEN=$(KEY_LEN) -DBPT_PAYLOAD_LEN=$(PAYLOAD_LEN) -o bpt_stress src/main.cpp
	./bpt_stress test

gen: tools/gen_dataset.cpp
	$(CXX) -O2 -std=c++17 -o gen_dataset tools/gen_dataset.cpp

clean:
	rm -f bpt bpt.exe bpt_stress gen_dataset index_disk.db index_buf.db test_*.db

.PHONY: all test gen clean
