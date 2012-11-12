/*
 * Copyright (c), Microsoft Open Technologies, Inc.
 * All rights reserved.
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *  - Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *  - Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/* IOCP-based ae.c module  */

#include <string.h>
#include "ae.h"
#include "win32fixes.h"
#include "zmalloc.h"
#include "adlist.h"
#include "win32_wsiocp.h"
#include <mswsock.h>
#include <Guiddef.h>

/* Note: On Win64 a SOCKET is 64 bits, but an int is 32 bits.
 *       Redis typically uses int where a SOCKET would be used.
 *       It is safe to cast between SOCKET and int because a
 *       object handle value is limited to 2^24.  */

/* Use GetQueuedCompletionStatusEx if possible.
 * Try to load the function pointer dynamically.
 * If it is not available, use GetQueuedCompletionStatus */
#define MAX_COMPLETE_PER_POLL       100
typedef BOOL (WINAPI *sGetQueuedCompletionStatusEx)
             (HANDLE CompletionPort,
              LPOVERLAPPED_ENTRY lpCompletionPortEntries,
              ULONG ulCount,
              PULONG ulNumEntriesRemoved,
              DWORD dwMilliseconds,
              BOOL fAlertable);
sGetQueuedCompletionStatusEx pGetQueuedCompletionStatusEx;

/* lookup structure for socket
 * socket value is not an index. Convert socket to index
  * and then find matching structure in list */

/* prefer prime number for number of indexes */
#define MAX_SOCKET_LOOKUP   1021

/* structure that keeps state of sockets and Completion port handle */
typedef struct aeApiState {
    HANDLE iocp;
    int setsize;
    OVERLAPPED_ENTRY entries[MAX_COMPLETE_PER_POLL];
    list lookup[MAX_SOCKET_LOOKUP];
    list closing;
} aeApiState;

/* convert socket value to an index
 * Use simple modulo. We can add hash if needed */
int aeSocketIndex(int fd) {
    return fd % MAX_SOCKET_LOOKUP;
}

/* get data for socket / fd being monitored. Create if not found*/
aeSockState *aeGetSockState(void *apistate, int fd) {
    int sindex;
    listNode *node;
    list *socklist;
    aeSockState *sockState;
    if (apistate == NULL) return NULL;

    sindex = aeSocketIndex(fd);
    socklist = &(((aeApiState *)apistate)->lookup[sindex]);
    node = listFirst(socklist);
    while (node != NULL) {
        sockState = (aeSockState *)listNodeValue(node);
        if (sockState->fd == fd) {
            return sockState;
        }
        node = listNextNode(node);
    }
    // not found. Do lazy create of sockState.
    sockState = (aeSockState *) zmalloc(sizeof(aeSockState));
    if (sockState != NULL) {
        sockState->fd = fd;
        sockState->masks = 0;
        sockState->wreqs = 0;
        sockState->reqs = NULL;
        memset(&sockState->wreqlist, 0, sizeof(sockState->wreqlist));

        if (listAddNodeHead(socklist, sockState) != NULL) {
            return sockState;
        } else {
            zfree(sockState);
        }
    }
    return NULL;
}

/* get data for socket / fd being monitored */
aeSockState *aeGetExistingSockState(void *apistate, int fd) {
    int sindex;
    listNode *node;
    list *socklist;
    aeSockState *sockState;
    if (apistate == NULL) return NULL;

    sindex = aeSocketIndex(fd);
    socklist = &(((aeApiState *)apistate)->lookup[sindex]);
    node = listFirst(socklist);
    while (node != NULL) {
        sockState = (aeSockState *)listNodeValue(node);
        if (sockState->fd == fd) {
            return sockState;
        }
        node = listNextNode(node);
    }

    return NULL;
}

// find matching value in list and remove. If found return 1
int removeMatchFromList(list *socklist, void *value) {
    listNode *node;
    node = listFirst(socklist);
    while (node != NULL) {
        if (listNodeValue(node) == value) {
            listDelNode(socklist, node);
            return 1;
        }
        node = listNextNode(node);
    }
    return 0;
}

/* delete data for socket / fd being monitored */
void aeDelSockState(void *apistate, aeSockState *sockState) {
    int sindex;
    list *socklist;

    if (apistate == NULL) return;

    if (sockState->wreqs == 0 && (sockState->masks & (READ_QUEUED | SOCKET_ATTACHED)) == 0) {
        // see if in active list
        sindex = aeSocketIndex(sockState->fd);
        socklist = &(((aeApiState *)apistate)->lookup[sindex]);
        if (removeMatchFromList(socklist, sockState) == 1) {
            zfree(sockState);
            return;
        }
        // try closing list
        socklist = &(((aeApiState *)apistate)->closing);
        if (removeMatchFromList(socklist, sockState) == 1) {
            zfree(sockState);
            return;
        }
    } else {
        // not safe to delete. Move to closing
        sindex = aeSocketIndex(sockState->fd);
        socklist = &(((aeApiState *)apistate)->lookup[sindex]);
        if (removeMatchFromList(socklist, sockState) == 1) {
            // removed from active list. add to closing list
            socklist = &(((aeApiState *)apistate)->closing);
            listAddNodeHead(socklist, sockState);
        }
    }
}

/* Called by ae to initialize state */
static int aeApiCreate(aeEventLoop *eventLoop) {
    HMODULE kernel32_module;
    aeApiState *state = (aeApiState *)zmalloc(sizeof(aeApiState));

    if (!state) return -1;
    memset(state, 0, sizeof(aeApiState));

    /* create a single IOCP to be shared by all sockets */
    state->iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE,
                                         NULL,
                                         0,
                                         1);
    if (state->iocp == NULL) {
        zfree(state);
        return -1;
    }

    pGetQueuedCompletionStatusEx = NULL;
    kernel32_module = GetModuleHandleA("kernel32.dll");
    if (kernel32_module != NULL) {
        pGetQueuedCompletionStatusEx = (sGetQueuedCompletionStatusEx) GetProcAddress(
                                        kernel32_module,
                                        "GetQueuedCompletionStatusEx");
    }

    state->setsize = AE_SETSIZE;
    eventLoop->apidata = state;
    /* initialize the IOCP socket code with state reference */
    aeWinInit(state, state->iocp, aeGetSockState, aeDelSockState);
    return 0;
}

/* termination */
static void aeApiFree(aeEventLoop *eventLoop) {
    aeApiState *state = (aeApiState *)eventLoop->apidata;
    CloseHandle(state->iocp);
    zfree(state);
    aeWinCleanup();
}

/* monitor state changes for a socket */
static int aeApiAddEvent(aeEventLoop *eventLoop, int fd, int mask) {
    aeApiState *state = (aeApiState *)eventLoop->apidata;
    aeSockState *sockstate = aeGetSockState(state, fd);
    if (sockstate == NULL) {
        errno = WSAEINVAL;
        return -1;
    }

    if (mask & AE_READABLE) {
        sockstate->masks |= AE_READABLE;
        if (sockstate->masks & LISTEN_SOCK) {
            /* actually a listen. Do not treat as read */
        } else {
            if ((sockstate->masks & READ_QUEUED) == 0) {
                // queue up a 0 byte read
                aeWinReceiveDone(fd);
            }
        }
    }
    if (mask & AE_WRITABLE) {
        sockstate->masks |= AE_WRITABLE;
        // if no write active, then need to queue write ready
        if (sockstate->wreqs == 0) {
            asendreq *areq = (asendreq *)zmalloc(sizeof(asendreq));
            memset(areq, 0, sizeof(asendreq));
            if (PostQueuedCompletionStatus(state->iocp,
                                        0,
                                        fd,
                                        &areq->ov) == 0) {
                errno = GetLastError();
                zfree(areq);
                return -1;
            }
            sockstate->wreqs++;
            listAddNodeTail(&sockstate->wreqlist, areq);
        }
    }
    return 0;
}

/* stop monitoring state changes for a socket */
static void aeApiDelEvent(aeEventLoop *eventLoop, int fd, int mask) {
    aeApiState *state = (aeApiState *)eventLoop->apidata;
    aeSockState *sockstate = aeGetExistingSockState(state, fd);
    if (sockstate == NULL) {
        errno = WSAEINVAL;
        return;
    }

    if (mask & AE_READABLE) sockstate->masks &= ~AE_READABLE;
    if (mask & AE_WRITABLE) sockstate->masks &= ~AE_WRITABLE;
}

/* return array of sockets that are ready for read or write 
   depending on the mask for each socket */
static int aeApiPoll(aeEventLoop *eventLoop, struct timeval *tvp) {
    aeApiState *state = (aeApiState *)eventLoop->apidata;
    aeSockState *sockstate;
    ULONG j;
    int numevents = 0;
    ULONG numComplete = 0;
    int rc;
    int mswait = (tvp->tv_sec * 1000) + (tvp->tv_usec / 1000);

    if (pGetQueuedCompletionStatusEx != NULL) {
        /* first get an array of completion notifications */
        rc = pGetQueuedCompletionStatusEx(state->iocp,
                                        state->entries,
                                        MAX_COMPLETE_PER_POLL,
                                        &numComplete,
                                        mswait,
                                        FALSE);
    } else {
        /* need to get one at a time. Use first array element */
        rc = GetQueuedCompletionStatus(state->iocp,
                                        &state->entries[0].dwNumberOfBytesTransferred,
                                        &state->entries[0].lpCompletionKey,
                                        &state->entries[0].lpOverlapped,
                                        mswait);
        if (!rc && state->entries[0].lpOverlapped == NULL) {
            // timeout. Return.
            return 0;
        } else {
            // check if more completions are ready
            int lrc = 1;
            rc = 1;
            numComplete = 1;

            while (numComplete < MAX_COMPLETE_PER_POLL) {
                lrc = GetQueuedCompletionStatus(state->iocp,
                                                &state->entries[numComplete].dwNumberOfBytesTransferred,
                                                &state->entries[numComplete].lpCompletionKey,
                                                &state->entries[numComplete].lpOverlapped,
                                                0);
                if (lrc) {
                   numComplete++;
                } else {
                    if (state->entries[numComplete].lpOverlapped == NULL) break;
                }
            }
        }
    }

    if (rc && numComplete > 0) {
        LPOVERLAPPED_ENTRY entry = state->entries;
        for (j = 0; j < numComplete && numevents < AE_SETSIZE; j++, entry++) {
            /* the competion key is the socket */
            SOCKET sock = (SOCKET)entry->lpCompletionKey;
            sockstate = aeGetExistingSockState(state, (int)sock);
            if (sockstate == NULL) continue;

            if ((sockstate->masks & LISTEN_SOCK) && entry->lpOverlapped != NULL) {
                /* need to set event for listening */
                aacceptreq *areq = (aacceptreq *)entry->lpOverlapped;
                areq->next = sockstate->reqs;
                sockstate->reqs = areq;
                sockstate->masks &= ~ACCEPT_PENDING;
                if (sockstate->masks & AE_READABLE) {
                    eventLoop->fired[numevents].fd = (int)sock;
                    eventLoop->fired[numevents].mask = AE_READABLE;
                    numevents++;
                }
            } else {
                /* check if event is read complete (may be 0 length read) */
                int matched = 0;
                if (entry->lpOverlapped == &sockstate->ov_read) {
                    matched = 1;
                    sockstate->masks &= ~READ_QUEUED;
                    if (sockstate->masks & AE_READABLE) {
                        eventLoop->fired[numevents].fd = (int)sock;
                        eventLoop->fired[numevents].mask = AE_READABLE;
                        numevents++;
                    }
                } else if (sockstate->wreqs > 0 && entry->lpOverlapped != NULL) {
                    /* should be write complete. Get results */
                    asendreq *areq = (asendreq *)entry->lpOverlapped;
                    matched = removeMatchFromList(&sockstate->wreqlist, areq);
                    if (matched) {
                        /* call write complete callback so buffers can be freed */
                        if (areq->proc != NULL) {
                            DWORD written = 0;
                            DWORD flags;
                            WSAGetOverlappedResult(sock, &areq->ov, &written, FALSE, &flags);
                            areq->proc(areq->eventLoop, (int)sock, &areq->req, (int)written);
                        }
                        sockstate->wreqs--;
                        zfree(areq);
                        /* if no active write requests, set ready to write */
                        if (sockstate->wreqs == 0 && sockstate->masks & AE_WRITABLE) {
                            eventLoop->fired[numevents].fd = (int)sock;
                            eventLoop->fired[numevents].mask = AE_WRITABLE;
                            numevents++;
                        }
                    }
                }
                if (matched == 0) {
                    // no match for active connection.
                    // Try the closing list since socket value is reused.
                    list *socklist = &(state->closing);
                    listNode *node;
                    node = listFirst(socklist);
                    while (node != NULL && matched == 0) {
                        sockstate = (aeSockState *)listNodeValue(node);
                        if (sockstate->fd == sock) {
                            if (entry->lpOverlapped == &sockstate->ov_read) {
                                // read complete
                                sockstate->masks &= ~READ_QUEUED;
                                matched = 1;
                            } else {
                                // check pending writes
                                asendreq *areq = (asendreq *)entry->lpOverlapped;
                                matched = removeMatchFromList(&sockstate->wreqlist, areq);
                                if (matched) {
                                    sockstate->wreqs--;
                                    zfree(areq);
                                }
                            }
                        }
                        node = listNextNode(node);
                    }
                }
                if (matched == 0) {
                    sockstate = NULL;
                }
            }
            if (sockstate != NULL && sockstate->wreqs == 0 &&
                (sockstate->masks & (READ_QUEUED | SOCKET_ATTACHED)) == 0) {
                // safe to delete sockstate
                aeDelSockState(state, sockstate);
            }
        }
    }
    return numevents;
}

/* name of this event handler */
static char *aeApiName(void) {
    return "winsock_IOCP";
}

