/**
 * Handle multiple client socket connections on multiple
 * host ports.
 *
 * VanguardiaSur (c) 2015 - ezequiel@vanguardiasur.com.ar
 *
 * Based on code by Silver Moon (m00n.silv3r@gmail.com)
 * http:// www.binarytides.com/multiple-socket-connections-fdset-select-linux/
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/time.h>

#include <stdbool.h>
#include <sys/epoll.h>

#include "threadpool.h"
#include <assert.h>


#define PORT_START	8888
#define PORT_COUNT	5
#define MAX_CLIENTS	50
#define MAX_BACKLOG	10

#define THREAD 10
#define QUEUE  256
#define MAXEVENTS 64


void taskClientHandle(void *fd);

/* shared data between threads */
int tasks = 0, done = 0;
pthread_mutex_t lock;

int clients;

int epollfd; // Epoll File descriptor new conection
//int epollfd_cli; // Epoll File descriptor new client
int sockets[MAX_CLIENTS];



int main()
{
	int server_socks[PORT_COUNT];

	struct sockaddr_in address;
	int i, j;


	struct epoll_event ev, events[MAXEVENTS];

	threadpool_error_t rc;
	threadpool_t *pool;

	/* initialize pthread mutex  */
	pthread_mutex_init(&lock, NULL);

    assert((pool = threadpool_create(THREAD, QUEUE, 0)) != NULL);
    printf("Pool started with %d threads and "
            "queue size of %d\n", THREAD, QUEUE);

    /* Creates an epoll instance */
	epollfd = epoll_create1(0);
	if (epollfd == -1) {
		perror("epoll_create1");
		exit(EXIT_FAILURE);
	}

	// initialise all sockets[] to 0 so not checked
	for (i = 0; i < MAX_CLIENTS; i++)
		sockets[i] = -1;

	// create a master socket
	for (i = 0; i < PORT_COUNT; i++) {
		if ((server_socks[i] = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
			perror("socket failed");
			exit(EXIT_FAILURE);
		}

		// set master socket to allow multiple connections,
		// this is just a good habit, it will work without this
		//if (setsockopt(server_socks[i], SOL_SOCKET, SO_REUSEADDR, (char *)&opt, sizeof(opt)) < 0 ) {
		//	perror("setsockopt");
		//	exit(EXIT_FAILURE);
		//}

		// type of socket created
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = INADDR_ANY;
		address.sin_port = htons(PORT_START + i);

		// bind the socket to localhost port 8888 + i
		if (bind(server_socks[i], (struct sockaddr *)&address, sizeof(address))<0) {
			perror("bind failed");
			exit(EXIT_FAILURE);
		}

		// specify maximum of pending connections
		// for the server socket
		if (listen(server_socks[i], MAX_BACKLOG) < 0) {
			perror("listen");
			exit(EXIT_FAILURE);
		}
		printf("Listening on port %d\n", PORT_START + i);

		ev.events = EPOLLIN | EPOLLRDHUP;
		ev.data.fd = server_socks[i];
		epoll_ctl( epollfd, EPOLL_CTL_ADD, server_socks[i], &ev);
	}

	// accept the incoming connection
	printf("Waiting for connections ...\n");


	while (1) {
		int addrlen = sizeof(address);
		int activity;

		activity = epoll_wait( epollfd, events, MAXEVENTS, -1);
		if ((activity < 0) && (errno!=EINTR)) {
			printf("epoll wait error");
		}
		//printf("\n*activity: %d\n",activity);

		// If something happened on the server socket,
		// then its an incoming connection
		int fd;
		for (i = 0; i < activity; i++)
		{
			fd = events[i].data.fd;
			for( j = 0; j < PORT_COUNT; j++)
			{
				if( server_socks[j] == fd )
				{
					if ( events[i].events & EPOLLIN) {
						int new_socket;

						if ((new_socket = accept( fd, (struct sockaddr *)&address, (socklen_t*)&addrlen))<0) {
							perror("accept");
							exit(EXIT_FAILURE);
						}
						// inform user of socket number,
						// used in send and receive commands
						printf("New connection: socket fd=%d, ip=%s, port=%d (%d)\n",
								new_socket, inet_ntoa(address.sin_addr),
								ntohs(address.sin_port), PORT_START + i);
						// add new socket to array of sockets
						for (j = 0; j < MAX_CLIENTS; j++) {
							// if position is empty
							if (sockets[j] == -1) {
								sockets[j] = new_socket;
								ev.events = EPOLLIN | EPOLLRDHUP | EPOLLET;
								ev.data.fd = new_socket;
								epoll_ctl( epollfd, EPOLL_CTL_ADD, new_socket, &ev);
								clients++;
								printf("Adding to list of sockets as %d\n" , j);
								break;
							}
						}
					}
				}
				else
				{
					// Check if it was for closing,
					// and also read the incoming message
					if ( events[i].events & EPOLLRDHUP) {
						// Somebody disconnected,
						// get his details and print
						getpeername( fd, (struct sockaddr*)&address , (socklen_t*)&addrlen);
						printf("Host disconnected, ip=%s, port=%d, fd=%d\n",
								inet_ntoa(address.sin_addr),
								ntohs(address.sin_port),
								fd);

						// Close the socket and mark as 0 in
						// list for reuse
						epoll_ctl( epollfd, EPOLL_CTL_DEL, fd, &ev);
						close(fd);
						int j;
						for(j=0; j<MAX_CLIENTS; j++) {
							if( fd == sockets[j] ) {
								sockets[j] = -1;
								if(clients > 0)	clients--;
							}
						}
					}
					else if ( events[i].events & EPOLLIN) {
						/* Tengo que mandar una tarea a trabajar */
						do {
							rc = threadpool_add( pool, &taskClientHandle, (void*)fd, 0);
							/* Si la lista esta llena intentamos denuevo */
						} while( rc == threadpool_queue_full );
						pthread_mutex_lock(&lock);
						tasks++;
						pthread_mutex_unlock(&lock);
					}
				}
			}
		}// for (i = 0; i < activity; i++)
	}// while(1)

	return 0;
}




void taskClientHandle(void *arg)
{
	char buffer[1025];  // data buffer of 1K
	int valread;
	int fd;

	/* Recivo el file descriptor del cliente */
	fd = (int)arg;


	if ((valread = read(fd, buffer, 1024)) > 0) {
		// set the string terminating NULL byte
		// on the end of the data read
		printf("respond to fd: %d\n", fd);
		buffer[valread] = '\0';
		if (send(fd, buffer, strlen(buffer),MSG_NOSIGNAL) != (int)strlen(buffer)) {
			printf("error al enviar por fd: %d\n",fd);
			perror("send");
		}
	}
	pthread_mutex_lock(&lock);
	done++;
	pthread_mutex_unlock(&lock);

	return;
}


