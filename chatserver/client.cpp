#include <iostream>
#include <string>
#include <cstring>
#include <errno.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "../thirdparty/json.hpp"

using json = nlohmann::json;

static bool recvLine(int sockfd, std::string& out) {
    out.clear();
    while (true) {
        char c = 0;
        ssize_t n = recv(sockfd, &c, 1, 0);
        if (n == 1) {
            if (c == '\n') {
                return !out.empty();
            }
            if (c != '\r') {
                out.push_back(c);
            }
        } else if (n == 0) {
            return false;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                return false;
            }
            return false;
        }
    }
}

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

    // 防止 recv 卡死
    struct timeval tv;
    tv.tv_sec = 3;
    tv.tv_usec = 0;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    std::cout << "Connected to server" << std::endl;

    // 测试注册功能
    json reg_msg;
    reg_msg["msgid"] = 4; // REG_MSG
    reg_msg["name"] = "testuser";
    reg_msg["password"] = "123456";
    std::string reg_str = reg_msg.dump();
    reg_str.push_back('\n');
    send(sockfd, reg_str.c_str(), reg_str.size(), 0);
    std::cout << "Sent registration message" << std::endl;

    // 接收注册响应
    std::string line;
    if (recvLine(sockfd, line)) {
        json js = json::parse(line);
        std::cout << "Registration response: " << line << std::endl;
    }

    // 测试登录功能
    json login_msg;
    login_msg["msgid"] = 1; // LOGIN_MSG
    login_msg["id"] = 1;
    login_msg["password"] = "123456";
    std::string login_str = login_msg.dump();
    login_str.push_back('\n');
    send(sockfd, login_str.c_str(), login_str.size(), 0);
    std::cout << "Sent login message" << std::endl;

    // 接收登录响应
    if (recvLine(sockfd, line)) {
        json js = json::parse(line);
        std::cout << "Login response: " << line << std::endl;
    }

    // 如果有离线消息，会在登录应答之后陆续推送；这里先快速把“多余行”读掉避免串到后续 recv。
    {
        struct timeval shortTv;
        shortTv.tv_sec = 0;
        shortTv.tv_usec = 300000; // 0.3s
        setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &shortTv, sizeof(shortTv));

        while (recvLine(sockfd, line)) {
            std::cout << "Offline message: " << line << std::endl;
        }

        // 恢复默认超时
        tv.tv_sec = 3;
        tv.tv_usec = 0;
        setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    // 测试单聊功能
    json chat_msg;
    chat_msg["msgid"] = 6; // ONE_CHAT_MSG
    chat_msg["id"] = 1;
    chat_msg["toid"] = 2;
    chat_msg["msg"] = "Hello from client!";
    std::string chat_str = chat_msg.dump();
    chat_str.push_back('\n');
    send(sockfd, chat_str.c_str(), chat_str.size(), 0);
    std::cout << "Sent chat message" << std::endl;

    // 接收聊天响应（如果有）
    if (recvLine(sockfd, line)) {
        json js = json::parse(line);
        std::cout << "Chat response: " << line << std::endl;
    }

    close(sockfd);
    return 0;
}
