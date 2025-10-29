#include "protocol.h"
#include <iostream>
#include <fstream>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/time.h>
#include <vector>
#include <map>
#include <chrono>

using namespace std;

// ============================================================================
// SENDER STATE MACHINE
// ============================================================================

enum SenderState {
    SENDER_START,
    DISK_ACCESS,
    FILE_ACCESS,
    FILE_HDR_CREATED,
    WAIT_1,
    CONNECTION_ESTABLISHED,
    BUFFER_WRITE,
    PACKETS_CREATED,
    BLAST_SENT,
    IS_BLAST_OVER_SENT,
    WAIT_2,
    REC_MISS_RECEIVED,
    MISSING_RECORDS_SENT,
    DISCONNECTED
};

// ============================================================================
// SENDER CLASS
// ============================================================================

class FileSender {
private:
    int sockfd;
    struct sockaddr_in receiver_addr;
    string filename;
    string output_filename;
    uint16_t record_size;
    uint32_t blast_size;
    double loss_rate;
    
    uint64_t file_size;
    uint32_t total_records;
    vector<vector<uint8_t>> file_records;  // all file records in memory
    
    Statistics stats;
    SenderState state;
    
    // Garbler: simulate packet loss
    bool should_drop_packet() {
        if (loss_rate <= 0.0) return false;
        return (rand() / (double)RAND_MAX) < loss_rate;
    }
    
    // Send packet with garbler
    bool send_packet(const uint8_t* buffer, size_t size, bool is_data_packet = false) {
        if (is_data_packet && should_drop_packet()) {
            stats.total_packets_lost++;
            return false;  // Simulate packet loss
        }
        
        ssize_t sent = sendto(sockfd, buffer, size, 0, 
                             (struct sockaddr*)&receiver_addr, sizeof(receiver_addr));
        if (sent < 0) {
            perror("sendto failed");
            return false;
        }
        stats.total_packets_sent++;
        if (is_data_packet) {
            stats.total_data_packets_sent++;
        }
        return true;
    }
    
    // Receive packet with timeout
    bool recv_packet_timeout(uint8_t* buffer, size_t& size, int timeout_sec) {
        struct timeval tv;
        tv.tv_sec = timeout_sec;
        tv.tv_usec = 0;
        setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        
        ssize_t n = recvfrom(sockfd, buffer, MAX_UDP_PAYLOAD, 0, NULL, NULL);
        if (n < 0) {
            return false;  // Timeout or error
        }
        size = n;
        return true;
    }
    
    // Load file into memory
    bool load_file() {
        ifstream input(filename, ios::binary);
        if (!input.is_open()) {
            cerr << "Error: Cannot open file " << filename << endl;
            return false;
        }
        
        // Get file size
        input.seekg(0, ios::end);
        file_size = input.tellg();
        input.seekg(0, ios::beg);
        
        // Calculate total records
        total_records = (file_size + record_size - 1) / record_size;
        
        cout << "File size: " << file_size << " bytes" << endl;
        cout << "Record size: " << record_size << " bytes" << endl;
        cout << "Total records: " << total_records << endl;
        
        // Read all records
        file_records.resize(total_records);
        for (uint32_t i = 0; i < total_records; i++) {
            file_records[i].resize(record_size, 0);  // Pad with zeros
            
            size_t bytes_to_read = record_size;
            if (i == total_records - 1) {
                // Last record might be partial
                size_t remaining = file_size % record_size;
                if (remaining != 0) {
                    bytes_to_read = remaining;
                }
            }
            
            input.read((char*)file_records[i].data(), bytes_to_read);
        }
        
        input.close();
        return true;
    }
    
    // Send FILE_HDR and wait for ACK
    bool send_file_header() {
        FileHeaderPacket hdr;
        hdr.file_size = file_size;
        hdr.record_size = record_size;
        hdr.blast_size = blast_size;
        
        // Ensure null termination
        memset(hdr.filename, 0, MAX_FILENAME_LEN);
        strncpy(hdr.filename, output_filename.c_str(), MAX_FILENAME_LEN - 1);
        hdr.filename[MAX_FILENAME_LEN - 1] = '\0';
        
        uint8_t send_buffer[1024];
        size_t size = hdr.serialize(send_buffer);
        
        cout << "Sending FILE_HDR..." << endl;
        
        // Retry loop with timeout
        uint8_t recv_buffer[MAX_UDP_PAYLOAD];
        for (int attempt = 0; attempt < 5; attempt++) {
            send_packet(send_buffer, size, false);
            
            // Wait for FILE_HDR_ACK
            size_t recv_size;
            if (recv_packet_timeout(recv_buffer, recv_size, TIMEOUT_FILE_HDR)) {
                if (recv_buffer[0] == FILE_HDR_ACK) {
                    cout << "Received FILE_HDR_ACK - Connection established!" << endl;
                    return true;
                }
            }
            cout << "Timeout waiting for FILE_HDR_ACK, retrying..." << endl;
        }
        
        cerr << "Error: Failed to establish connection" << endl;
        return false;
    }
    
    // Create data packets from records
    vector<DataPacket> create_data_packets(uint32_t start_rec, uint32_t end_rec) {
        vector<DataPacket> packets;
        
        uint32_t current_rec = start_rec;
        while (current_rec <= end_rec) {
            DataPacket pkt;
            pkt.num_segments = 0;
            
            uint32_t records_in_packet = 0;
            uint32_t segment_start = current_rec;
            
            // Pack up to MAX_RECORDS_PER_PACKET records
            while (current_rec <= end_rec && records_in_packet < MAX_RECORDS_PER_PACKET) {
                // Add record data
                pkt.data.insert(pkt.data.end(), 
                               file_records[current_rec - 1].begin(), 
                               file_records[current_rec - 1].end());
                records_in_packet++;
                current_rec++;
            }
            
            // Create segment descriptor
            pkt.segments[pkt.num_segments++] = Segment(segment_start, current_rec - 1);
            
            packets.push_back(pkt);
        }
        
        return packets;
    }
    
    // Send a blast of records
    bool send_blast(uint32_t start_rec, uint32_t end_rec, bool is_retransmission = false) {
        cout << "Sending blast: records " << start_rec << "-" << end_rec;
        if (is_retransmission) cout << " (retransmission)";
        cout << endl;
        
        // Create packets
        vector<DataPacket> packets = create_data_packets(start_rec, end_rec);
        
        // Send all packets
        uint8_t buffer[MAX_UDP_PAYLOAD];
        for (auto& pkt : packets) {
            size_t size = pkt.serialize(buffer, MAX_UDP_PAYLOAD);
            if (size > 0) {
                bool sent = send_packet(buffer, size, true);
                if (!sent && is_retransmission) {
                    stats.retransmissions++;
                }
            }
        }
        
        return true;
    }
    
    // Send IS_BLAST_OVER and wait for REC_MISS
    bool send_blast_over_and_wait(uint32_t start_rec, uint32_t end_rec, RecMissPacket& rec_miss) {
        BlastOverPacket blast_over(start_rec, end_rec);
        uint8_t send_buffer[1024];
        size_t size = blast_over.serialize(send_buffer);
        
        // Retry loop
        for (int attempt = 0; attempt < 5; attempt++) {
            send_packet(send_buffer, size, false);
            
            // Wait for REC_MISS
            uint8_t recv_buffer[MAX_UDP_PAYLOAD];
            size_t recv_size;
            if (recv_packet_timeout(recv_buffer, recv_size, TIMEOUT_BLAST_OVER)) {
                if (recv_buffer[0] == REC_MISS) {
                    rec_miss.deserialize(recv_buffer, recv_size);
                    return true;
                }
            }
            cout << "Timeout waiting for REC_MISS, retrying..." << endl;
        }
        
        cerr << "Error: Failed to receive REC_MISS" << endl;
        return false;
    }
    
    // Process one blast cycle
    bool process_blast_cycle(uint32_t start_rec, uint32_t end_rec) {
        stats.total_blasts++;
        
        // Send initial blast
        send_blast(start_rec, end_rec, false);
        
        // Loop until all records received
        while (true) {
            RecMissPacket rec_miss;
            if (!send_blast_over_and_wait(start_rec, end_rec, rec_miss)) {
                return false;
            }
            
            if (rec_miss.num_missing == 0) {
                cout << "Blast complete - all records received!" << endl;
                break;
            }
            
            cout << "Missing " << rec_miss.num_missing << " segment(s), retransmitting..." << endl;
            
            // Retransmit missing segments
            for (int i = 0; i < rec_miss.num_missing; i++) {
                send_blast(rec_miss.missing[i].start_record, 
                          rec_miss.missing[i].end_record, true);
            }
        }
        
        return true;
    }
    
    // Send disconnect
    void send_disconnect() {
        DisconnectPacket disc;
        uint8_t buffer[16];
        size_t size = disc.serialize(buffer);
        send_packet(buffer, size, false);
        cout << "Sent DISCONNECT" << endl;
    }

public:
    FileSender(const string& ip, int port, const string& fname, const string& output_fname,
               uint16_t rec_size, uint32_t b_size, double loss) 
        : filename(fname), output_filename(output_fname), record_size(rec_size), 
          blast_size(b_size), loss_rate(loss), state(SENDER_START) {
        
        // Create UDP socket
        sockfd = socket(AF_INET, SOCK_DGRAM, 0);
        if (sockfd < 0) {
            perror("Socket creation failed");
            exit(1);
        }
        
        memset(&receiver_addr, 0, sizeof(receiver_addr));
        receiver_addr.sin_family = AF_INET;
        receiver_addr.sin_port = htons(port);
        if (inet_pton(AF_INET, ip.c_str(), &receiver_addr.sin_addr) <= 0) {
            cerr << "Invalid IP address" << endl;
            exit(1);
        }
        
        srand(time(NULL));
    }
    
    ~FileSender() {
        close(sockfd);
    }
    
    bool run() {
        auto start_time = chrono::high_resolution_clock::now();
        
        cout << "\n=== File Sender Started ===" << endl;
        cout << "Loss rate: " << (loss_rate * 100) << "%" << endl;
        
        // Phase 1: Connection Setup
        state = DISK_ACCESS;
        if (!load_file()) return false;
        
        state = FILE_HDR_CREATED;
        if (!send_file_header()) return false;
        
        state = CONNECTION_ESTABLISHED;
        
        // Phase 2: Data Transfer
        uint32_t current_rec = 1;
        while (current_rec <= total_records) {
            uint32_t blast_end = min(current_rec + blast_size - 1, total_records);
            
            if (!process_blast_cycle(current_rec, blast_end)) {
                return false;
            }
            
            current_rec = blast_end + 1;
        }
        
        // Phase 3: Disconnect
        send_disconnect();
        state = DISCONNECTED;
        
        auto end_time = chrono::high_resolution_clock::now();
        chrono::duration<double> elapsed = end_time - start_time;
        stats.total_time_sec = elapsed.count();
        stats.throughput_mbps = (file_size * 8.0) / (stats.total_time_sec * 1000000.0);
        
        cout << "\n=== Transfer Complete ===" << endl;
        stats.print();
        
        return true;
    }
};

// ============================================================================
// MAIN
// ============================================================================

int main(int argc, char* argv[]) {
    if (argc < 4) {
        cerr << "Usage: " << argv[0] << " <receiver_ip> <receiver_port> <filename> [record_size] [blast_size] [loss_rate]" << endl;
        cerr << "Example: " << argv[0] << " 127.0.0.1 8080 test.txt 512 1000 0.1" << endl;
        return 1;
    }
    
    string receiver_ip = argv[1];
    int receiver_port = atoi(argv[2]);
    string filename = argv[3];
    uint16_t record_size = (argc > 4) ? atoi(argv[4]) : DEFAULT_RECORD_SIZE;
    uint32_t blast_size = (argc > 5) ? atoi(argv[5]) : DEFAULT_BLAST_SIZE;
    double loss_rate = (argc > 6) ? atof(argv[6]) : 0.0;
    
    // Extract output filename from path
    string output_filename = filename;
    size_t last_slash = filename.find_last_of("/\\");
    if (last_slash != string::npos) {
        output_filename = filename.substr(last_slash + 1);
    }
    
    // Validate parameters
    if (record_size != 256 && record_size != 512 && record_size != 1024) {
        cerr << "Error: Record size must be 256, 512, or 1024" << endl;
        return 1;
    }
    
    if (blast_size < 200 || blast_size > 10000) {
        cerr << "Error: Blast size must be between 200 and 10000" << endl;
        return 1;
    }
    
    if (loss_rate < 0.0 || loss_rate > 1.0) {
        cerr << "Error: Loss rate must be between 0.0 and 1.0" << endl;
        return 1;
    }
    
    FileSender sender(receiver_ip, receiver_port, filename, output_filename,
                     record_size, blast_size, loss_rate);
    
    if (!sender.run()) {
        cerr << "Transfer failed!" << endl;
        return 1;
    }
    
    return 0;
}