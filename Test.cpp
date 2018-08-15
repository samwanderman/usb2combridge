#ifdef __unix__
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstdint>
#include <cstring>
#include <iostream>

#define BUF_MSIZE	1024
#define IP			"172.16.3.158"
#define PORT		12001

int main() {
	int sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock == -1) {
		std::cerr << "E: socket() error" << std::endl;
		return -1;
	}

	struct sockaddr_in sa;
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = PF_INET;
	sa.sin_port = htons(PORT);
	sa.sin_addr.s_addr = inet_addr(IP);

	if (connect(sock, (struct sockaddr*)&sa, sizeof(sa)) == -1) {
		std::cerr << "E: connect() error" << std::endl;
		return -1;
	}

	uint8_t buffer[BUF_MSIZE];
	uint16_t bufferLen;

	buffer[0] = 'A';
	buffer[1] = 'V';
	buffer[2] = 'R';
	bufferLen = 3;

	if (write(sock, buffer, bufferLen) == -1) {
		close(sock);
		std::cerr << "E: write() error" << std::endl;
		return -1;
	}
	std::cout << "D: write() success" << std::endl;


	if ((bufferLen = read(sock, buffer, BUF_MSIZE)) == -1) {
		close(sock);
		std::cerr << "E: read() error" << std::endl;
		return -1;
	}
	std::cout << "D: read() success" << std::endl;

	std::cout << "D: Response:" << std::endl;
	for (int i = 0; i < bufferLen; i++) {
		std::cout << std::hex << buffer[i];
	}
	std::cout << std::endl << bufferLen << std::endl;

	close(sock);

	return 0;
}
#else

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iostream>
#include <unistd.h>

#define DEFAULT_HOST	"172.16.3.158"
#define DEFAULT_PORT	"12001"
#define BUF_LEN			512

#pragma comment(lib, "Ws2_32.lib");

int main(int ac, char** av) {
	WSADATA wsaData;
	SOCKET sock = INVALID_SOCKET;
	struct addrinfo config, *result, *ptr;

	int res = WSAStartup(MAKEWORD(2, 2), &wsaData);
	if (res == -1) {
		std::cerr << "E: WSAStartup() error" << std::endl;

		return -1;
	}
	std::cout << "D: WSAStartup() success" << std::endl;

	ZeroMemory(&config, sizeof(config));
	config.ai_family = AF_UNSPEC;
	config.ai_socktype = SOCK_STREAM;
	config.ai_protocol = IPPROTO_TCP;

	res = getaddrinfo(DEFAULT_HOST, DEFAULT_PORT, &config, &result);
	if (res == -1) {
		std::cerr << "E: getaddrinfo() error" << std::endl;
		WSACleanup();

		return -1;
	}
	std::cout << "D: getaddrinfo() success" << std::endl;

	ptr = result;
	sock = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
	if (sock == INVALID_SOCKET) {
		std::cerr << "E: socket() error" << std::endl;
		WSACleanup();

		return -1;
	}
	std::cout << "E: socket() success" << std::endl;

	res = connect(sock, ptr->ai_addr, (int)ptr->ai_addrlen);
	if (res == -1) {
		closesocket(sock);
		sock = INVALID_SOCKET;
		std::cerr << "E: connect() error" << std::endl;

		return -1;
	}
	std::cout << "D: connect() success" << std::endl;

	freeaddrinfo(result);

	if (sock == INVALID_SOCKET) {
		WSACleanup();
		std::cerr << "E: socket error" << std::endl;

		return -1;
	}

	char buffer[32];
	buffer[0] = 'A';
	buffer[1] = 'V';
	buffer[2] = 'R';

	res = send(sock, buffer, 3 * sizeof(char), 0);
	std::cout << "D: bytes written: " << res << std::endl;
	if (res == -1) {
		closesocket(sock);
		WSACleanup();
		std::cerr << "E: write() error" << std::endl;

		return -1;
	}
	std::cout << "D: write() success" << std::endl;

	do {
		res = recv(sock, buffer, 32, 0);
		if (res > 0) {
			std::cout << "D: bytes received: " << res << std::endl;
			for (int i = 0; i < res; i++) {
				std::cout << std::hex << buffer[i];
			}
			std::cout << std::endl;
			break;
		} else if (res == 0) {
			std::cout << "D: connection closed" << std::endl;
		} else {
			std::cerr << "E: recv() error" << std::endl;
		}
	} while(res > 0);

	res = shutdown(sock, SD_SEND);
	if (res == SOCKET_ERROR) {
		closesocket(sock);
		WSACleanup();
		std::cerr << "E: shutdown() error" << std::endl;

		return -1;
	}
	std::cout << "D: shutdown() success" << std::endl;

	closesocket(sock);
	WSACleanup();

	return 0;
}

#endif
