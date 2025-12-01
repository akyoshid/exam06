#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <strings.h>
#include <stdlib.h>
#include <signal.h>

#include <string>
#include <iostream>
#include <fstream>
#include <sstream>
#include <map>
#include <vector>

volatile sig_atomic_t g_running = 1;

class Server {
 public:
    Server(char* port, char* path);
    ~Server();
    void setup();
    void run();

 private:
    // var
    int port_;
    char *path_;
    int serverFd_;
    int epollFd_;
    std::map<int, std::string> clients_;
    std::map<std::string, std::string> database_;

    // helper
    void exit_with_error(const std::string& cause);

    // fd
    void set_nonblocking(int fd);
    void register_epoll(int fd);
    void unregister_epoll(int fd);

    // connection
    void handle_new_connection();
    void handle_disconnection(int fd);

    // database
    void load_database();
    void save_database();

    // recv data & process command
    std::vector<std::string> process_tokens(std::string command);
    std::string process_command(std::string line);
    void process_commands(int fd);
    void recv_data(int fd);

    // = delete
    Server();
    Server(const Server& src);
    Server& operator=(const Server& src);
};

/////////////////////////////////////////////
// constructor & destructor
/////////////////////////////////////////////
Server::Server(char* port, char* path)
    : port_(atoi(port)), path_(path), serverFd_(-1), epollFd_(-1) {
}

Server::~Server() {
    if (epollFd_ != -1)
        close(epollFd_);
    for (std::map<int, std::string>::iterator it = clients_.begin();
        it != clients_.end(); ++it) {
        close(it->first);
    }
    if (serverFd_ != -1)
        close(serverFd_);
}

/////////////////////////////////////////////
// helper
/////////////////////////////////////////////
void Server::exit_with_error(const std::string& cause) {
    std::cerr << "Fatal error: " << cause << std::endl;
    exit(1);
}

void signal_handler(int signum) {
    (void)signum;
    g_running = 0;
}

/////////////////////////////////////////////
// fd
/////////////////////////////////////////////
void Server::set_nonblocking(int fd) {
    int flag = fcntl(fd, F_GETFL, 0);
    if (flag < 0)
        exit_with_error("fcntl");
    if (fcntl(fd, F_SETFL, flag | O_NONBLOCK) < 0)
        exit_with_error("fcntl");
}

void Server::register_epoll(int fd) {
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = fd;
    if (epoll_ctl(epollFd_, EPOLL_CTL_ADD, fd, &ev) < 0)
        exit_with_error("register_epoll");
}

void Server::unregister_epoll(int fd) {
    if (epoll_ctl(epollFd_, EPOLL_CTL_DEL, fd, NULL) < 0)
        exit_with_error("unregister_epoll");
}

/////////////////////////////////////////////
// connection
/////////////////////////////////////////////
void Server::handle_new_connection() {
    while (1) {
        int client_fd = accept(serverFd_, NULL, NULL);
        if (client_fd >= 0) {
            set_nonblocking(client_fd);
            register_epoll(client_fd);
            clients_[client_fd] = "";
            // std::cerr << "DEBUG: connect: fd " << client_fd << std::endl;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            else
                exit_with_error("accept");
        }
    }
}

void Server::handle_disconnection(int fd) {
    // std::cerr << "DEBUG: disconnect: fd " << fd << std::endl;
    clients_.erase(fd);
    unregister_epoll(fd);
    close(fd);
}

/////////////////////////////////////////////
// database
/////////////////////////////////////////////
void Server::load_database() {
    std::ifstream ifs(path_);
    if (!ifs.is_open())
        return;
    std::string line;
    while (std::getline(ifs, line)) {
        std::istringstream iss(line);
        std::string key;
        std::string value;
        if (iss >> key >> value)
            database_[key] = value;
    }
    ifs.close();
}

void Server::save_database() {
    std::ofstream ofs(path_);
    if (!ofs.is_open())
        exit_with_error("ofs");
    for (std::map<std::string, std::string>::iterator it = database_.begin();
        it != database_.end(); ++it) {
        ofs << it->first << " " << it->second << "\n";
    }
    ofs.close();
}

/////////////////////////////////////////////
// recv data & process command
/////////////////////////////////////////////
std::vector<std::string> Server::process_tokens(std::string command) {
    std::istringstream iss(command);
    std::vector<std::string> tokens;
    std::string token;
    while (iss >> token)
        tokens.push_back(token);
    return tokens;
}

std::string Server::process_command(std::string command) {
    std::vector<std::string> tokens = process_tokens(command);
    if (tokens.empty()) {
        return "2\n";
    } else if (tokens[0] == "POST" && tokens.size() == 3) {
        database_[tokens[1]] = tokens[2];
        return "0\n";
    } else if (tokens[0] == "GET" && tokens.size() == 2) {
        if (database_.count(tokens[1]))
            return "0 " + database_[tokens[1]] + "\n";
        else
            return "1\n";
    } else if (tokens[0] == "DELETE" && tokens.size() == 2) {
        if (database_.erase(tokens[1])) return "0\n";
        else
            return "1\n";
    } else {
        return "2\n";
    }
}

void Server::process_commands(int fd) {
    std::string& buff = clients_[fd];
    size_t pos;
    while ((pos = buff.find('\n')) != std::string::npos) {
        std::string command = buff.substr(0, pos);
        buff.erase(0, pos + 1);
        std::string response = process_command(command);
        send(fd, response.c_str(), response.length(), 0);
    }
}

void Server::recv_data(int fd) {
    char buff[1024];
    while (1) {
        int bytes = recv(fd, buff, sizeof(buff) - 1, 0);
        if (bytes > 0) {
            buff[bytes] = '\0';
            clients_[fd] += buff;
            // std::cerr << "DEBUG: recv: fd " << fd << ": " << buff;
        } else if (bytes == 0) {
            handle_disconnection(fd);
            return;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            else
                exit_with_error("recv");
        }
    }
    process_commands(fd);
}

/////////////////////////////////////////////
// run
/////////////////////////////////////////////
void Server::run() {
    struct epoll_event events[64];
    while (g_running) {
        int ret = epoll_wait(epollFd_, events, 64, -1);
        if (ret > 0) {
            for (int i = 0; i < ret; ++i) {
                if (events[i].data.fd == serverFd_)
                    handle_new_connection();
                else
                    recv_data(events[i].data.fd);
            }
        } else if (ret < 0) {
            if (errno == EINTR)
                continue;
            else
                exit_with_error("epoll_wait");
        }
    }
    // reach here by SIGINT
    save_database();
}

/////////////////////////////////////////////
// setup
/////////////////////////////////////////////
void Server::setup() {
    // handle signal
    signal(SIGINT, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    // load database
    load_database();

    // create server socket
    serverFd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (serverFd_ < 0)
        exit_with_error("socket");

    // setsockopt
    int opt = 1;
    if (setsockopt(serverFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
        exit_with_error("setsockopt");

    // set_nonblocking
    set_nonblocking(serverFd_);

    // bind
    struct sockaddr_in addr;
    bzero(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port_);
    if (bind(serverFd_, (struct sockaddr*)&addr, sizeof(addr)) < 0)
        exit_with_error("bind");

    // listen
    if (listen(serverFd_, 128) < 0)
        exit_with_error("listen");

    // epoll_create
    epollFd_ = epoll_create1(0);
    if (epollFd_ < 0)
        exit_with_error("epoll_create1");

    // register epoll
    register_epoll(serverFd_);

    std::cout << "ready" << std::endl;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        write(2, "Usage: ./a.out <port> <path>\n", 29);
        return 1;
    }

    Server server(argv[1], argv[2]);
    server.setup();
    server.run();

    return 0;
}
