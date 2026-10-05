#ifndef NM_TTS_FAKE_SOCKETS_H
#define NM_TTS_FAKE_SOCKETS_H
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <unistd.h>
#include <fcntl.h>
int nm_fake_socket(int, int, int);
int nm_fake_fcntl(int, int, ...);
int nm_fake_connect(int, const struct sockaddr *, socklen_t);
int nm_fake_getsockopt(int, int, int, void *, socklen_t *);
int nm_fake_select(int, fd_set *, fd_set *, fd_set *, struct timeval *);
int nm_fake_send(int, const void *, size_t, int);
int nm_fake_recv(int, void *, size_t, int);
int nm_fake_close(int);
#define socket nm_fake_socket
#define fcntl nm_fake_fcntl
#define connect nm_fake_connect
#define getsockopt nm_fake_getsockopt
#define select nm_fake_select
#define send nm_fake_send
#define recv nm_fake_recv
#define close nm_fake_close
#endif
