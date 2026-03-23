#include "InetAddress.h"

#include <iostream>
#include <strings.h>

InetAddress::InetAddress(uint16_t port, std::string ip)
{
    bzero(&addr_, sizeof addr_);
    addr_.sin_family = AF_INET;
    addr_.sin_port = htons(port);
    addr_.sin_addr.s_addr = inet_addr(ip.c_str());
}

std::string InetAddress::toIp() const
{
    return std::string(inet_ntoa(addr_.sin_addr));
}

std::string InetAddress::toIpPort() const
{
    char buf[32];
    snprintf(buf, sizeof buf, "%s:%u", inet_ntoa(addr_.sin_addr), ntohs(addr_.sin_port));
    return std::string(buf);
}

uint16_t InetAddress::toPort() const
{
    return ntohs(addr_.sin_port);
}

// int main()
// {
//     InetAddress addr(8080);
//     std::cout << "IP: " << addr.toIp() << std::endl;
//     std::cout << "IP:Port: " << addr.toIpPort() << std::endl;
//     std::cout << "Port: " << addr.toPort() << std::endl;
//     return 0;
// }