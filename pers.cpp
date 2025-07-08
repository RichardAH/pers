#include <iostream>
#include <fstream>
#include <string>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/prctl.h>
#include <fcntl.h>
#include <signal.h>
#include <filesystem>
#include <cstring>
#include <sstream>
#include <chrono>
#include <thread>
#include <unordered_map>

namespace fs = std::filesystem;

const char* DAEMON_NAME = "persistent_daemon";
const char* FIFO_REQUEST = "/tmp/persistent_daemon_req";
const char* FIFO_RESPONSE = "/tmp/persistent_daemon_resp";
const char* PID_FILE = "/tmp/persistent_daemon.pid";

// Check if daemon is running
bool is_daemon_running() {
    if (!fs::exists(PID_FILE)) {
        return false;
    }
    
    std::ifstream pid_file(PID_FILE);
    pid_t pid;
    pid_file >> pid;
    pid_file.close();
    
    // Check if process exists
    return kill(pid, 0) == 0;
}

// Create daemon process
void daemonize() {
    // Fork off the parent process
    pid_t pid = fork();
    if (pid < 0) {
        exit(EXIT_FAILURE);
    }
    if (pid > 0) {
        exit(EXIT_SUCCESS); // Parent exits
    }
    
    // Child process becomes session leader
    if (setsid() < 0) {
        exit(EXIT_FAILURE);
    }
    
    // Fork again to ensure we can't reacquire a terminal
    pid = fork();
    if (pid < 0) {
        exit(EXIT_FAILURE);
    }
    if (pid > 0) {
        exit(EXIT_SUCCESS);
    }
    
    // Change working directory
    chdir("/");
    
    // Close file descriptors
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
    
    // Open null devices
    open("/dev/null", O_RDONLY); // stdin
    open("/dev/null", O_RDWR);   // stdout
    open("/dev/null", O_RDWR);   // stderr
    
    // Write PID file
    std::ofstream pid_file(PID_FILE);
    pid_file << getpid();
    pid_file.close();
}

// Daemon main loop
void daemon_main() {
    // Change process name
    prctl(PR_SET_NAME, DAEMON_NAME, 0, 0, 0);
    
    // Create named pipes if they don't exist
    unlink(FIFO_REQUEST);
    unlink(FIFO_RESPONSE);
    mkfifo(FIFO_REQUEST, 0666);
    mkfifo(FIFO_RESPONSE, 0666);
    
    // Simulated large state (in real app, this could be a database, cache, etc.)
    std::unordered_map<std::string, std::string> state;
    int query_count = 0;
    auto start_time = std::chrono::steady_clock::now();
    
    // Initialize some state
    state["version"] = "1.0";
    state["status"] = "running";
    
    while (true) {
        // Open request pipe for reading (blocks until client connects)
        int req_fd = open(FIFO_REQUEST, O_RDONLY);
        if (req_fd < 0) continue;
        
        // Read request
        char buffer[1024] = {0};
        ssize_t bytes = read(req_fd, buffer, sizeof(buffer) - 1);
        close(req_fd);
        
        if (bytes <= 0) continue;
        
        std::string request(buffer);
        std::string response;
        
        // Process request
        if (request == "STATUS") {
            auto uptime = std::chrono::steady_clock::now() - start_time;
            auto seconds = std::chrono::duration_cast<std::chrono::seconds>(uptime).count();
            response = "Status: Running\n";
            response += "Queries: " + std::to_string(++query_count) + "\n";
            response += "Uptime: " + std::to_string(seconds) + " seconds\n";
            response += "State entries: " + std::to_string(state.size()) + "\n";
        }
        else if (request.substr(0, 4) == "SET ") {
            std::istringstream iss(request.substr(4));
            std::string key, value;
            if (iss >> key && std::getline(iss, value)) {
                value = value.substr(1); // Remove leading space
                state[key] = value;
                response = "OK: Set " + key + " = " + value;
            } else {
                response = "ERROR: Invalid SET command";
            }
        }
        else if (request.substr(0, 4) == "GET ") {
            std::string key = request.substr(4);
            key.erase(key.find_last_not_of(" \n\r\t") + 1); // Trim
            auto it = state.find(key);
            if (it != state.end()) {
                response = "VALUE: " + it->second;
            } else {
                response = "ERROR: Key not found";
            }
        }
        else if (request == "SHUTDOWN") {
            response = "Shutting down...";
            // Send response before shutting down
            int resp_fd = open(FIFO_RESPONSE, O_WRONLY);
            if (resp_fd >= 0) {
                write(resp_fd, response.c_str(), response.length());
                close(resp_fd);
            }
            break;
        }
        else {
            response = "ERROR: Unknown command. Use STATUS, GET <key>, SET <key> <value>, or SHUTDOWN";
        }
        
        // Send response
        int resp_fd = open(FIFO_RESPONSE, O_WRONLY);
        if (resp_fd >= 0) {
            write(resp_fd, response.c_str(), response.length());
            close(resp_fd);
        }
    }
    
    // Cleanup
    unlink(FIFO_REQUEST);
    unlink(FIFO_RESPONSE);
    unlink(PID_FILE);
}

// Client function to send requests to daemon
std::string send_request(const std::string& request) {
    // Open request pipe for writing
    int req_fd = open(FIFO_REQUEST, O_WRONLY);
    if (req_fd < 0) {
        return "ERROR: Cannot connect to daemon";
    }
    
    // Send request
    write(req_fd, request.c_str(), request.length());
    close(req_fd);
    
    // Open response pipe for reading
    int resp_fd = open(FIFO_RESPONSE, O_RDONLY);
    if (resp_fd < 0) {
        return "ERROR: Cannot read response";
    }
    
    // Read response
    char buffer[1024] = {0};
    ssize_t bytes = read(resp_fd, buffer, sizeof(buffer) - 1);
    close(resp_fd);
    
    if (bytes > 0) {
        return std::string(buffer, bytes);
    }
    return "ERROR: No response";
}

int main(int argc, char* argv[]) {
    // Check if we're being run as daemon
    if (argc > 1 && std::string(argv[1]) == "--daemon") {
        daemonize();
        daemon_main();
        return 0;
    }
    
    // Client mode
    std::cout << "Client Process Started (PID: " << getpid() << ")\n";
    
    // Check if daemon is running, start if not
    if (!is_daemon_running()) {
        std::cout << "Starting daemon process...\n";
        
        pid_t pid = fork();
        if (pid == 0) {
            // Child: exec ourselves with --daemon flag
            execl(argv[0], argv[0], "--daemon", nullptr);
            exit(EXIT_FAILURE); // exec failed
        } else if (pid > 0) {
            // Parent: wait a bit for daemon to start
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        } else {
            std::cerr << "Failed to fork daemon\n";
            return 1;
        }
    }
    
    // Interactive client loop
    std::cout << "\nConnected to daemon. Commands:\n";
    std::cout << "  STATUS           - Get daemon status\n";
    std::cout << "  GET <key>        - Get value from daemon state\n";
    std::cout << "  SET <key> <val>  - Set value in daemon state\n";
    std::cout << "  SHUTDOWN         - Shutdown daemon\n";
    std::cout << "  EXIT             - Exit client only\n\n";
    
    std::string line;
    while (std::cout << "> " && std::getline(std::cin, line)) {
        if (line == "EXIT" || line == "exit") {
            break;
        }
        
        if (line.empty()) continue;
        
        std::string response = send_request(line);
        std::cout << response << "\n";
        
        if (line == "SHUTDOWN") {
            break;
        }
    }
    
    std::cout << "Client exiting.\n";
    return 0;
}
