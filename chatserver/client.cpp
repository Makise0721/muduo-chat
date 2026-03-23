#include <iostream>
#include <string>
#include <cstring>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "../thirdparty/json.hpp"

using json = nlohmann::json;

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cout << "Usage: ./client <ip> <port>" << std::endl;
        return -1;
    }

    const char* ip = argv[1];
    int port = std::stoi(argv[2]);

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1) {
        std::cerr << "socket creation failed" << std::endl;
        return -1;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    inet_pton(AF_INET, ip, &server_addr.sin_addr);

    if (connect(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
        std::cerr << "connect failed: " << strerror(errno) << std::endl;
        close(sockfd);
        return -1;
    }

    std::cout << "Connected to server" << std::endl;

    // 测试注册功能
    json reg_msg;
    reg_msg["msgid"] = 4; // REG_MSG
    reg_msg["name"] = "testuser";
    reg_msg["password"] = "123456";
    std::string reg_str = reg_msg.dump();
    send(sockfd, reg_str.c_str(), reg_str.size(), 0);
    std::cout << "Sent registration message" << std::endl;

    // 接收注册响应
    char buffer[1024] = {0};
    int len = recv(sockfd, buffer, sizeof(buffer), 0);
    if (len > 0) {
        std::string response(buffer, len);
        json js = json::parse(response);
        std::cout << "Registration response: " << response << std::endl;
    }

    // 测试登录功能
    json login_msg;
    login_msg["msgid"] = 1; // LOGIN_MSG
    login_msg["id"] = 1;
    login_msg["password"] = "123456";
    std::string login_str = login_msg.dump();
    send(sockfd, login_str.c_str(), login_str.size(), 0);
    std::cout << "Sent login message" << std::endl;

    // 接收登录响应
    memset(buffer, 0, sizeof(buffer));
    len = recv(sockfd, buffer, sizeof(buffer), 0);
    if (len > 0) {
        std::string response(buffer, len);
        json js = json::parse(response);
        std::cout << "Login response: " << response << std::endl;
    }

    // 测试单聊功能
    json chat_msg;
    chat_msg["msgid"] = 6; // ONE_CHAT_MSG
    chat_msg["id"] = 1;
    chat_msg["toid"] = 2;
    chat_msg["msg"] = "Hello from client!";
    std::string chat_str = chat_msg.dump();
    send(sockfd, chat_str.c_str(), chat_str.size(), 0);
    std::cout << "Sent chat message" << std::endl;

    // 接收聊天响应（如果有）
    memset(buffer, 0, sizeof(buffer));
    len = recv(sockfd, buffer, sizeof(buffer), 0);
    if (len > 0) {
        std::string response(buffer, len);
        json js = json::parse(response);
        std::cout << "Chat response: " << response << std::endl;
    }

    close(sockfd);
    return 0;
}
