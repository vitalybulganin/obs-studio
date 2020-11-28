#include "check-connection.h"
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>

#include <util/base.h>
#include <util/bmem.h>

#if !defined(_WIN32)
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <fcntl.h>
#endif // !defined(_WIN32)

//!< Gets current time in nanoseconds.
static long current_time() {
        struct timespec time_start;
	// Getting current time.
	clock_gettime(CLOCK_REALTIME, &time_start);
	return 1000000000*time_start.tv_sec + time_start.tv_nsec;
}
//!< Connects to ingest server.
static bool check_connection(const char * host, long port) {
        bool status = false;
        int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock >= 0) {
                struct sockaddr_in addr;
                // clearing the struct.
                memset(&addr, 0, sizeof(addr));
                // Getting a host by its name.
                const struct hostent * server = gethostbyname(host);
		if (server) {
                        fcntl(sock, F_SETFL, O_NONBLOCK);
                        // now set the values properly
                        addr.sin_family = AF_INET;
                        addr.sin_addr = *((struct in_addr *)server->h_addr);
                        addr.sin_port = htons(port);
                        // Connecting to server.
                        int result = connect(sock, (struct sockaddr *)(&addr), sizeof(addr));
			if (result == -1 && errno == EINPROGRESS) {
                                fd_set rfds;
                                struct timeval tv;
				socklen_t len = sizeof(result);
                                // Wait for connection.
                                tv.tv_sec = 3;
                                tv.tv_usec = 0;

                                FD_ZERO(&rfds);
                                FD_SET(sock, &rfds);
				result = select(sock + 1,
						NULL,
                                                &rfds,
						NULL,
						&tv);
				if (result > 0 && getsockopt(sock, SOL_SOCKET, SO_ERROR, &result, &len) >= 0) {
                                        status = true;
                                }
			} else {
                                status = (result == 0);
			}
		}
#ifdef _DEBUG
                if (!status) {
                        blog(LOG_WARNING,
			     "check-connection.c: "
			     "[check_connection] connection failed: "
			     "%s to %s",
                             strerror(errno),
			     host);
                }
#endif // _DEBUG
                // closing the socket.
                shutdown(sock, SHUT_RDWR);
        }
        return status;
}

static char * get_host_name(const char * url) {
	if (url != NULL) {
		const unsigned protocol_length = strlen("rtmp://");
		for (unsigned i = protocol_length; i < strlen(url); ++i) {
			if (url[i] == ':' || url[i] == '/') {
				return bstrdup_n(url + protocol_length,
						 i - protocol_length);
			}
		}
	}
	return NULL;
}

long connection_time(const char * host, long port) {
	// Getting a name of domain.
	char * domain_name = get_host_name(host);
	if (domain_name) {
                // Getting start time of the operation.
                const long start_time = current_time();
                // Checking connection to host.
                const bool status = check_connection(domain_name, port);
		// Releasing allocated memory.
		bfree(domain_name);

                return (status) ? (current_time() - start_time) / 1000000 : INT32_MAX;
	}
	return INT32_MAX;
}
