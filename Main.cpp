#include <asm-generic/socket.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <syslog.h>
#include <unistd.h>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <type_traits>

#include "libs/serialport/SerialPort.h"

#define TEST_MODE
#define DAEMON

// Const
#define PORT_SPEED			9600
#define QUEUE_MSIZE			1024
#define BUFFER_MSIZE		32
#define MAX_EVENTS			128
#define CONNECTIONS_MSIZE	5
#define TIMEOUT				200000
#define PROPERTY_MSIZE		128

#define CONFIG_FILE			"/etc/USB2COMBridge/config"

// Config struct
struct Config {
	uint8_t qFromSerial[QUEUE_MSIZE];
	uint16_t qFromSerialLen;
	uint8_t qFromEthernet[QUEUE_MSIZE];
	uint16_t qFromEthernetLen;
	SerialPort* port;
	int socket;
	std::mutex qFromSerialMutex;
	std::mutex qFromEthernetMutex;
	pthread_t thPortWriter;
	pthread_t thPortReader;
	pthread_t thSocketWriter;
	bool thPortWriterRunning;
	bool thPortReaderRunning;
	bool thSocketWriterRunning;
};
typedef struct Config ConfigT;

// Logging
void openLogger() {
#ifdef DAEMON
	openlog("USB2COMBridge", 0, LOG_USER);
#endif
}

void printLog(int level, const char *message) {
#ifdef DAEMON
	syslog(level, message);
#else
	puts(message);
#endif
}

void closeLogger() {
#ifdef DAEMON
	closelog();
#endif
}

void logData(uint8_t *buffer, uint16_t bufferLen) {
	std::stringstream ss;
	for (int i = 0; i < bufferLen; i++) {
		ss << buffer[i];
	}

	ss << std::endl;
	printLog(LOG_INFO, ss.str().c_str());
}

void* portReader(void* args) {
	ConfigT* config = (ConfigT*) args;

	config->thPortReaderRunning = true;

	uint8_t buffer[QUEUE_MSIZE];
	int bufferLen = -1;

	struct epoll_event event, events[MAX_EVENTS];
	int efd = epoll_create1(0);
	if (efd == -1) {
		config->thPortReaderRunning = false;
		printLog(LOG_ERR, "E: epoll_create1() failed");

		return NULL;
	}
	printLog(LOG_INFO, "D: epoll_create1() success");

	event.data.fd = config->port->getRawHandler();
	event.events = EPOLLIN | EPOLLET | EPOLLHUP | EPOLLERR | EPOLLRDHUP;

	if (epoll_ctl(efd, EPOLL_CTL_ADD, config->port->getRawHandler(), &event) == -1) {
		config->thPortReaderRunning = false;
		printLog(LOG_ERR, "E: epoll_ctl() failed");

		return NULL;
	}
	printLog(LOG_INFO, "D: epoll_ctl() success");

	while (true) {
		int res = epoll_wait(efd, events, MAX_EVENTS, -1);
		if (res == -1) {
			config->thPortReaderRunning = false;
			printLog(LOG_ERR, "E: epoll_wait() failed");

			return NULL;
		}

		for (int i = 0; i < res; i++) {
			// Some error occur in the port
			if (events[i].events & (EPOLLHUP | EPOLLERR | EPOLLRDHUP)) {
				printLog(LOG_ERR, "E: port connection error");

				if (config->port->close() == -1) {
					printLog(LOG_ERR, "E: port->close() failed");

					return NULL;
				}
				printLog(LOG_INFO, "D: port->close() success");

				while (true) {
					printLog(LOG_INFO, "D: try to connect...");
					if (config->port->open() != -1) {
						printLog(LOG_ERR, "E: port->open() failed");

						break;
					}

					usleep(TIMEOUT * 10);
				}

				if (epoll_ctl(efd, EPOLL_CTL_DEL, events[i].data.fd, NULL) == -1) {
					printLog(LOG_ERR, "E: epoll_ctl(EPOLL_CTL_DEL) failed");

					return NULL;
				}
				printLog(LOG_INFO, "D: epoll_ctl(EPOLL_CTL_DEL) success");

				if (epoll_ctl(efd, EPOLL_CTL_ADD, config->port->getRawHandler(), &event) == -1) {
					printLog(LOG_ERR, "E: epoll_ctl(EPOLL_CTL_ADD) failed");

					return NULL;
				}
				printLog(LOG_INFO, "D: epoll_ctl(EPOLL_CTL_ADD) success");
			// Data available in the port
			} else if (events[i].events & (EPOLLIN | EPOLLET)) {
				bufferLen = read(events[i].data.fd, (void *)buffer, QUEUE_MSIZE * sizeof(uint8_t));
				if (bufferLen > 0) {
					std::cout << "+PR----------" << std::endl;
					logData(buffer, bufferLen);
					std::cout << "-PR----------" << std::endl;

					config->qFromSerialMutex.lock();

					if (config->qFromSerialLen + bufferLen < QUEUE_MSIZE) {
						memmove((void *)&(config->qFromSerial[config->qFromSerialLen]), (const void *)buffer, bufferLen * sizeof(uint8_t));
						config->qFromSerialLen += bufferLen;
					}

					config->qFromSerialMutex.unlock();
				}
			}
		}
	}

	config->thPortReaderRunning = false;

	return NULL;
}

void* portWriter(void* args) {
	ConfigT* config = (ConfigT*) args;

	config->thPortWriterRunning = true;

	uint8_t buffer[BUFFER_MSIZE];
	int bufferLen = -1;

	std::chrono::high_resolution_clock::time_point start, end;

	int timeout = TIMEOUT;

	while (true) {
		config->qFromEthernetMutex.lock();

		start = std::chrono::high_resolution_clock::now();

		while (config->qFromEthernetLen != 0) {
			bufferLen = (config->qFromEthernetLen > BUFFER_MSIZE ? BUFFER_MSIZE : config->qFromEthernetLen);
			memmove(buffer, config->qFromEthernet, bufferLen * sizeof(uint8_t));

			std::cout << "+PW----------" << std::endl;
			logData(buffer, bufferLen);
			std::cout << "-PW----------" << std::endl;

			if (config->port->write(buffer, bufferLen) == -1) {
				printLog(LOG_ERR, "E: port->write() failed");

				break;
			}

			memmove(config->qFromEthernet, &config->qFromEthernet[bufferLen], (config->qFromEthernetLen - bufferLen) * sizeof(uint8_t));
			config->qFromEthernetLen -= bufferLen;
		}

		end = std::chrono::high_resolution_clock::now();

		config->qFromEthernetMutex.unlock();

		std::chrono::duration<unsigned long> elapsedTime = std::chrono::duration_cast<std::chrono::duration<unsigned long>>(end - start);

		timeout = timeout - elapsedTime.count();

		usleep(timeout > 0 ? timeout : 0);
	}

	config->thPortReaderRunning = false;

	return NULL;
}

int readFromSocket(ConfigT* config) {
	printLog(LOG_INFO, "D: readFromSocket()");
	uint8_t buffer[QUEUE_MSIZE];
	uint16_t bufferLen = 0;

	if ((bufferLen = read(config->socket, buffer, QUEUE_MSIZE * sizeof(uint8_t))) == -1) {
		printLog(LOG_ERR, "E: read() failed");

		return -1;
	}

	config->qFromEthernetMutex.unlock();

	std::cout << "+SR----------" << std::endl;
	logData(buffer, bufferLen);
	std::cout << "-SR----------" << std::endl;

	config->qFromEthernetMutex.lock();

	if (config->qFromEthernetLen + bufferLen < QUEUE_MSIZE) {
		memmove(&(config->qFromEthernet[config->qFromEthernetLen]), buffer, bufferLen * sizeof(uint8_t));
		config->qFromEthernetLen += bufferLen;
	}

	config->qFromEthernetMutex.unlock();

#ifdef TEST_MODE
	config->qFromSerialMutex.lock();

//	std::cout << buffer[0] << buffer[1] << buffer[2] << std::endl;

	if ((buffer[0] == 'A') && (buffer[1] == 'V') && (buffer[2] == 'R')) {
		config->qFromSerial[config->qFromSerialLen] = 'V';
		config->qFromSerial[config->qFromSerialLen + 1] = 'D';
		config->qFromSerial[config->qFromSerialLen + 2] = '1';
		config->qFromSerial[config->qFromSerialLen + 3] = '7';
		config->qFromSerialLen += 4;
	} else if ((buffer[0] == 'T') && (buffer[1] == 'U') && (buffer[2] == 'R')) {
		config->qFromSerial[config->qFromSerialLen] = 'N';
		config->qFromSerial[config->qFromSerialLen + 1] = 'I';
		config->qFromSerial[config->qFromSerialLen + 2] = 'K';
		config->qFromSerial[config->qFromSerialLen + 3] = 'E';
		config->qFromSerial[config->qFromSerialLen + 4] = 'T';
		config->qFromSerialLen += 5;
	} else if ((buffer[0] == 'A') && (buffer[1] == '1') && (buffer[2] == '7')) {
		config->qFromSerial[config->qFromSerialLen] = 'A';
		config->qFromSerial[config->qFromSerialLen + 1] = 'D';
		config->qFromSerial[config->qFromSerialLen + 2] = 'S';
		config->qFromSerial[config->qFromSerialLen + 3] = '1';
		config->qFromSerialLen += 4;
	} else if ((buffer[0] == 'A') && (buffer[1] == '2') && (buffer[2] == '7')) {
		config->qFromSerial[config->qFromSerialLen] = 'A';
		config->qFromSerial[config->qFromSerialLen + 1] = 'D';
		config->qFromSerial[config->qFromSerialLen + 2] = 'S';
		config->qFromSerial[config->qFromSerialLen + 3] = '2';
		config->qFromSerialLen += 4;
	} else if ((buffer[0] == 'A') && (buffer[1] == '1') && (buffer[2] == '0')) {
		config->qFromSerial[config->qFromSerialLen] = 'A';
		config->qFromSerial[config->qFromSerialLen + 1] = 'X';
		config->qFromSerial[config->qFromSerialLen + 2] = 'P';
		config->qFromSerial[config->qFromSerialLen + 3] = '1';
		config->qFromSerialLen += 4;
	} else if ((buffer[0] == 'A') && (buffer[1] == '2') && (buffer[2] == '0')) {
		config->qFromSerial[config->qFromSerialLen] = 'A';
		config->qFromSerial[config->qFromSerialLen + 1] = 'X';
		config->qFromSerial[config->qFromSerialLen + 2] = 'P';
		config->qFromSerial[config->qFromSerialLen + 3] = '2';
		config->qFromSerialLen += 4;
	} else if ((buffer[0] == 'A') && (buffer[1] == '1') && (buffer[2] == '9')) {
		config->qFromSerial[config->qFromSerialLen] = 'A';
		config->qFromSerial[config->qFromSerialLen + 1] = 'L';
		config->qFromSerial[config->qFromSerialLen + 2] = 'L';
		config->qFromSerial[config->qFromSerialLen + 3] = '1';
		config->qFromSerialLen += 4;
	} else if ((buffer[0] == 'A') && (buffer[1] == '2') && (buffer[2] == '9')) {
		config->qFromSerial[config->qFromSerialLen] = 'A';
		config->qFromSerial[config->qFromSerialLen + 1] = 'L';
		config->qFromSerial[config->qFromSerialLen + 2] = 'L';
		config->qFromSerial[config->qFromSerialLen + 3] = '2';
		config->qFromSerialLen += 4;
	} else if ((buffer[0] == 'A') && (buffer[1] == '1') && (buffer[2] == '5')) {
		config->qFromSerial[config->qFromSerialLen] = 'P';
		config->qFromSerial[config->qFromSerialLen + 1] = 'U';
		config->qFromSerial[config->qFromSerialLen + 2] = 'L';
		config->qFromSerial[config->qFromSerialLen + 3] = '1';
		config->qFromSerialLen += 4;
	} else if ((buffer[0] == 'A') && (buffer[1] == '2') && (buffer[2] == '5')) {
		config->qFromSerial[config->qFromSerialLen] = 'P';
		config->qFromSerial[config->qFromSerialLen + 1] = 'U';
		config->qFromSerial[config->qFromSerialLen + 2] = 'L';
		config->qFromSerial[config->qFromSerialLen + 3] = '2';
		config->qFromSerialLen += 4;
	}

	config->qFromSerialMutex.unlock();

#endif

	return bufferLen;
}

void* socketWriter(void* args) {
	ConfigT* config = (ConfigT*) args;

//	std::cout << "D: socketWriter()" << std::endl;

	config->thSocketWriterRunning = true;

	uint8_t buffer[BUFFER_MSIZE];
	int bufferLen = -1;

	std::chrono::high_resolution_clock::time_point start, end;

	int timeout = TIMEOUT;

	while (true) {
		if (config->socket == -1) {
			continue;
		}

		config->qFromSerialMutex.lock();

		start = std::chrono::high_resolution_clock::now();

//		std::cout << "D: serialQueueLen" << config->qFromSerialLen << std::endl;
		while (config->qFromSerialLen != 0) {
			bufferLen = (config->qFromSerialLen > BUFFER_MSIZE ? BUFFER_MSIZE : config->qFromSerialLen);
			memmove(buffer, config->qFromSerial, bufferLen * sizeof(uint8_t));

			std::cout << "+SW----------" << std::endl;
			logData(buffer, bufferLen);
			std::cout << "-SW----------" << std::endl;

			if (write(config->socket, buffer, bufferLen) == -1) {
				printLog(LOG_ERR, "E: socket->write() failed");
				exit(0);

				break;
			}

			memmove(config->qFromSerial, &config->qFromSerial[bufferLen], (config->qFromSerialLen - bufferLen) * sizeof(uint8_t));
			config->qFromSerialLen -= bufferLen;
		}

		end = std::chrono::high_resolution_clock::now();

		config->qFromSerialMutex.unlock();

		std::chrono::duration<unsigned long> elapsedTime = std::chrono::duration_cast<std::chrono::duration<unsigned long>>(end - start);

		timeout = timeout - elapsedTime.count();

		usleep(timeout > 0 ? timeout : 0);
	}

	config->thSocketWriterRunning = false;

	return NULL;
}

int startPortThreads(ConfigT* config) {
	if (pthread_create(&config->thPortReader, NULL, portReader, config) < 0) {
		printLog(LOG_ERR, "E: pthread_create(thPortReader) failed");

		return -1;
	}
	printLog(LOG_INFO, "D: pthread_create(thPortReader) success");

	if (pthread_create(&config->thPortWriter, NULL, portWriter, config) < 0) {
		pthread_join(config->thPortReader, NULL);
		printLog(LOG_ERR, "E: pthread_create(thPortWriter) failed");

		return -1;
	}
	printLog(LOG_INFO, "D: pthread_create(thPortWriter) success");

	return 0;
}

int stopPortThreads(ConfigT* config) {
	if (pthread_join(config->thPortReader, NULL) < 0) {
		printLog(LOG_ERR, "E: pthread_join(thPortReader) failed");

		return -1;
	}
	printLog(LOG_INFO, "D: pthread_join(thPortReader) success");

	if (pthread_join(config->thPortWriter, NULL) < 0) {
		printLog(LOG_ERR, "E: pthread_join(thPortWriter) failed");

		return -1;
	}
	printLog(LOG_INFO, "D: pthread_join(thPortWriter) success");

	return 0;
}

int startSocketThreads(ConfigT* config) {
	if (pthread_create(&config->thSocketWriter, NULL, socketWriter, config) < 0) {
		printLog(LOG_ERR, "E: pthread_create(thSocketWriter) failed");

		return -1;
	}
	printLog(LOG_INFO, "D: pthread_create(thSocketWriter) success");

	return 0;
}

int stopSocketThreads(ConfigT* config) {
	if (pthread_join(config->thSocketWriter, NULL) < 0) {
		printLog(LOG_ERR, "E: pthread_join(thSocketWriter) failed");

		return -1;
	}
	printLog(LOG_INFO, "D: pthread_join(thSocketWriter) success");

	return 0;
}

int setNonBlocking(int fd) {
	int res = fcntl(fd, F_GETFL, 0);
	if (res == -1) {
		return -1;
	}

	if (fcntl(fd, F_SETFL, res | O_NONBLOCK) == -1) {
		return -1;
	}

	return 0;
}

void signalHandler(int signal) {
	switch (signal) {
	case SIGHUP:
		break;

	case SIGTERM:
		closeLogger();
		exit(0);

		break;
	}
}

int main(int argc, char** argv) {
	// Make daemon
#ifdef DAEMON
	int pid = fork();
#endif

	openLogger();

#ifdef DAEMON
	if (pid == -1) {
		printLog(LOG_ERR, "E: fork() failed");

		return -1;
	} else if (pid != 0) {
		return 0;
	} else {
		umask(0);
		setsid();
		chdir("/");
		close(STDIN_FILENO);
		close(STDOUT_FILENO);
		close(STDERR_FILENO);
	}

	// Signals
	signal(SIGHUP, signalHandler);
	signal(SIGTERM, signalHandler);
#endif

	// Setup config
	ConfigT* config = new ConfigT();
	memset(config, 0, sizeof(ConfigT));

	std::map<std::string, std::string> properties;
	std::string line;

	std::string PORT_NAME;
	int HTTP_PORT;

	std::ifstream propertyFile(CONFIG_FILE);
	if (propertyFile.is_open()) {
		int i = 0;
		while (getline(propertyFile, line)) {
			switch(i) {
			case 0:
				PORT_NAME = line;

				break;

			case 1:
				HTTP_PORT = std::stoi(line);

				break;
			}

//			std::cout << line << std::endl;

			i++;
		}
		propertyFile.close();
	}

#ifndef TEST_MODE
	// Init port IO
	config->port = new SerialPort(PORT_NAME.c_str(), PORT_SPEED);
	if (config->port->open() == -1) {
		char str[128];
		sprintf(str, "E: SerialPort.open(%s, %i) failed\r\n", PORT_NAME.c_str(), PORT_SPEED);
		printLog(LOG_ERR, str);
		closeLogger();

		return -1;
	}
	printLog(LOG_INFO, "D: SerialPort open() success");

	if (startPortThreads(config) == -1) {
		config->port->close();
		printLog(LOG_ERR, "E: startPortThreads() failed");
		closeLogger();

		return -1;
	}
	printLog(LOG_INFO, "D: startPortThreads() success");
#endif

	// Init server socket
	struct sockaddr_in addr;
	int addrLen = 0;
	addr.sin_family = AF_INET;
	addr.sin_port = htons(HTTP_PORT);
	addr.sin_addr.s_addr = INADDR_ANY;

	int serverSocket = socket(PF_INET, SOCK_STREAM, 0);
	if (serverSocket == -1) {
#ifndef TEST_MODE
		stopPortThreads(config);
		config->port->close();
#endif
		printLog(LOG_ERR, "E: socket() failed");
		closeLogger();

		return -1;
	}
	printLog(LOG_INFO, "D: socket() success");

	int f = 1;
	if(setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR, (const char*)&f, sizeof(f)) == -1) {
#ifndef TEST_MODE
		stopPortThreads(config);
		config->port->close();
#endif
		close(serverSocket);
		printLog(LOG_ERR, "E: setsockopt() failed");
		closeLogger();

		return -1;
	}
	printLog(LOG_INFO, "D: setsockopt() success");

	if (setNonBlocking(serverSocket) == -1) {
#ifndef TEST_MODE
		stopPortThreads(config);
		config->port->close();
#endif
		close(serverSocket);
		printLog(LOG_ERR, "E: setNonBlocking() failed");
		closeLogger();

		return -1;
	}
	printLog(LOG_INFO, "D: setNonBlocking() success");

	if (bind(serverSocket, (struct sockaddr*) &addr, sizeof(addr)) != 0) {
#ifndef TEST_MODE
		stopPortThreads(config);
		config->port->close();
#endif
		close(serverSocket);
		printLog(LOG_ERR, "E: bind() failed");
		closeLogger();

		return -1;
	}
	printLog(LOG_INFO, "D: bind() success");

	if (listen(serverSocket, CONNECTIONS_MSIZE) == -1) {
#ifndef TEST_MODE
		stopPortThreads(config);
		config->port->close();
#endif
		close(serverSocket);
		printLog(LOG_ERR, "E: listen() failed");
		closeLogger();

		return -1;
	}
	printLog(LOG_INFO, "D: listen() success");

	// Epoll
	struct epoll_event ev, events[MAX_EVENTS];

	int epollFd = -1;
	if ((epollFd = epoll_create1(0)) == -1) {
#ifndef TEST_MODE
		stopPortThreads(config);
		config->port->close();
#endif
		close(serverSocket);
		printLog(LOG_ERR, "E: epoll_create1() failed");
		closeLogger();

		return -1;
	}

	ev.events = EPOLLIN | EPOLLET | EPOLLHUP | EPOLLERR | EPOLLRDHUP;
	ev.data.fd = serverSocket;
	if (epoll_ctl(epollFd, EPOLL_CTL_ADD, serverSocket, &ev) == -1) {
#ifndef TEST_MODE
		stopPortThreads(config);
		config->port->close();
#endif
		close(serverSocket);
		printLog(LOG_ERR, "E: epoll_ctl() failed");
		closeLogger();

		return -1;
	}

	while (true) {
		int res = epoll_wait(epollFd, events, MAX_EVENTS, TIMEOUT / 2000);
		if (res == -1) {
#ifndef TEST_MODE
			stopPortThreads(config);
			config->port->close();
#endif
			close(serverSocket);
			printLog(LOG_ERR, "E: epoll_wait() failed");
			closeLogger();

			return -1;
		}

		for (int n = 0; n < res; n++) {
			if (events[n].data.fd == serverSocket) {
				printLog(LOG_INFO, "D: on SERVER SOCKET");

				if (events[n].events & (EPOLLIN | EPOLLET)) {
					if ((config->socket = accept(serverSocket, (struct sockaddr*) &addr, (unsigned int*)&addrLen)) == -1) {
						printLog(LOG_ERR, "E: accept() failed");

						continue;
					}
					printLog(LOG_INFO, "D: accept() success");

					if (setNonBlocking(config->socket) == -1) {
						close(config->socket);
						config->socket = -1;
						printLog(LOG_ERR, "E: setNonBlocking() failed");

						continue;
					}
					printLog(LOG_INFO, "D: setNonBlocking() success");

					ev.events = EPOLLIN | EPOLLET | EPOLLHUP | EPOLLERR | EPOLLRDHUP;
					ev.data.fd = config->socket;

					if (epoll_ctl(epollFd, EPOLL_CTL_ADD, config->socket, &ev) == -1) {
						close(config->socket);
						config->socket = -1;
						printLog(LOG_ERR, "E: epoll_ctl() failed");

						continue;
					}

					if (startSocketThreads(config) == -1) {
						close(config->socket);
						config->socket = -1;
						printLog(LOG_ERR, "E: startSocketThreads() failed");

						continue;
					}
				}
			} else {
				printLog(LOG_INFO, "D: on CLIENT SOCKET");

				if (events[n].events & (EPOLLERR | EPOLLRDHUP | EPOLLHUP)) {
					if (epoll_ctl(epollFd, EPOLL_CTL_DEL, events[n].data.fd, NULL) == -1) {
						printLog(LOG_ERR, "E: epoll_ctl() failed");

						continue;
					}

					close(events[n].data.fd);
					config->socket = -1;

					printLog(LOG_INFO, "D: close client() socket");
				} else if (events[n].events & (EPOLLIN | EPOLLET)) {
					readFromSocket(config);
				}
			}
		}
	}

	// Stop port IO
#ifndef TEST_MODE
	if (stopPortThreads(config) == -1) {
		config->port->close();
		close(serverSocket);
		printLog(LOG_ERR, "E: stopPortThreads() failed");
		closeLogger();

		return -1;
	}
	printLog(LOG_INFO, "D: stopPortThreads() success");
#endif

	if (close(serverSocket) == -1) {
#ifndef TEST_MODE
		config->port->close();
#endif
		close(serverSocket);
		printLog(LOG_ERR, "E: close(serverSocket) failed");
		closeLogger();

		return -1;
	}
	printLog(LOG_INFO, "D: close(serverSocket) success");

#ifndef TEST_MODE
	if (config->port->close() == -1) {
		close(serverSocket);
		printLog(LOG_ERR, "E: port->close() failed");
		closeLogger();

		return -1;
	}
	printLog(LOG_INFO, "D: port->close() success");
#endif

	if (close(serverSocket) == -1) {
		printLog(LOG_ERR, "E: close() failed");
		closeLogger();

		return -1;
	}

	closeLogger();

	return 0;
}
