/**
 * Copyright 2021, Philip Meulengracht
 *
 * This program is free software : you can redistribute it and / or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation ? , either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.If not, see <http://www.gnu.org/licenses/>.
 *
 *
 * Gracht Socket Link Type Definitions & Structures
 * - This header describes the base link-structure, prototypes
 *   and functionality, refer to the individual things for descriptions
 */

#ifndef __GRACHT_SOCKET_OS_H__
#define __GRACHT_SOCKET_OS_H__

#if defined(MOLLENOS)
#include <inet/socket.h>
#include <ioset.h>
#include <io.h>

static int socket_aio_add(int aio, int iod) {
    struct ioset_event event = {
        .events = IOSETIN | IOSETCTL | IOSETLVT,
        .data.iod = iod
    };
    return ioset_ctrl(aio, IOSET_ADD, iod, &event);
}

#define socket_aio_remove(aio, iod) ioset_ctrl(aio, IOSET_DEL, iod, NULL);

#elif defined(__linux__)
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/socket.h>

static int socket_aio_add(int aio, int iod) {
    struct epoll_event event = {
        .events = EPOLLIN | EPOLLRDHUP,
        .data.fd = iod
    };
    return epoll_ctl(aio, EPOLL_CTL_ADD, iod, &event);
}

#define socket_aio_remove(aio, iod) epoll_ctl(aio, EPOLL_CTL_DEL, iod, NULL)

#elif defined(_WIN32)
#include <windows.h>
#include "aio.h"
#include "utils.h"
#include <io.h>
#include <stdlib.h>
#define close closesocket

#define MSG_DONTWAIT 0x10000

struct iocp_socket {
    SOCKET              socket;
    WSAEVENT            events;
    struct iocp_socket* link;
};

static int socket_aio_add(gracht_handle_t aio, gracht_conn_t iod) {
    struct iocp_socket* iocpEntry = malloc(sizeof(struct iocp_socket));
    struct iocp_handle* iocp      = (struct iocp_handle*)aio;
    struct iocp_socket* itr;

    if (!iocpEntry) {
        return -1;
    }

    iocpEntry->link   = NULL;
    iocpEntry->socket = (SOCKET)iod;
    iocpEntry->events = WSACreateEvent();
    if (iocpEntry->events == WSA_INVALID_EVENT) {
        free(iocpEntry);
        return -2;
    }

    // enable events for the socket
    if (WSAEventSelect(iocpEntry->socket, iocpEntry->events, FD_ACCEPT | FD_READ | FD_CLOSE) == SOCKET_ERROR) {
        WSACloseEvent(iocpEntry->events);
        free(iocpEntry);
        return -3;
    }

    HANDLE handle = CreateIoCompletionPort(iocpEntry->socket, iocp->iocp, (DWORD)iod, 0);
    if (!handle) {
        WSACloseEvent(iocpEntry->events);
        free(iocpEntry);
        return -4;
    }

    // add it to the iocp list
    itr = iocp->head;
    if (!itr) {
        iocp->head = iocpEntry;
    }
    else {
        while (itr->link) {
            itr = itr->link;
        }
        itr->link = iocpEntry;
    }
    return 0;
}

static int socket_aio_remove(gracht_handle_t aio, gracht_conn_t iod)
{
    struct iocp_handle* iocp = (struct iocp_handle*)aio;
    struct iocp_socket* itr  = iocp->head;
    struct iocp_socket* prev = NULL;
    
    // remove it from the list of iocp
    while (itr) {
        if (itr->socket == (SOCKET)iod) {
            if (prev) {
                prev->link = itr->link;
                break;
            }
            else {
                iocp->head = itr->link;
                break;
            }
        }

        prev = itr;
        itr  = itr->link;
    }

    if (itr) {
        WSACloseEvent(itr->events);
        free(itr);
        return 0;
    }
    return -1;
}

#endif

#endif // !__GRACHT_SOCKET_OS_H__
