﻿/* Establishing and handling network connections.
   Copyright (C) 1995, 1996, 1997 Free Software Foundation, Inc.

This file is part of Wget.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.  */

#include "config.h"

#include <stdio.h>
#include <sys/types.h>
#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif

#ifdef WINDOWS
# include <winsock.h>
#else
# include <sys/socket.h>
# include <netdb.h>
# include <netinet/in.h>
# include <arpa/inet.h>
#endif /* WINDOWS */

#include <errno.h>
#ifdef HAVE_STRING_H
# include <string.h>
#else
# include <strings.h>
#endif /* HAVE_STRING_H */
#ifdef HAVE_SYS_SELECT_H
# include <sys/select.h>
#endif /* HAVE_SYS_SELECT_H */

#include "wget.h"
#include "connect.h"
#include "host.h"

#ifndef errno
extern int errno;
#endif

/* Variables shared by bindport and acceptport: */
static int msock = -1;
static struct sockaddr *addr;


/* Create an internet connection to HOSTNAME on PORT.  The created
   socket will be stored to *SOCK.  */
uerr_t
make_connection (int *sock, char *hostname, unsigned short port)
{
    struct sockaddr_in sock_name;
    /* struct hostent *hptr; */

    /* Get internet address of the host.  We can do it either by calling
       ngethostbyname, or by calling store_hostaddress, from host.c.
       storehostaddress is better since it caches calls to
       gethostbyname.  */
#if 1
    //z 将host保存到Host list 中去
    //z 使用 store_hostaddress 相对要更好一些，因为其 cache 了 gethostbyname 的结果
    if (!store_hostaddress ((unsigned char *)&sock_name.sin_addr, hostname))
        return HOSTERR;
#else  /* never */
    if (!(hptr = ngethostbyname (hostname)))
        return HOSTERR;
    /* Copy the address of the host to socket description.  */
    memcpy (&sock_name.sin_addr, hptr->h_addr, hptr->h_length);
#endif /* never */

    /* Set port and protocol */
    sock_name.sin_family = AF_INET;
    sock_name.sin_port = htons (port);

    /* Make an internet socket, stream type.  */
    if ((*sock = socket (AF_INET, SOCK_STREAM, 0)) == -1)
        return CONSOCKERR;

    /* Connect the socket to the remote host.  */
    if (connect (*sock, (struct sockaddr *) &sock_name, sizeof (sock_name)))
    {
        if (errno == ECONNREFUSED)
            return CONREFUSED;
        else
            return CONERROR;
    }
    DEBUGP (("Created fd %d.\n", *sock));
    return NOCONERROR;
}

/*z
绑定到本地端口 PORT，完成所有所需的工作（创建socket，在其上设置SO_REUSEADDR选项），然后调用bind以及listen
如果*port为0，那么随机选择一个端口，并将其值存回*PORT。内部值MPORT设置为确保master socket的值。
调用 acceptport 来阻止并接受一个连接
*/
/* Bind the local port PORT.  This does all the necessary work, which
   is creating a socket, setting SO_REUSEADDR option on it, then
   calling bind() and listen().  If *PORT is 0, a random port is
   chosen by the system, and its value is stored to *PORT.  The
   internal variable MPORT is set to the value of the ensuing master
   socket.  Call acceptport() to block for and accept a connection.  */
uerr_t
bindport (unsigned short *port)
{
    int optval = 1;
    static struct sockaddr_in srv;

    msock = -1;
    addr = (struct sockaddr *) &srv;
    //z 创建 socket
    if ((msock = socket (AF_INET, SOCK_STREAM, 0)) < 0)
        return CONSOCKERR;
    //z 设置 msock 选项。设置 SO_REUSEADDR
    if (setsockopt (msock, SOL_SOCKET, SO_REUSEADDR,
                    (char *)&optval, sizeof (optval)) < 0)
        return CONSOCKERR;
    srv.sin_family = AF_INET;
    srv.sin_addr.s_addr = htonl (INADDR_ANY);
    srv.sin_port = htons (*port);

    //z bind socket 到 addr （这种细节函数看看UNP）
    if (bind (msock, addr, sizeof (struct sockaddr_in)) < 0)
    {
        //z 绑定失败，关闭socket
        CLOSE (msock);
        msock = -1;
        return BINDERR;
    }
    DEBUGP (("Master socket fd %d bound.\n", msock));

    //z 如果*port为0
    if (!*port)
    {
        size_t addrlen = sizeof (struct sockaddr_in);
        //z The getsockname function retrieves the local name for a socket.
        if (getsockname (msock, addr, (int *)&addrlen) < 0)
        {
            CLOSE (msock);
            msock = -1;
            return CONPORTERR;
        }
        //z 如果传入端口号为0，将随机分配的端口号存储回port中去。
        *port = ntohs (srv.sin_port);
    }
    //z 在msock上进行侦听
    if (listen (msock, 1) < 0)
    {
        CLOSE (msock);
        msock = -1;
        return LISTENERR;
    }
    return BINDOK;
}

#ifdef HAVE_SELECT
/* Wait for file descriptor FD to be readable, MAXTIME being the
   timeout in seconds.  If WRITEP is non-zero, checks for FD being
   writable instead.

   Returns 1 if FD is accessible, 0 for timeout and -1 for error in
   select().  */
//z 等待 fd 变成 readable， MAXTIME 为超时时间，以秒为单位。如果writep不为0，那么检查FD是否为writable的。
static int
select_fd (int fd, int maxtime, int writep)
{
    fd_set fds, exceptfds;
    struct timeval timeout;

    FD_ZERO (&fds);
    FD_SET (fd, &fds);
    FD_ZERO (&exceptfds);
    FD_SET (fd, &exceptfds);

    timeout.tv_sec = maxtime;
    timeout.tv_usec = 0;
    /* HPUX reportedly warns here.  What is the correct incantation?  */
    return select (fd + 1, writep ? NULL : &fds, writep ? &fds : NULL,
                   &exceptfds, &timeout);
}
#endif /* HAVE_SELECT */

/*
	在MSOCK上调用 accept()，将结果储存到*SOCK中去。假设bindport()用于初始化MSOCK为一个合适正确的值。
	这阻塞了调用者直到一个连接建立起来。

	如果在OPT.TIMEOUT秒内没有建立任何连接，函数将会返回一个错误状态码。
*/
/* Call accept() on MSOCK and store the result to *SOCK.  This assumes
   that bindport() has been used to initialize MSOCK to a correct
   value.  It blocks the caller until a connection is established.  If
   no connection is established for OPT.TIMEOUT seconds, the function
   exits with an error status.  */
uerr_t
acceptport (int *sock)
{
    int addrlen = sizeof (struct sockaddr_in);

	//z 设置错误状态码
#ifdef HAVE_SELECT
	//z 设置超时值
    if (select_fd (msock, opt.timeout, 0) <= 0)
        return ACCEPTERR;
#endif
	//z accept 返回的值。
    if ((*sock = accept (msock, addr, &addrlen)) < 0)
        return ACCEPTERR;
    DEBUGP (("Created socket fd %d.\n", *sock));
    return ACCEPTOK;
}

/* Close SOCK, as well as the most recently remembered MSOCK, created
   via bindport().  If SOCK is -1, close MSOCK only.  */
void
closeport (int sock)
{
    /*shutdown (sock, 2);*/
    if (sock != -1)
        CLOSE (sock);
    if (msock != -1)
        CLOSE (msock);
    msock = -1;
}

/* Return the local IP address associated with the connection on FD.
   It is returned in a static buffer.  */
unsigned char *
conaddr (int fd)
{
    //z 本程序中很喜欢使用这种函数内的 static buffer 了。
    //z pros : 很快
    //z cons : 不可重入
    //z 查询了下英文资料 reentrant 以及 non-reentrant
    //z 还有个概念是 thread-safe
    //z 函数可重入，那么需要没有使用static buffer，而且不得调用不可重入的函数
    //z 不可重入的函数一般几乎不可能使之成为 thread-safe 的
    static unsigned char res[4];
    struct sockaddr_in mysrv;
    struct sockaddr *myaddr;
    size_t addrlen = sizeof (mysrv);

    myaddr = (struct sockaddr *) (&mysrv);
    //z 还是多看看UNP这样子了。
    if (getsockname (fd, myaddr, (int *)&addrlen) < 0)
        return NULL;
    memcpy (res, &mysrv.sin_addr, 4);
    return res;
}

/* Read at most LEN bytes from FD, storing them to BUF.  This is
   virtually the same as read(), but takes care of EINTR braindamage
   and uses select() to timeout the stale connections (a connection is
   stale if more than OPT.TIMEOUT time is spent in select() or
   read()).  */
int
iread (int fd, char *buf, int len)
{
    int res;

    do
    {
#ifdef HAVE_SELECT
        //z 是否考虑超时
        if (opt.timeout)
        {
            do
            {
                res = select_fd (fd, opt.timeout, 0);
            }
            while (res == -1 && errno == EINTR);
            if (res <= 0)
            {
                /* Set errno to ETIMEDOUT on timeout.  */
                if (res == 0)
                    /* #### Potentially evil!  */
                    errno = ETIMEDOUT;
                return -1;
            }
        }
#endif
        //#define READ(fd, buf, cnt) recv ((fd), (buf), (cnt), 0)
        res = READ (fd, buf, len);
        //OutputDebugString(buf);
    }
    while (res == -1 && errno == EINTR);

    return res;
}

/*
	从BUF中写入Len字节到FD中去。这类似于iread，但不烦用 select()。
	不同于iread()，其确保BUF中的内容实际写入到FD中，调用者不需要检查返回值是否与LEN同。只需要检查返回值是否为-1。
*/
/* Write LEN bytes from BUF to FD.  This is similar to iread(), but
   doesn't bother with select().  Unlike iread(), it makes sure that
   all of BUF is actually written to FD, so callers needn't bother
   with checking that the return value equals to LEN.  Instead, you
   should simply check for -1.  */
int
iwrite (int fd, char *buf, int len)
{
    int res = 0;

	/*z
		write 可能写入的少于LEN字节数，外循环将持续尝试直至所有内容已经写入，或直至有错误发生。
	*/
    /* `write' may write less than LEN bytes, thus the outward loop
       keeps trying it until all was written, or an error occurred.  The
       inner loop is reserved for the usual EINTR f*kage, and the
       innermost loop deals with the same during select().  */
    while (len > 0)
    {
        do
        {
#ifdef HAVE_SELECT
			//z 等待fd可写
            if (opt.timeout)
            {
                do
                {
                    res = select_fd (fd, opt.timeout, 1);
                }
                while (res == -1 && errno == EINTR);
                if (res <= 0)
                {
                    /* Set errno to ETIMEDOUT on timeout.  */
                    if (res == 0)
                        /* #### Potentially evil!  */
                        errno = ETIMEDOUT;
                    return -1;
                }
            }
#endif

			//z #define READ(fd, buf, cnt) recv ((fd), (buf), (cnt), 0)
			//z #define WRITE(fd, buf, cnt) send ((fd), (buf), (cnt), 0)
			//z 向fd写入（发送）buf内的内容。
            res = WRITE (fd, buf, len);
            OutputDebugString(buf);
        }
        while (res == -1 && errno == EINTR);
        if (res <= 0)
            break;
        buf += res;
        len -= res;
    }
    return res;
}
