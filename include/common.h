// OpenPS3FTP source common includes
#pragma once

#include <stdio.h>
#include <unistd.h>
#include <malloc.h>
#include <string.h>
#include <fcntl.h>

#include <sys/sysfs.h>
#include <net/net.h>

#include "functions.h"

// macros
#define getPort(p1,p2) ((p1 * 256) + p2)
#define FD(socket) (socket & ~SOCKET_FD_MASK)
#define NIPQUAD(addr) \
         ((unsigned char *)&addr)[0], \
         ((unsigned char *)&addr)[1], \
         ((unsigned char *)&addr)[2], \
         ((unsigned char *)&addr)[3]
#define BUFFER_SIZE		32768		// the buffer size used for file transfers
