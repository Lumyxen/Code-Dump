CXX ?= g++
NATIVE_FLAGS ?= -march=native -mtune=native
CXXFLAGS ?= -std=c++23 -O3 -DNDEBUG $(NATIVE_FLAGS) -m64 -flto -Wall -Wextra -Wpedantic
LDFLAGS ?= -m64 -flto -s
TARGET := dump
SRC := dump.cpp

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) $(SRC) -o $(TARGET) $(LDFLAGS)

clean:
	rm -f $(TARGET) dump.md
