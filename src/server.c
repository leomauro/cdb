#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include "fmt.h"
#include "comm.h"
#include "mem.h"
#include "seq.h"

static char rcsid[] = "$Id$";

static SOCKET in, out;

static Nub_callback_T breakhandler;

static void sendout(int op, const void *buf, int size) {
	Header_T msg;

	msg = op;
	tracemsg("%s: sending %s\n", identity, msgname(msg));
	sendmsg(out, &msg, sizeof msg);
	if (size > 0) {
		assert(buf);
		tracemsg("%s: sending %d bytes\n", identity, size);
		sendmsg(out, buf, size);
	}
}

void _Nub_init(Nub_callback_T startup, Nub_callback_T fault) {
	Header_T msg;
	Nub_state_T state;

	recvmsg(in, &msg, sizeof msg);
	assert(msg == NUB_STARTUP);
	recvmsg(in, &state, sizeof state);
	startup(state);
	for (;;) {
		sendout(NUB_CONTINUE, NULL, 0);
		recvmsg(in, &msg, sizeof msg);
		tracemsg("%s: switching on %s\n", identity, msgname(msg));
		switch (msg) {
		case NUB_BREAK:
			recvmsg(in, &state, sizeof state);
			(*breakhandler)(state);
			break;
		case NUB_FAULT:
			recvmsg(in, &state, sizeof state);
			fault(state);
			break;
		case NUB_QUIT: return;
		default: assert(0);
		}
	}
}

Nub_callback_T _Nub_set(Nub_coord_T src, Nub_callback_T onbreak) {
	Nub_callback_T prev = breakhandler;

	breakhandler = onbreak;
	sendout(NUB_SET, &src, sizeof src);
	return prev;
}

Nub_callback_T _Nub_remove(Nub_coord_T src) {
	sendout(NUB_REMOVE, &src, sizeof src);
	return breakhandler;
}

int _Nub_fetch(int space, void *address, void *buf, int nbytes) {
	struct nub_fetch args;

	args.space = space;
	args.address = address;
	args.nbytes = nbytes;
	sendout(NUB_FETCH, &args, sizeof args);
	recvmsg(in, &args.nbytes, sizeof args.nbytes);
	recvmsg(in, buf, args.nbytes);
	return args.nbytes;
}

int _Nub_store(int space, void *address, void *buf, int nbytes) {
	struct nub_store args;

	args.space = space;
	args.address = address;
	args.nbytes = nbytes;
	assert(nbytes <= sizeof args.buf);
	memcpy(args.buf, buf, nbytes);
	sendout(NUB_STORE, &args, sizeof args);
	recvmsg(in, &args.nbytes, sizeof args.nbytes);
	return args.nbytes;
}

int _Nub_frame(int n, Nub_state_T *state) {
	struct nub_frame args;

	args.n = n;
	sendout(NUB_FRAME, &args, sizeof args);
	recvmsg(in, &args, sizeof args);
	if (args.n >= 0)
		memcpy(state, &args.state, sizeof args.state);
	return args.n;
}

void _Nub_src(Nub_coord_T src,
	void apply(int i, Nub_coord_T *src, void *cl), void *cl) {
	int i = 0, n;
	static Seq_T srcs;

	if (srcs == NULL)
		srcs = Seq_new(0);
	n = Seq_length(srcs);
	sendout(NUB_SRC, &src, sizeof src);
	recvmsg(in, &src, sizeof src);
	while (src.y) {
		Nub_coord_T *p;
		if (i < n)
			p = Seq_get(srcs, i);
		else {
			Seq_addhi(srcs, NEW(p));
			n++;
		}
		*p = src;
		i++;
		recvmsg(in, &src, sizeof src);
	}
	for (n = 0; n < i; n++)
		apply(n, Seq_get(srcs, n), cl);
}

extern void _Cdb_startup(Nub_state_T state), _Cdb_fault(Nub_state_T state);

static void cleanup(void) {
	if (out)
		sendout(NUB_QUIT, NULL, 0);
#ifdef _WIN32
	WSACleanup();
#endif
}

#ifdef unix
static in WSAGetLastError(void) {
	return errno;
}
#endif

static int server(short port) {
	struct sockaddr_in server;
	SOCKET fd = socket(AF_INET, SOCK_STREAM, 0);

	if (fd == SOCKET_ERROR) {
		perror(Fmt_string("%s: socket (%d)", identity, WSAGetLastError()));
		return EXIT_FAILURE;
	}
	memset(&server, 0, sizeof server);
	server.sin_family = AF_INET;
	server.sin_addr.s_addr = htonl(INADDR_ANY);
	server.sin_port = htons(port);
	if (bind(fd, (struct sockaddr *)&server, sizeof server) != 0) {
		perror(Fmt_string("%s: bind (%d)", identity, WSAGetLastError()));
		return EXIT_FAILURE;
	}
	printf("%s listening on %s:%d\n", identity, inet_ntoa(server.sin_addr), ntohs(server.sin_port));
	listen(fd, 5);
	for (;;) {
		struct sockaddr_in client;
		int len = sizeof client;
		in = accept(fd, (struct sockaddr *)&client, &len);
		if (in == SOCKET_ERROR) {
			perror(Fmt_string("%s: accept (%d)", identity, WSAGetLastError()));
			return EXIT_FAILURE;
		}
		printf("%s: now serving %s:%d\n", identity, inet_ntoa(client.sin_addr), ntohs(client.sin_port));
		out = in;
		_Nub_init(_Cdb_startup, _Cdb_fault);
		out = 0;
		closesocket(in);
		printf("%s: disconnected\n", identity);
	}
}

int main(int argc, char *argv[]) {
	short port = 9001;
	char *s;

	if ((s = getenv("TRACE")) != NULL)
		trace = atoi(s);
	identity = argv[0];
#if _WIN32
	{
		WSADATA wsaData;
		int err = WSAStartup(MAKEWORD(2, 0), &wsaData);
		if (err != 0) {
			fprintf(stderr, "%s: WSAStartup (%d)", identity, err);
			return EXIT_FAILURE;
		}
	}
#endif
	atexit(cleanup);
	if (argc > 1)
		port = atoi(argv[1]);
	return server(port);
}
