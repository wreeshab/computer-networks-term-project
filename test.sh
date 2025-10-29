#!/bin/bash

# Test script for Fast File Transfer over UDP
# Computer Networks Project - NIT Trichy

set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo -e "${BLUE}========================================${NC}"
echo -e "${BLUE}Fast File Transfer over UDP - Test Suite${NC}"
echo -e "${BLUE}========================================${NC}\n"

# Check if executables exist
if [ ! -f "./sender" ] || [ ! -f "./receiver" ]; then
    echo -e "${RED}Error: Executables not found. Run 'make' first.${NC}"
    exit 1
fi

# Function to generate test file
generate_file() {
    local filename=$1
    local size_kb=$2
    echo -e "${YELLOW}Generating test file: $filename ($size_kb KB)...${NC}"
    
    # Generate text file with readable content
    {
        echo "=== Fast File Transfer Test File ==="
        echo "Filename: $filename"
        echo "Size: $size_kb KB"
        echo "Generated: $(date)"
        echo "======================================="
        echo ""
        
        # Generate paragraphs of lorem ipsum style text
        local bytes_generated=0
        local target_bytes=$((size_kb * 1024))
        local line_num=1
        
        while [ $bytes_generated -lt $target_bytes ]; do
            echo "Line $line_num: The quick brown fox jumps over the lazy dog. This is a test of the file transfer protocol. Lorem ipsum dolor sit amet, consectetur adipiscing elit."
            line_num=$((line_num + 1))
            bytes_generated=$((bytes_generated + 150))
        done
    } > "$filename"
    
    echo -e "${GREEN}✓ Created $filename${NC}\n"
}

# Function to run test
run_test() {
    local test_name=$1
    local filename=$2
    local record_size=$3
    local blast_size=$4
    local loss_rate=$5
    local port=$6
    
    echo -e "${BLUE}========================================${NC}"
    echo -e "${BLUE}TEST: $test_name${NC}"
    echo -e "${BLUE}========================================${NC}"
    echo -e "File: $filename"
    echo -e "Record Size: $record_size bytes"
    echo -e "Blast Size: $blast_size records"
    echo -e "Loss Rate: $(echo "$loss_rate * 100" | bc)%"
    echo -e "${BLUE}========================================${NC}\n"
    
    # Start receiver in background
    echo -e "${YELLOW}Starting receiver on port $port...${NC}"
    ./receiver $port > receiver_output.log 2>&1 &
    RECEIVER_PID=$!
    
    # Give receiver time to start
    sleep 1
    
    # Check if receiver is running
    if ! ps -p $RECEIVER_PID > /dev/null; then
        echo -e "${RED}✗ Receiver failed to start${NC}"
        cat receiver_output.log
        return 1
    fi
    
    echo -e "${GREEN}✓ Receiver started (PID: $RECEIVER_PID)${NC}\n"
    
    # Start sender
    echo -e "${YELLOW}Starting sender...${NC}"
    ./sender 127.0.0.1 $port $filename $record_size $blast_size $loss_rate > sender_output.log 2>&1
    SENDER_EXIT=$?
    
    # Wait for receiver to finish
    sleep 2
    
    # Kill receiver if still running
    if ps -p $RECEIVER_PID > /dev/null; then
        kill $RECEIVER_PID 2>/dev/null || true
        wait $RECEIVER_PID 2>/dev/null || true
    fi
    
    echo -e "\n${YELLOW}Checking results...${NC}"
    
    # Check sender exit code
    if [ $SENDER_EXIT -ne 0 ]; then
        echo -e "${RED}✗ Sender failed${NC}"
        cat sender_output.log
        return 1
    fi
    
    # Get received filename (same as sent)
    RECEIVED_FILE=$(basename $filename)
    
    # Check if file was received
    if [ ! -f "$RECEIVED_FILE" ]; then
        echo -e "${RED}✗ File not received${NC}"
        cat receiver_output.log
        return 1
    fi
    
    # Compare files
    if cmp -s "$filename" "$RECEIVED_FILE"; then
        echo -e "${GREEN}✓ File transfer successful - files match!${NC}"
        
        # Display statistics from sender output
        echo -e "\n${BLUE}=== Statistics ===${NC}"
        grep -A 10 "=== Transfer Statistics ===" sender_output.log
        
        # Clean up received file
        rm -f "$RECEIVED_FILE"
        
        return 0
    else
        echo -e "${RED}✗ File transfer failed - files differ${NC}"
        return 1
    fi
}

# Clean up function
cleanup() {
    echo -e "\n${YELLOW}Cleaning up...${NC}"
    rm -f test_*.txt *.log
    pkill -f "./receiver" 2>/dev/null || true
    echo -e "${GREEN}✓ Cleanup complete${NC}"
}

# Trap to ensure cleanup on exit
trap cleanup EXIT

# ============================================================================
# TESTS
# ============================================================================

TESTS_PASSED=0
TESTS_FAILED=0
PORT=8080

# Test 1: Small file, no loss
echo -e "\n${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo -e "${BLUE}Test 1: Small File (10 KB), No Loss${NC}"
echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}\n"
generate_file "test_10kb.txt" 10
if run_test "Small File - No Loss" "test_10kb.txt" 512 100 0.0 $PORT; then
    ((TESTS_PASSED++))
else
    ((TESTS_FAILED++))
fi
sleep 2

# Test 2: Medium file, 5% loss
echo -e "\n${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo -e "${BLUE}Test 2: Medium File (100 KB), 5% Loss${NC}"
echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}\n"
generate_file "test_100kb.txt" 100
if run_test "Medium File - 5% Loss" "test_100kb.txt" 512 1000 0.05 $PORT; then
    ((TESTS_PASSED++))
else
    ((TESTS_FAILED++))
fi
sleep 2

# Test 3: Large file, 10% loss
echo -e "\n${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo -e "${BLUE}Test 3: Large File (1 MB), 10% Loss${NC}"
echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}\n"
generate_file "test_1mb.txt" 1024
if run_test "Large File - 10% Loss" "test_1mb.txt" 512 1000 0.10 $PORT; then
    ((TESTS_PASSED++))
else
    ((TESTS_FAILED++))
fi
sleep 2

# Test 4: Different record size (256 bytes)
echo -e "\n${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo -e "${BLUE}Test 4: 100 KB, 256-byte records, 10% Loss${NC}"
echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}\n"
if run_test "256-byte Records" "test_100kb.txt" 256 2000 0.10 $PORT; then
    ((TESTS_PASSED++))
else
    ((TESTS_FAILED++))
fi
sleep 2

# Test 5: Different record size (1024 bytes)
echo -e "\n${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo -e "${BLUE}Test 5: 100 KB, 1024-byte records, 10% Loss${NC}"
echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}\n"
if run_test "1024-byte Records" "test_100kb.txt" 1024 500 0.10 $PORT; then
    ((TESTS_PASSED++))
else
    ((TESTS_FAILED++))
fi
sleep 2

# Test 6: High loss rate (20%)
echo -e "\n${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo -e "${BLUE}Test 6: 100 KB, High Loss Rate (20%)${NC}"
echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}\n"
if run_test "High Loss Rate" "test_100kb.txt" 512 1000 0.20 $PORT; then
    ((TESTS_PASSED++))
else
    ((TESTS_FAILED++))
fi

# ============================================================================
# SUMMARY
# ============================================================================

echo -e "\n${BLUE}========================================${NC}"
echo -e "${BLUE}TEST SUMMARY${NC}"
echo -e "${BLUE}========================================${NC}"
echo -e "${GREEN}Tests Passed: $TESTS_PASSED${NC}"
echo -e "${RED}Tests Failed: $TESTS_FAILED${NC}"
echo -e "${BLUE}========================================${NC}\n"

if [ $TESTS_FAILED -eq 0 ]; then
    echo -e "${GREEN}🎉 All tests passed! Your implementation is working correctly.${NC}\n"
    exit 0
else
    echo -e "${RED}❌ Some tests failed. Check the logs above for details.${NC}\n"
    exit 1
fimake test-small
make test-large