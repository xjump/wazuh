/* Copyright (C) 2009 Trend Micro Inc.
 * All rights reserved.
 *
 * This program is a free software; you can redistribute it
 * and/or modify it under the terms of the GNU General Public
 * License (version 2) as published by the FSF - Free Software
 * Foundation
 */

#include "shared.h"
#include "config/config.h"
#include "os_net/os_net.h"

static char * msgsubst(const char * pattern, const char * logmsg, const char * location, time_t timestamp);

int sock_fail_time;

#ifndef WIN32

/* Start the Message Queue. type: WRITE||READ */
int StartMQ(const char *path, short int type)
{
    if (type == READ) {
        return (OS_BindUnixDomain(path, SOCK_DGRAM, OS_MAXSTR + 512));
    }

    /* We give up to 21 seconds for the other end to start */
    else {
        int rc = 0;
        if (File_DateofChange(path) < 0) {
            sleep(1);
            if (File_DateofChange(path) < 0) {
                sleep(5);
                if (File_DateofChange(path) < 0) {
                    sleep(15);
                    if (File_DateofChange(path) < 0) {
                        merror(QUEUE_ERROR, path, "Queue not found");
                        return (-1);
                    }
                }
            }
        }

        /* Wait up to 3 seconds to connect to the unix domain.
         * After three errors, exit.
         */
        if ((rc = OS_ConnectUnixDomain(path, SOCK_DGRAM, OS_MAXSTR + 256)) < 0) {
            sleep(1);
            if ((rc = OS_ConnectUnixDomain(path, SOCK_DGRAM, OS_MAXSTR + 256)) < 0) {
                sleep(2);
                if ((rc = OS_ConnectUnixDomain(path, SOCK_DGRAM, OS_MAXSTR + 256)) < 0) {
                    merror(QUEUE_ERROR, path, strerror(errno));
                    return (-1);
                }
            }
        }

        mdebug1(MSG_SOCKET_SIZE, OS_getsocketsize(rc));
        return (rc);
    }
}

/* Send a message to the queue */
int SendMSG(int queue, const char *message, const char *locmsg, char loc)
{
    int __mq_rcode;
    char tmpstr[OS_MAXSTR + 1];
    static int reported = 0;

    tmpstr[OS_MAXSTR] = '\0';

    /* Check for global locks */
    os_wait();

    if (loc == SECURE_MQ) {
        loc = message[0];
        message++;

        if (message[0] != ':') {
            merror(FORMAT_ERROR);
            return (0);
        }
        message++; /* Pointing now to the location */

        if (strncmp(message, "keepalive", 9) == 0) {
            return (0);
        }

        snprintf(tmpstr, OS_MAXSTR, "%c:%s->%s", loc, locmsg, message);
    } else {
        snprintf(tmpstr, OS_MAXSTR, "%c:%s:%s", loc, locmsg, message);
    }

    /* Queue not available */
    if (queue < 0) {
        return (-1);
    }

    if ((__mq_rcode = OS_SendUnix(queue, tmpstr, 0)) < 0) {
        /* Error on the socket */
        if (__mq_rcode == OS_SOCKTERR) {
            merror("socketerr (not available).");
            close(queue);
            return (-1);
        }

        /* Unable to send. Socket busy */
        mdebug2("Socket busy, discarding message.");

        if (!reported) {
            reported = 1;
            mwarn("Socket busy, discarding message.");
        }
    }

    return (0);
}

/* Send a message to socket */
int SendMSGtoSCK(int queue, const char *message, const char *locmsg, char loc, logsocket **sockets, const char * pattern)
{
    int __mq_rcode;
    char tmpstr[OS_MAXSTR + 1];
    int i;
    time_t mtime = time(NULL);
    char * _message = msgsubst(pattern, message, locmsg, mtime);

    for (i = 0; sockets[i] && sockets[i]->name; i++) {
        if (strcmp(sockets[i]->name, "agent") == 0) {
            SendMSG(queue, _message, locmsg, loc);
        }
        else {
            tmpstr[OS_MAXSTR] = '\0';

            int sock_type;
            const char * strmode;

            switch (sockets[i]->mode) {
            case UDP_PROTO:
                sock_type = SOCK_DGRAM;
                strmode = "udp";
                break;
            case TCP_PROTO:
                sock_type = SOCK_STREAM;
                strmode = "tcp";
                break;
            default:
                merror("At %s(): undefined protocol. This shouldn't happen.", __FUNCTION__);
                free(_message);
                return -1;
            }

            // create message and add prefix
            if (sockets[i]->prefix && *sockets[i]->prefix) {
                snprintf(tmpstr, OS_MAXSTR, "%s%s", sockets[i]->prefix, _message);
            } else {
                snprintf(tmpstr, OS_MAXSTR, "%s", _message);
            }

            // Connect to socket if disconnected
            if (sockets[i]->socket < 0) {
                if (mtime = time(NULL), mtime > sockets[i]->last_attempt + sock_fail_time) {
                    if (sockets[i]->socket = OS_ConnectUnixDomain(sockets[i]->location, sock_type, OS_MAXSTR + 256), sockets[i]->socket < 0) {
                        sockets[i]->last_attempt = mtime;
                        merror("Unable to connect to socket '%s': %s (%s)", sockets[i]->name, sockets[i]->location, strmode);
                        continue;
                    }

                    mdebug1("Connected to socket '%s' (%s)", sockets[i]->name, sockets[i]->location);
                } else {
                    mdebug2("Discarding event from '%s' due to connection issue with '%s'", locmsg, sockets[i]->name);
                    continue;
                }
            }

            // Send msg to socket

            if (__mq_rcode = OS_SendUnix(sockets[i]->socket, tmpstr, 0), __mq_rcode < 0) {
                if (__mq_rcode == OS_SOCKTERR) {
                    if (mtime = time(NULL), mtime > sockets[i]->last_attempt + sock_fail_time) {
                        close(sockets[i]->socket);

                        if (sockets[i]->socket = OS_ConnectUnixDomain(sockets[i]->location, sock_type, OS_MAXSTR + 256), sockets[i]->socket < 0) {
                            merror("Unable to connect to socket '%s': %s (%s)", sockets[i]->name, sockets[i]->location, strmode);
                            sockets[i]->last_attempt = mtime;
                            continue;
                        }

                        mdebug1("Connected to socket '%s' (%s)", sockets[i]->name, sockets[i]->location);

                        if (OS_SendUnix(sockets[i]->socket, tmpstr, 0), __mq_rcode < 0) {
                            merror("Cannot send message to socket '%s'. (Retry)", sockets[i]->name);
                            SendMSG(queue, "Cannot send message to socket.", "logcollector", LOCALFILE_MQ);
                            sockets[i]->last_attempt = mtime;
                            continue;
                        }
                    } else {
                        mdebug2("Discarding event from '%s' due to connection issue with '%s'", locmsg, sockets[i]->name);
                        continue;
                    }

                } else {
                    merror("Cannot send message to socket '%s'. (Retry)", sockets[i]->name);
                    SendMSG(queue, "Cannot send message to socket.", "logcollector", LOCALFILE_MQ);
                    continue;
                }
            }
        }
    }

    free(_message);
    return (0);
}

#else

int SendMSGtoSCK(int queue, const char *message, const char *locmsg, char loc, __attribute__((unused)) logsocket **sockets, const char * pattern) {
    char * _message = msgsubst(pattern, message, locmsg, time(NULL));
    int retval = SendMSG(queue, _message, locmsg, loc);
    free(_message);
    return retval;
}

#endif /* !WIN32 */

char * msgsubst(const char * pattern, const char * logmsg, const char * location, time_t timestamp) {
    char * final;
    char * _pattern;
    char * cur;
    char * tok;
    char * end;
    char * param;
    const char * field;
    char _timestamp[64];
    char hostname[512];
    size_t n = 0;
    size_t z;

    if (!pattern) {
        return strdup(logmsg);
    }

    os_malloc(OS_MAXSTR, final);
    os_strdup(pattern, _pattern);

    for (cur = _pattern; tok = strstr(cur, "$("), tok; cur = end) {
        field = NULL;
        *tok = '\0';

        // Skip $(
        param = tok + 2;

        // Copy anything before the token
        z = strlen(cur);

        if (n + z >= OS_MAXSTR) {
            goto fail;
        }

        strncpy(final + n, cur, OS_MAXSTR - n);
        n += z;

        if (end = strchr(param, ')'), !end) {
            // Token not closed: break
            *tok = '$';
            cur = tok;
            break;
        }

        *end++ = '\0';

        // Find parameter

        if (strcmp(param, "log") == 0 || strcmp(param, "output") == 0) {
            field = logmsg;
        } else if (strcmp(param, "location") == 0 || strcmp(param, "command") == 0) {
            field = location;
        } else if (strncmp(param, "timestamp", 9) == 0) {
            struct tm tm;
            char * format;

            localtime_r(&timestamp, &tm);

            if (format = strchr(param, ' '), format) {
                if (strftime(_timestamp, sizeof(_timestamp), format + 1, &tm)) {
                    field = _timestamp;
                } else {
                    mdebug1("Cannot format time '%s': %s (%d)", format, strerror(errno), errno);
                }
            } else {
                // If format is not speficied, use RFC3164
#ifdef WIN32
                // strfrime() does not allow %e in Windows
                const char * MONTHS[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

                if (snprintf(_timestamp, sizeof(_timestamp), "%s %s%d %02d:%02d:%02d", MONTHS[tm.tm_mon], tm.tm_mday < 10 ? " " : "", tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec) < (int)sizeof(_timestamp)) {
                    field = _timestamp;
                }
#else
                if (strftime(_timestamp, sizeof(_timestamp), "%b %e %T", &tm)) {
                    field = _timestamp;
                }
#endif // WIN32
            }
        } else if (strcmp(param, "hostname") == 0) {
            if (gethostname(hostname, sizeof(hostname)) != 0) {
                strncpy(hostname, "localhost", sizeof(hostname));
            }

            hostname[sizeof(hostname) - 1] = '\0';
            field = hostname;
        } else {
            mdebug1("Invalid parameter '%s' for log format.", param);
            continue;
        }

        if (field) {
            z = strlen(field);

            if (n + z >= OS_MAXSTR) {
                goto fail;
            }

            strncpy(final + n, field, OS_MAXSTR - n);
            n += z;
        }
    }

    // Copy rest of the pattern

    z = strlen(cur);

    if (n + z >= OS_MAXSTR) {
        goto fail;
    }

    strncpy(final + n, cur, OS_MAXSTR - n);
    final[n + z] = '\0';

    free(_pattern);
    return final;

fail:
    mdebug1("Too long message format");
    strncpy(final, logmsg, OS_MAXSTR - 1);
    final[OS_MAXSTR - 1] = '\0';
    free(_pattern);
    return final;
}
