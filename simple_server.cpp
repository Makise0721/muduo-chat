#include <iostream>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cout << "Usage: ./simple_server <ip> <port>" << std::endl;
        return -1;
    }

    const char* ip = argv[1];
    int port = std::stoi(argv[2]);

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1) {
        std::cerr << "socket creation failed: " << strerror(errno) << std::endl;
        return -1;
    }

    int opt = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
        std::cerr << "setsockopt failed: " << strerror(errno) << std::endl;
        close(sockfd);
        return -1;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    inet_pton(AF_INET, ip, &server_addr.sin_addr);

    if (bind(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
        std::cerr << "bind failed: " << strerror(errno) << std::endl;
        close(sockfd);
        return -1;
    }

    if (listen(sockfd, SOMAXCONN) == -1) {
        std::cerr << "listen failed: " << strerror(errno) << std::endl;
        close(sockfd);
        return -1;
    }

    std::cout << "Server listening on " << ip << ":" << port << std::endl;

    while (true) {
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);
        int connfd = accept(sockfd, (struct sockaddr*)&client_addr, &client_addr_len);
        if (connfd == -1) {
            std::cerr << "accept failed: " << strerror(errno) << std::endl;
            continue;
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
        std::cout << "New connection from " << client_ip << ":" << ntohs(client_addr.sin_port) << std::endl;

        close(connfd);
    }

    close(sockfd);
    return 0;
}
