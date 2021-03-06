/*
 * Copyright (c) 2015, Stefan Lankes, RWTH Aachen University
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *    * Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *    * Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *    * Neither the name of the University nor the names of its contributors
 *      may be used to endorse or promote products derived from this
 *      software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <linux/tcp.h>

#define MAX_PATH	255
#define INADDR(a, b, c, d) (struct in_addr) { .s_addr = ((((((d) << 8) | (c)) << 8) | (b)) << 8) | (a) }

#define HERMIT_PORT	0x494E
#define HERMIT_IP(isle)	INADDR(192, 168, 28, isle + 2)
#define HERMIT_MAGIC	0x7E317
#define HERMIT_ELFOSABI	0x42

#define __HERMIT_exit	0
#define __HERMIT_write	1
#define __HERMIT_open	2
#define __HERMIT_close	3
#define __HERMIT_read	4
#define __HERMIT_lseek	5

static int sobufsize = 131072;
static unsigned int isle_nr = 0;

extern char **environ;

static void stop_hermit(void);
static void dump_log(void);

static void fini_env(void)
{
	dump_log();
	stop_hermit();
}

static void exit_handler(int sig)
{
	exit(0);
}

static int init_env(const char *path)
{
	int ret;
	char* str;
	FILE* file;
	char isle_path[MAX_PATH];
	char* result;
	struct sigaction sINT, sTERM;

	// define action for SIGINT
	sINT.sa_handler = exit_handler;
	sINT.sa_flags = 0;
	if (sigaction(SIGINT, &sINT, NULL) < 0)
	{
		perror("sigaction");
		exit(1);
	}

	// define action for SIGTERM
	sTERM.sa_handler = exit_handler;
	sTERM.sa_flags = 0;
	if (sigaction(SIGTERM, &sTERM, NULL) < 0)
	{
		perror("sigaction");
		exit(1);
	}

	str = getenv("HERMIT_ISLE");
	if (str)
	{
		isle_nr = atoi(str);
		if (isle_nr > 254)
			isle_nr = 0;
	}

	// set path to temporary file
	snprintf(isle_path, MAX_PATH, "/sys/hermit/isle%d/path", isle_nr);
	file = fopen(isle_path, "w");
	if (!file) {
		perror("fopen");
		exit(1);
	}

	fprintf(file, "%s", path);
	fclose(file);

	// start application
	snprintf(isle_path, MAX_PATH, "/sys/hermit/isle%d/cpus", isle_nr);
	file = fopen(isle_path, "w");
	if (!file) {
		perror("fopen");
		exit(1);
	}

	str = getenv("HERMIT_CPUS");
	if (str)
		fprintf(file, "%s", str);
	else
		fprintf(file, "%s", "1");

	fclose(file);

	// check result
	file = fopen(isle_path, "r");
	if (!file) {
		perror("fopen");
		exit(1);
	}

	result = NULL;
	ret = fscanf(file, "%ms", &result);

	fclose(file);

	if (ret <= 0) {
		fprintf(stderr, "Unable to check the boot process!\n");
		exit(1);
	}

	if (strcmp(result, "-1") == 0) {
		free(result);
		fprintf(stderr, "Unable to boot cores %s\n", str ? str : "1");
		exit(1);
	}

	free(result);

	return 0;
}

static void dump_log(void)
{
	char isle_path[MAX_PATH];
	char* str = getenv("HERMIT_VERBOSE");
	FILE* file;
	char line[2048];

	if (!str)
		return;

	snprintf(isle_path, MAX_PATH, "/sys/hermit/isle%d/log", isle_nr);
	file = fopen(isle_path, "r");
	if (!file) {
		perror("fopen");
		return;
	}

	puts("\nDump kernel log:");
	puts("================\n");

	while(fgets(line, 2048, file)) {
		printf("%s", line);
	}

	fclose(file);
}

static void stop_hermit(void)
{
	FILE* file;
	char isle_path[MAX_PATH];

	fflush(stdout);
	fflush(stderr);

	snprintf(isle_path, MAX_PATH, "/sys/hermit/isle%d/cpus", isle_nr);

	file = fopen(isle_path, "w");
	if (!file) {
		perror("fopen");
		return;
	}

	fprintf(file, "-1");

	fclose(file);
}

/*
 * in principle, HermitCore forwards basic system calls to
 * this proxy, which mapped these call to Linux system calls.
 */
int handle_syscalls(int s)
{
	int ret;
	int sysnr;

	while(1)
	{
		ret = read(s, &sysnr, sizeof(sysnr));
		if (ret < 0)
			goto out;

		switch(sysnr)
		{
		case __HERMIT_exit: {
			int arg = 0;

			ret = read(s, &arg, sizeof(arg));
			if (ret < 0)
				goto out;
			close(s);

			// already called by fini_env
			//dump_log();
			//stop_hermit();

			exit(arg);
			break;
		}
		case __HERMIT_write: {
			int fd;
			ssize_t j;
			size_t len;
			char* buff;

			ret = read(s, &fd, sizeof(fd));
			if (ret < 0)
				goto out;
			ret = read(s, &len, sizeof(len));
			if (ret < 0)
				goto out;

			buff = malloc(len);
			if (!buff) {
				fprintf(stderr,"Proxy: not enough memory");
				return 1;
			}

			j=0;
			while(j < len)
			{
				ret = read(s, buff+j, len-j);
				if (ret < 0)
					goto out;
				j += len;
			}

			j = write(fd, buff, len);
			if (fd > 2)
				write(s, &j, sizeof(j));

			free(buff);
			break;
		}
		case __HERMIT_open: {
			size_t j, len;
			char* fname;
			int flags, mode;

			ret = read(s, &len, sizeof(len));
			if (ret < 0)
				goto out;

			fname = malloc(len);
			if (!fname)
				goto out;

			j = 0;
			while(j < len)
			{
				ret = read(s, fname+j, len-j);
				if (ret < 0)
					goto out;

				j += ret;
			}

			ret = read(s, &flags, sizeof(flags));
			if (ret < 0)
				goto out;

			ret = read(s, &mode, sizeof(mode));
			if (ret < 0)
				goto out;

			//printf("flags 0x%x, mode 0x%x\n", flags, mode);

			ret = open(fname, flags, mode);
			write(s, &ret, sizeof(ret));

			free(fname);
			break;
		}
		case __HERMIT_close: {
			int fd;

			ret = read(s, &fd, sizeof(fd));
			if (ret < 0)
				goto out;

			if (fd > 2)
				ret = close(fd);
			else
				ret = 0;

			ret = write(s, &ret, sizeof(ret));
			if (ret < 0)
				goto out;
			break;
		}
		case __HERMIT_read: {
			int fd, flag;
			size_t len;
			ssize_t j;
			char* buff;

			ret = read(s, &fd, sizeof(fd));
			if (ret < 0)
				goto out;

			ret = read(s, &len, sizeof(len));
			if (ret < 0)
				goto out;

			buff = malloc(len);
			if (!buff)
				goto out;

			j = read(fd, buff, len);

			flag = 0;
			setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char *) &flag, sizeof(int));

			write(s, &j, sizeof(j));

			if (j > 0)
			{
				ssize_t i = 0;

				while(i < j)
				{
					ret = write(s, buff+i, j-i);
					if (ret < 0)
						break;

					i += ret;
				}
			}

			flag = 1;
			setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char *) &flag, sizeof(int));

			free(buff);
			break;
		}
		case __HERMIT_lseek: {
			int fd, whence;
			off_t offset;

			read(s, &fd, sizeof(fd));
			read(s, &offset, sizeof(offset));
			read(s, &whence, sizeof(whence));

			offset = lseek(fd, offset, whence);
			write(s, &offset, sizeof(offset));
			break;
		}
		default:
			fprintf(stderr, "Proxy: invalid syscall number %d, errno %d\n", sysnr, errno);
			close(s);
			exit(1);
			break;
		}
	}

out:
	perror("Proxy -- communication error");

	return 1;
}

int main(int argc, char **argv)
{
	int i, j, ret, s;
	int32_t magic = HERMIT_MAGIC;
	struct sockaddr_in serv_name;

	init_env(argv[1]);
	atexit(fini_env);

	/* create a socket */
	s = socket(PF_INET, SOCK_STREAM, 0);
	if (s < 0)
	{
		perror("Proxy: socket creation error");
		exit(1);
	}

	setsockopt(s, SOL_SOCKET, SO_RCVBUF, (char *) &sobufsize, sizeof(sobufsize));
	setsockopt(s, SOL_SOCKET, SO_SNDBUF, (char *) &sobufsize, sizeof(sobufsize));
	i = 1;
	setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char *) &i, sizeof(i));
	i = 0;
	setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, (char *) &i, sizeof(i));

	/* server address  */
	memset((char *) &serv_name, 0x00, sizeof(serv_name));
	serv_name.sin_family = AF_INET;
	serv_name.sin_addr = HERMIT_IP(isle_nr);
	serv_name.sin_port = htons(HERMIT_PORT);

	i = 0;
retry:
	ret = connect(s, (struct sockaddr*)&serv_name, sizeof(serv_name));
	if (ret < 0)
	{
		i++;
		if (i <= 10) {
			usleep(10000);
			goto retry;
		}
		perror("Proxy -- connection error");
		close(s);
		exit(1);
	}

	ret = write(s, &magic, sizeof(magic));
	if (ret < 0)
		goto out;

	// forward program arguments to HermitCore
	// argv[0] is path of this proxy so we strip it

	argv++;
	argc--;

	ret = write(s, &argc, sizeof(argc));
	if (ret < 0)
		goto out;

	for(i=0; i<argc; i++)
	{
		int len = strlen(argv[i])+1;

		ret = write(s, &len, sizeof(len));
		if (ret < 0)
			goto out;

		j = 0;
		while(j < len)
		{
			ret = write(s, argv[i]+j, len-j);
			if (ret < 0)
				goto out;
			j += ret;
		}
	}

	// send environment
	i = 0;
	while(environ[i])
		i++;

	ret = write(s, &i, sizeof(i));
	if (ret < 0)
		goto out;

	for(i=0; environ[i] ;i++)
	{
		int len = strlen(environ[i])+1;

		ret = write(s, &len, sizeof(len));
		if (ret < 0)
			goto out;

		j = 0;
		while(j < len)
		{
			ret = write(s, environ[i]+j, len-j);
			if (ret < 0)
				goto out;
			j += ret;
		}
	}

	ret = handle_syscalls(s);

	close(s);

	return ret;

out:
	perror("Proxy -- communication error");
	close(s);
	return 1;
}
