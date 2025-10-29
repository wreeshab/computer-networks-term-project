#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <cstdint>
#include <cstring>
#include <vector>
#include <bits/stdc++.h>

// ============================================================================
// CONSTANTS
// ============================================================================

const int DEFAULT_RECORD_SIZE = 512;
const int DEFAULT_BLAST_SIZE = 1000;
const int MAX_RECORDS_PER_PACKET = 16;
const int TIMEOUT_FILE_HDR = 2;          // seconds
const int TIMEOUT_BLAST_OVER = 2;        // seconds
const int LINGER_TIME = 5;               // seconds
const int MAX_FILENAME_LEN = 256;
const int MAX_MISSING_SEGMENTS = 1000;
const int MAX_UDP_PAYLOAD = 65000;       // safe UDP payload size

// ============================================================================
// PACKET TYPES
// ============================================================================

enum PacketType : uint8_t {
    FILE_HDR = 1,
    FILE_HDR_ACK = 2,
    DATA = 3,
    IS_BLAST_OVER = 4,
    REC_MISS = 5,
    DISCONNECT = 6
};

// ============================================================================
// SEGMENT STRUCTURE
// ============================================================================

struct Segment {
    uint32_t start_record;
    uint32_t end_record;
    
    Segment() : start_record(0), end_record(0) {}
    Segment(uint32_t s, uint32_t e) : start_record(s), end_record(e) {}
};

// ============================================================================
// FILE HEADER PACKET
// ============================================================================

struct FileHeaderPacket {
    uint8_t type;                       // FILE_HDR
    uint64_t file_size;                 // total file size in bytes
    uint16_t record_size;               // 256, 512, or 1024
    uint32_t blast_size;                // M records per blast
    char filename[MAX_FILENAME_LEN];    // output filename
    
    FileHeaderPacket() : type(FILE_HDR), file_size(0), record_size(0), blast_size(0) {
        memset(filename, 0, MAX_FILENAME_LEN);
    }
    
    size_t serialize(uint8_t* buffer) const {
        size_t offset = 0;
        buffer[offset++] = type;
        memcpy(buffer + offset, &file_size, sizeof(file_size));
        offset += sizeof(file_size);
        memcpy(buffer + offset, &record_size, sizeof(record_size));
        offset += sizeof(record_size);
        memcpy(buffer + offset, &blast_size, sizeof(blast_size));
        offset += sizeof(blast_size);
        memcpy(buffer + offset, filename, MAX_FILENAME_LEN);
        offset += MAX_FILENAME_LEN;
        return offset;
    }
    
    size_t deserialize(const uint8_t* buffer) {
        size_t offset = 0;
        type = buffer[offset++];
        memcpy(&file_size, buffer + offset, sizeof(file_size));
        offset += sizeof(file_size);
        memcpy(&record_size, buffer + offset, sizeof(record_size));
        offset += sizeof(record_size);
        memcpy(&blast_size, buffer + offset, sizeof(blast_size));
        offset += sizeof(blast_size);
        memcpy(filename, buffer + offset, MAX_FILENAME_LEN);
        offset += MAX_FILENAME_LEN;
        return offset;
    }
};

// ============================================================================
// FILE HEADER ACK PACKET
// ============================================================================

struct FileHeaderAckPacket {
    uint8_t type;  // FILE_HDR_ACK
    
    FileHeaderAckPacket() : type(FILE_HDR_ACK) {}
    
    size_t serialize(uint8_t* buffer) const {
        buffer[0] = type;
        return 1;
    }
    
    size_t deserialize(const uint8_t* buffer) {
        type = buffer[0];
        return 1;
    }
};

// ============================================================================
// DATA PACKET
// ============================================================================

struct DataPacket {
    uint8_t type;                           // DATA
    uint8_t num_segments;                   // number of segments (1-16)
    Segment segments[MAX_RECORDS_PER_PACKET]; // segment descriptors
    std::vector<uint8_t> data;              // actual record data
    
    DataPacket() : type(DATA), num_segments(0) {}
    
    size_t serialize(uint8_t* buffer, size_t buffer_size) const {
        size_t offset = 0;
        
        if (offset + 1 > buffer_size) return 0;
        buffer[offset++] = type;
        
        if (offset + 1 > buffer_size) return 0;
        buffer[offset++] = num_segments;
        
        // Serialize segments
        for (int i = 0; i < num_segments; i++) {
            if (offset + sizeof(uint32_t) * 2 > buffer_size) return 0;
            memcpy(buffer + offset, &segments[i].start_record, sizeof(uint32_t));
            offset += sizeof(uint32_t);
            memcpy(buffer + offset, &segments[i].end_record, sizeof(uint32_t));
            offset += sizeof(uint32_t);
        }
        
        // Serialize data
        if (offset + data.size() > buffer_size) return 0;
        memcpy(buffer + offset, data.data(), data.size());
        offset += data.size();
        
        return offset;
    }
    
    size_t deserialize(const uint8_t* buffer, size_t buffer_size) {
        size_t offset = 0;
        
        if (offset + 1 > buffer_size) return 0;
        type = buffer[offset++];
        
        if (offset + 1 > buffer_size) return 0;
        num_segments = buffer[offset++];
        
        // Deserialize segments
        for (int i = 0; i < num_segments; i++) {
            if (offset + sizeof(uint32_t) * 2 > buffer_size) return 0;
            memcpy(&segments[i].start_record, buffer + offset, sizeof(uint32_t));
            offset += sizeof(uint32_t);
            memcpy(&segments[i].end_record, buffer + offset, sizeof(uint32_t));
            offset += sizeof(uint32_t);
        }
        
        // Deserialize data (rest of buffer)
        if (buffer_size > offset) {
            data.resize(buffer_size - offset);
            memcpy(data.data(), buffer + offset, buffer_size - offset);
            offset = buffer_size;
        }
        
        return offset;
    }
};

// ============================================================================
// IS_BLAST_OVER PACKET
// ============================================================================

struct BlastOverPacket {
    uint8_t type;            // IS_BLAST_OVER
    uint32_t start_record;   // M_st
    uint32_t end_record;     // M_fin
    
    BlastOverPacket() : type(IS_BLAST_OVER), start_record(0), end_record(0) {}
    BlastOverPacket(uint32_t s, uint32_t e) : type(IS_BLAST_OVER), start_record(s), end_record(e) {}
    
    size_t serialize(uint8_t* buffer) const {
        size_t offset = 0;
        buffer[offset++] = type;
        memcpy(buffer + offset, &start_record, sizeof(start_record));
        offset += sizeof(start_record);
        memcpy(buffer + offset, &end_record, sizeof(end_record));
        offset += sizeof(end_record);
        return offset;
    }
    
    size_t deserialize(const uint8_t* buffer) {
        size_t offset = 0;
        type = buffer[offset++];
        memcpy(&start_record, buffer + offset, sizeof(start_record));
        offset += sizeof(start_record);
        memcpy(&end_record, buffer + offset, sizeof(end_record));
        offset += sizeof(end_record);
        return offset;
    }
};

// ============================================================================
// REC_MISS PACKET
// ============================================================================

struct RecMissPacket {
    uint8_t type;                               // REC_MISS
    uint16_t num_missing;                       // count of missing segments
    Segment missing[MAX_MISSING_SEGMENTS];      // missing segments
    
    RecMissPacket() : type(REC_MISS), num_missing(0) {}
    
    size_t serialize(uint8_t* buffer, size_t buffer_size) const {
        size_t offset = 0;
        
        if (offset + 1 > buffer_size) return 0;
        buffer[offset++] = type;
        
        if (offset + sizeof(uint16_t) > buffer_size) return 0;
        memcpy(buffer + offset, &num_missing, sizeof(num_missing));
        offset += sizeof(num_missing);
        
        // Serialize missing segments
        for (int i = 0; i < num_missing; i++) {
            if (offset + sizeof(uint32_t) * 2 > buffer_size) return 0;
            memcpy(buffer + offset, &missing[i].start_record, sizeof(uint32_t));
            offset += sizeof(uint32_t);
            memcpy(buffer + offset, &missing[i].end_record, sizeof(uint32_t));
            offset += sizeof(uint32_t);
        }
        
        return offset;
    }
    
    size_t deserialize(const uint8_t* buffer, size_t buffer_size) {
        size_t offset = 0;
        
        if (offset + 1 > buffer_size) return 0;
        type = buffer[offset++];
        
        if (offset + sizeof(uint16_t) > buffer_size) return 0;
        memcpy(&num_missing, buffer + offset, sizeof(num_missing));
        offset += sizeof(num_missing);
        
        // Deserialize missing segments
        for (int i = 0; i < num_missing && i < MAX_MISSING_SEGMENTS; i++) {
            if (offset + sizeof(uint32_t) * 2 > buffer_size) return 0;
            memcpy(&missing[i].start_record, buffer + offset, sizeof(uint32_t));
            offset += sizeof(uint32_t);
            memcpy(&missing[i].end_record, buffer + offset, sizeof(uint32_t));
            offset += sizeof(uint32_t);
        }
        
        return offset;
    }
};

// ============================================================================
// DISCONNECT PACKET
// ============================================================================

struct DisconnectPacket {
    uint8_t type;  // DISCONNECT
    
    DisconnectPacket() : type(DISCONNECT) {}
    
    size_t serialize(uint8_t* buffer) const {
        buffer[0] = type;
        return 1;
    }
    
    size_t deserialize(const uint8_t* buffer) {
        type = buffer[0];
        return 1;
    }
};

// ============================================================================
// STATISTICS STRUCTURE
// ============================================================================

struct Statistics {
    uint32_t total_packets_sent;
    uint32_t total_data_packets_sent;
    uint32_t total_packets_lost;
    uint32_t retransmissions;
    uint32_t total_blasts;
    double throughput_mbps;
    double total_time_sec;
    
    Statistics() : total_packets_sent(0), total_data_packets_sent(0), 
                   total_packets_lost(0), retransmissions(0), total_blasts(0),
                   throughput_mbps(0.0), total_time_sec(0.0) {}
    
    void print() const {
        printf("\n=== Transfer Statistics ===\n");
        printf("Total packets sent: %u\n", total_packets_sent);
        printf("Data packets sent: %u\n", total_data_packets_sent);
        printf("Packets lost: %u (%.2f%%)\n", total_packets_lost, 
               total_data_packets_sent > 0 ? (total_packets_lost * 100.0 / total_data_packets_sent) : 0.0);
        printf("Retransmissions: %u\n", retransmissions);
        printf("Total blasts: %u\n", total_blasts);
        printf("Total time: %.3f seconds\n", total_time_sec);
        printf("Throughput: %.2f Mbps\n", throughput_mbps);
        printf("===========================\n");
    }
};

#endif // PROTOCOL_H