#include "protocol.h"
#include <iostream>
#include <fstream>
#include <cstring>
#include <cstdlib>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <vector>
#include <set>
#include <chrono>
#include <iomanip>
#include <sstream>

using namespace std;

// ============================================================================
// RECEIVER STATE MACHINE
// ============================================================================

enum ReceiverState {
    RECEIVER_START,
    WAIT_FILE_HDR,
    CONNECTION_ESTABLISHED,
    WAIT_BLAST,
    BLAST_RECEIVED,
    BUFFER_WRITE,
    DISK_WRITE,
    WAIT_IS_BLAST_OVER,
    REC_MISS_CREATED,
    REC_MISS_SENT,
    LINGER,
    RECEIVER_DISCONNECTED
};

// ============================================================================
// RECEIVER CLASS
// ============================================================================

class FileReceiver {
private:
    int sockfd;
    struct sockaddr_in server_addr;
    struct sockaddr_in sender_addr;
    socklen_t sender_addr_len;
    int port;
    
    uint64_t file_size;
    uint16_t record_size;
    uint32_t blast_size;
    uint32_t total_records;
    string output_filename;
    
    vector<bool> received_records;       // Track which records received
    vector<vector<uint8_t>> record_buffer;  // Store received records
    
    ReceiverState state;
    bool connection_active;
    
    // Send packet
    bool send_packet(const uint8_t* buffer, size_t size) {
        ssize_t sent = sendto(sockfd, buffer, size, 0, 
                             (struct sockaddr*)&sender_addr, sender_addr_len);
        return (sent >= 0);
    }
    
    // Receive packet
    bool recv_packet(uint8_t* buffer, size_t& size) {
        sender_addr_len = sizeof(sender_addr);
        ssize_t n = recvfrom(sockfd, buffer, MAX_UDP_PAYLOAD, 0,
                            (struct sockaddr*)&sender_addr, &sender_addr_len);
        if (n < 0) {
            return false;
        }
        size = n;
        return true;
    }
    
    // Receive packet with timeout
    bool recv_packet_timeout(uint8_t* buffer, size_t& size, int timeout_sec) {
        struct timeval tv;
        tv.tv_sec = timeout_sec;
        tv.tv_usec = 0;
        setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        
        return recv_packet(buffer, size);
    }
    
    // Send FILE_HDR_ACK
    void send_file_hdr_ack() {
        FileHeaderAckPacket ack;
        uint8_t buffer[16];
        size_t size = ack.serialize(buffer);
        send_packet(buffer, size);
        cout << "Sent FILE_HDR_ACK" << endl;
    }
    
    // Process FILE_HDR
    bool process_file_hdr(const uint8_t* buffer, size_t size) {
        FileHeaderPacket hdr;
        hdr.deserialize(buffer);
        
        file_size = hdr.file_size;
        record_size = hdr.record_size;
        blast_size = hdr.blast_size;
        output_filename = hdr.filename;
        
        total_records = (file_size + record_size - 1) / record_size;
        
        cout << "\n=== File Header Received ===" << endl;
        cout << "Filename: " << output_filename << endl;
        cout << "File size: " << file_size << " bytes" << endl;
        cout << "Record size: " << record_size << " bytes" << endl;
        cout << "Blast size: " << blast_size << " records" << endl;
        cout << "Total records: " << total_records << endl;
        
        // Initialize buffers
        received_records.resize(total_records + 1, false);  // 1-indexed
        record_buffer.resize(total_records + 1);
        
        send_file_hdr_ack();
        return true;
    }
    
    // Process DATA packet
    void process_data_packet(const uint8_t* buffer, size_t size) {
        DataPacket pkt;
        pkt.deserialize(buffer, size);
        
        // Extract records from packet
        size_t data_offset = 0;
        for (int i = 0; i < pkt.num_segments; i++) {
            uint32_t start = pkt.segments[i].start_record;
            uint32_t end = pkt.segments[i].end_record;
            
            for (uint32_t rec = start; rec <= end; rec++) {
                if (rec >= 1 && rec <= total_records) {
                    record_buffer[rec].resize(record_size);
                    memcpy(record_buffer[rec].data(), 
                          pkt.data.data() + data_offset, 
                          record_size);
                    received_records[rec] = true;
                    data_offset += record_size;
                }
            }
        }
    }
    
    // Find missing records in range
    vector<Segment> find_missing_records(uint32_t start_rec, uint32_t end_rec) {
        vector<Segment> missing;
        
        uint32_t segment_start = 0;
        bool in_segment = false;
        
        for (uint32_t rec = start_rec; rec <= end_rec; rec++) {
            if (!received_records[rec]) {
                if (!in_segment) {
                    segment_start = rec;
                    in_segment = true;
                }
            } else {
                if (in_segment) {
                    missing.push_back(Segment(segment_start, rec - 1));
                    in_segment = false;
                }
            }
        }
        
        // Close last segment if needed
        if (in_segment) {
            missing.push_back(Segment(segment_start, end_rec));
        }
        
        return missing;
    }
    
    // Send REC_MISS
    void send_rec_miss(uint32_t start_rec, uint32_t end_rec) {
        vector<Segment> missing = find_missing_records(start_rec, end_rec);
        
        RecMissPacket rec_miss;
        rec_miss.num_missing = min((int)missing.size(), MAX_MISSING_SEGMENTS);
        for (int i = 0; i < rec_miss.num_missing; i++) {
            rec_miss.missing[i] = missing[i];
        }
        
        uint8_t buffer[MAX_UDP_PAYLOAD];
        size_t size = rec_miss.serialize(buffer, MAX_UDP_PAYLOAD);
        send_packet(buffer, size);
        
        if (rec_miss.num_missing == 0) {
            cout << "Sent REC_MISS: empty (all received)" << endl;
        } else {
            cout << "Sent REC_MISS: " << rec_miss.num_missing << " missing segment(s)" << endl;
        }
    }
    
    // Write received file to disk
    bool write_file_to_disk() {
        // Create timestamp string
        auto now = chrono::system_clock::now();
        auto now_time_t = chrono::system_clock::to_time_t(now);
        auto now_ms = chrono::duration_cast<chrono::milliseconds>(
            now.time_since_epoch()) % 1000;
        
        struct tm* timeinfo = localtime(&now_time_t);
        
        // Format: YYYYMMDD-H:MM-AM/PM (e.g., 20251029-9:50-PM)
        stringstream timestamp_ss;
        timestamp_ss << put_time(timeinfo, "%Y%m%d-")
                    << (timeinfo->tm_hour % 12 == 0 ? 12 : timeinfo->tm_hour % 12)
                    << put_time(timeinfo, ":%M-%p");
        string timestamp = timestamp_ss.str();
        
        // Create directory structure: received_files/YYYYMMDD-H:MM-AM/PM/
        string dir_path = "received_files/" + timestamp;
        
        // Create directories recursively
        mkdir("received_files", 0755);  // Create parent directory
        if (mkdir(dir_path.c_str(), 0755) != 0 && errno != EEXIST) {
            cerr << "Error: Cannot create directory " << dir_path << endl;
            return false;
        }
        
        // Full output path
        string full_output_path = dir_path + "/" + output_filename;
        
        cout << "\nWriting file to disk: " << full_output_path << endl;
        
        ofstream output(full_output_path, ios::binary);
        if (!output.is_open()) {
            cerr << "Error: Cannot create output file" << endl;
            return false;
        }
        
        for (uint32_t rec = 1; rec <= total_records; rec++) {
            if (!received_records[rec]) {
                cerr << "Error: Missing record " << rec << endl;
                output.close();
                return false;
            }
            
            size_t bytes_to_write = record_size;
            if (rec == total_records) {
                // Last record - only write actual file bytes
                size_t remaining = file_size % record_size;
                if (remaining != 0) {
                    bytes_to_write = remaining;
                }
            }
            
            output.write((char*)record_buffer[rec].data(), bytes_to_write);
        }
        
        output.close();
        cout << "File written successfully to: " << full_output_path << endl;
        return true;
    }

public:
    FileReceiver(int p) : port(p), state(RECEIVER_START), connection_active(false) {
        // Create UDP socket
        sockfd = socket(AF_INET, SOCK_DGRAM, 0);
        if (sockfd < 0) {
            perror("Socket creation failed");
            exit(1);
        }
        
        memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_addr.s_addr = INADDR_ANY;
        server_addr.sin_port = htons(port);
        
        if (bind(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
            perror("Bind failed");
            exit(1);
        }
        
        cout << "Receiver listening on port " << port << endl;
    }
    
    ~FileReceiver() {
        close(sockfd);
    }
    
    bool run() {
        uint8_t buffer[MAX_UDP_PAYLOAD];
        size_t size;
        
        state = WAIT_FILE_HDR;
        
        // Phase 1: Wait for FILE_HDR
        while (state == WAIT_FILE_HDR) {
            if (recv_packet(buffer, size)) {
                if (buffer[0] == FILE_HDR) {
                    process_file_hdr(buffer, size);
                    state = CONNECTION_ESTABLISHED;
                    connection_active = true;
                }
            }
        }
        
        // Phase 2: Receive data
        uint32_t expected_blast_start = 1;
        
        while (connection_active) {
            state = WAIT_BLAST;
            
            // Receive packets until IS_BLAST_OVER or DISCONNECT
            while (true) {
                if (!recv_packet_timeout(buffer, size, 10)) {
                    continue;  // Timeout, keep waiting
                }
                
                PacketType type = (PacketType)buffer[0];
                
                if (type == DATA) {
                    process_data_packet(buffer, size);
                }
                else if (type == IS_BLAST_OVER) {
                    state = WAIT_IS_BLAST_OVER;
                    BlastOverPacket blast_over;
                    blast_over.deserialize(buffer);
                    
                    cout << "\nReceived IS_BLAST_OVER(" << blast_over.start_record 
                         << ", " << blast_over.end_record << ")" << endl;
                    
                    // Send REC_MISS
                    send_rec_miss(blast_over.start_record, blast_over.end_record);
                    
                    // Check if this blast is complete
                    vector<Segment> missing = find_missing_records(
                        blast_over.start_record, blast_over.end_record);
                    
                    if (missing.empty()) {
                        expected_blast_start = blast_over.end_record + 1;
                        
                        // Check if all records received
                        if (blast_over.end_record >= total_records) {
                            cout << "\nAll data received!" << endl;
                            break;  // Exit inner loop
                        }
                    }
                }
                else if (type == DISCONNECT) {
                    cout << "\nReceived DISCONNECT" << endl;
                    connection_active = false;
                    break;
                }
                else if (type == FILE_HDR) {
                    // Sender retransmitting FILE_HDR, resend ACK
                    send_file_hdr_ack();
                }
            }
            
            if (!connection_active || expected_blast_start > total_records) {
                break;
            }
        }
        
        // Phase 3: Linger
        state = LINGER;
        cout << "\nEntering linger state for " << LINGER_TIME << " seconds..." << endl;
        
        auto linger_start = chrono::steady_clock::now();
        while (true) {
            auto now = chrono::steady_clock::now();
            auto elapsed = chrono::duration_cast<chrono::seconds>(now - linger_start).count();
            if (elapsed >= LINGER_TIME) {
                break;
            }
            
            // Still respond to IS_BLAST_OVER during linger
            if (recv_packet_timeout(buffer, size, 1)) {
                PacketType type = (PacketType)buffer[0];
                if (type == IS_BLAST_OVER) {
                    BlastOverPacket blast_over;
                    blast_over.deserialize(buffer);
                    send_rec_miss(blast_over.start_record, blast_over.end_record);
                }
            }
        }
        
        state = RECEIVER_DISCONNECTED;
        
        // Write file to disk
        if (!write_file_to_disk()) {
            return false;
        }
        
        cout << "\n=== Transfer Complete ===" << endl;
        return true;
    }
};

// ============================================================================
// MAIN
// ============================================================================

int main(int argc, char* argv[]) {
    if (argc < 2) {
        cerr << "Usage: " << argv[0] << " <port>" << endl;
        cerr << "Example: " << argv[0] << " 8080" << endl;
        return 1;
    }
    
    int port = atoi(argv[1]);
    
    FileReceiver receiver(port);
    
    if (!receiver.run()) {
        cerr << "Transfer failed!" << endl;
        return 1;
    }
    
    return 0;
}   