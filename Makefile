# Makefile for Fast File Transfer over UDP
# Computer Networks Project - NIT Trichy

CXX = g++
CXXFLAGS = -std=c++11 -Wall -Wextra -O2
LDFLAGS = 

# Target executables
TARGETS = sender receiver

# Source files
SENDER_SRC = sender.cpp
RECEIVER_SRC = receiver.cpp

# Header files
HEADERS = protocol.h

.PHONY: all clean test

# Build all targets
all: $(TARGETS)

# Build sender
sender: $(SENDER_SRC) $(HEADERS)
	$(CXX) $(CXXFLAGS) -o sender $(SENDER_SRC) $(LDFLAGS)
	@echo "Sender built successfully!"

# Build receiver
receiver: $(RECEIVER_SRC) $(HEADERS)
	$(CXX) $(CXXFLAGS) -o receiver $(RECEIVER_SRC) $(LDFLAGS)
	@echo "Receiver built successfully!"

# Clean build artifacts
clean:
	rm -f $(TARGETS) *.o
	@echo "Cleaned build artifacts"

# Test with small file (100KB)
test-small: all
	@echo "=== Testing with 100KB file ==="
	@{ echo "=== Test File ==="; echo "Size: 100KB"; echo ""; seq 1 10000 | while read i; do echo "Line $$i: The quick brown fox jumps over the lazy dog. Testing UDP file transfer protocol."; done; } > test_100kb.txt
	@echo "Created test file: test_100kb.txt"
	@echo "Run in separate terminals:"
	@echo "  Terminal 1: ./receiver 8080"
	@echo "  Terminal 2: ./sender 127.0.0.1 8080 test_100kb.txt 512 1000 0.05"

# Test with large file (1MB)
test-large: all
	@echo "=== Testing with 1MB file ==="
	@{ echo "=== Test File ==="; echo "Size: 1MB"; echo ""; seq 1 100000 | while read i; do echo "Line $$i: The quick brown fox jumps over the lazy dog. Testing UDP file transfer protocol."; done; } > test_1mb.txt
	@echo "Created test file: test_1mb.txt"
	@echo "Run in separate terminals:"
	@echo "  Terminal 1: ./receiver 8080"
	@echo "  Terminal 2: ./sender 127.0.0.1 8080 test_1mb.txt 512 1000 0.10"

# Generate test files
generate-tests:
	@echo "Generating test files..."
	@{ echo "=== Test File 100KB ==="; seq 1 10000 | while read i; do echo "Line $$i: The quick brown fox jumps over the lazy dog. Testing UDP."; done; } > test_100kb.txt
	@{ echo "=== Test File 1MB ==="; seq 1 100000 | while read i; do echo "Line $$i: The quick brown fox jumps over the lazy dog. Testing UDP."; done; } > test_1mb.txt
	@{ echo "=== Test File 10MB ==="; seq 1 1000000 | while read i; do echo "Line $$i: The quick brown fox jumps over the lazy dog. Testing UDP."; done; } > test_10mb.txt
	@echo "Test files created: test_100kb.txt, test_1mb.txt, test_10mb.txt"

# Help
help:
	@echo "Fast File Transfer over UDP - Makefile"
	@echo ""
	@echo "Usage:"
	@echo "  make              - Build sender and receiver"
	@echo "  make clean        - Remove build artifacts"
	@echo "  make generate-tests - Create test files"
	@echo "  make test-small   - Instructions for testing with 100KB file"
	@echo "  make test-large   - Instructions for testing with 1MB file"
	@echo "  make help         - Show this help message"
	@echo ""
	@echo "Manual usage:"
	@echo "  Receiver: ./receiver <port>"
	@echo "  Sender:   ./sender <ip> <port> <file> [rec_size] [blast_size] [loss_rate]"
	@echo ""
	@echo "Example:"
	@echo "  Terminal 1: ./receiver 8080"
	@echo "  Terminal 2: ./sender 127.0.0.1 8080 test.txt 512 1000 0.1"