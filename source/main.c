/*
    This file is part of OpenPS3FTP.

    OpenPS3FTP is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    OpenPS3FTP is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with OpenPS3FTP.  If not, see <http://www.gnu.org/licenses/>.
*/
#include <assert.h>

#include <psl1ght/lv2/timer.h>

#include <sysutil/video.h>
#include <sysutil/events.h>

#include <rsx/gcm.h>
#include <rsx/reality.h>

#include <net/netctl.h>

#include <sys/thread.h>

#include <io/pad.h>

#include "common.h"
#include "sconsole.h"

#define LISTEN_PORT		21			// the port that OpenPS3FTP will listen for connections on
#define DEFAULT_USER	"user"		// default username
#define DEFAULT_PASS	"ps3"	// default password

const char* TITLE	= "OpenFTP";
const char* VERSION	= "2.0";
const char* AUTHOR	= "@jjolano and @Axtux";
const char* PASSWD_FILE	= "/dev_hdd0/game/OFTP00001/USRDIR/passwd";

char userpass[64];
char ipport[128];

int exitapp = 0;
int xmbopen = 0;
int anonymous = 0;
int listall = 0;

typedef struct {
	int height;
	int width;
	uint32_t *ptr;
	// Internal stuff
	uint32_t offset;
} buffer;

gcmContextData *context;
VideoResolution res;
int currentBuffer = 0;
buffer *buffers[2];

void waitFlip()
{
	// Block the PPU thread until the previous flip operation has finished.
	while (gcmGetFlipStatus() != 0)
		usleep(200);
	gcmResetFlipStatus();
}

void flip(s32 buffer)
{
	assert(gcmSetFlip(context, buffer) == 0);
	realityFlushBuffer(context);
	gcmSetWaitFlip(context);
}

void makeBuffer(int id, int size)
{
	buffer *buf = malloc(sizeof(buffer));
	buf->ptr = rsxMemAlign(16, size);
	assert(buf->ptr != NULL);

	assert(realityAddressToOffset(buf->ptr, &buf->offset) == 0);
	assert(gcmSetDisplayBuffer(id, buf->offset, res.width * 4, res.width, res.height) == 0);

	buf->width = res.width;
	buf->height = res.height;
	buffers[id] = buf;
}

void init_screen()
{
	void *host_addr = memalign(1024*1024, 1024*1024);
	assert(host_addr != NULL);

	context = realityInit(0x10000, 1024*1024, host_addr); 
	assert(context != NULL);

	VideoState state;
	assert(videoGetState(0, 0, &state) == 0);
	assert(state.state == 0);

	assert(videoGetResolution(state.displayMode.resolution, &res) == 0);

	VideoConfiguration vconfig;
	memset(&vconfig, 0, sizeof(VideoConfiguration));
	vconfig.resolution = state.displayMode.resolution;
	vconfig.format = VIDEO_BUFFER_FORMAT_XRGB;
	vconfig.pitch = res.width * 4;

	assert(videoConfigure(0, &vconfig, NULL, 0) == 0);
	assert(videoGetState(0, 0, &state) == 0); 

	s32 buffer_size = 4 * res.width * res.height;

	gcmSetFlipMode(GCM_FLIP_VSYNC);
	makeBuffer(0, buffer_size);
	makeBuffer(1, buffer_size);

	gcmResetFlipStatus();
	flip(1);
}

void sysevent_callback(u64 status, u64 param, void * userdata)
{
	switch(status)
	{
		case EVENT_REQUEST_EXITAPP:	exitapp = 1;	break;
		case EVENT_MENU_OPEN:		xmbopen = 1;	break;
		case EVENT_MENU_CLOSE:		xmbopen = 0;	break;
	}
}

void opf_clienthandler(u64 arg)
{
	int conn_s = (int)arg;
	int data_s = -1;
	
	int connactive = 1;
	int dataactive = 0;
	int loggedin = 0;
	long long rest = 0;
	
	char user[32];
	char rnfr[256];
	char cwd[256] = "/\0";
	char path[256];
	
	char buffer[384];
	size_t bytes;
	
	netSocketInfo p;
	netGetSockInfo(FD(conn_s), &p, 1);
	
	srand(conn_s);
	int p1 = (rand() % 251) + 4;
	int p2 = rand() % 256;
	
	char pasv_output[64];
	sprintf(pasv_output, "227 Entering Passive Mode (%i,%i,%i,%i,%i,%i)\r\n", NIPQUAD(p.local_adr.s_addr), p1, p2);
	
	sprintf(buffer, "220 %s %s by %s\r\n", TITLE, VERSION, AUTHOR);
	ssend(conn_s, buffer);
	
	while(exitapp == 0 && connactive == 1 && (bytes = recv(conn_s, buffer, 383, 0)) > 0)
	{
		buffer[bytes - 1] = '\0';
		buffer[bytes - 2] = '\0';
		
		char cmd[16], param[256];
		int split = ssplit(buffer, cmd, 15, param, 255);
		
		if(strcasecmp(cmd, "QUIT") == 0)
		{
			connactive = 0;
			ssend(conn_s, "221 Bye!\r\n");
			break;
		}
		else
		if(strcasecmp(cmd, "FEAT") == 0)
		{
			const char *feat[] =
			{
				"REST STREAM", "PASV", "PORT", "MDTM", "MLSD", "SIZE", "SITE CHMOD",
				"MLST type*;size*;modify*;UNIX.mode*;UNIX.uid*;UNIX.gid*;"
			};
			
			const int feat_count = sizeof(feat) / sizeof(char *);
			
			ssend(conn_s, "211 Features:\r\n");
			
			for(int i = 0; i < feat_count; i++)
			{
				sprintf(buffer, " %s\r\n", feat[i]);
				ssend(conn_s, buffer);
			}
			
			ssend(conn_s, "211 End\r\n");
		}
		else
		if(strcasecmp(cmd, "SYST") == 0)
		{
			ssend(conn_s, "215 UNIX Type: L8\r\n");
		}
		else
		if(strcasecmp(cmd, "NOOP") == 0)
		{
			ssend(conn_s, "200 NOOP command successful\r\n");
		}
		else
		if(loggedin == 1)
		{
			if(strcasecmp(cmd, "CWD") == 0)
			{
				abspath(param, cwd, path);
				
				if(is_dir(path))
				{
					strcpy(cwd, path);
					ssend(conn_s, "250 Directory change successful\r\n");
				}
				else
				{
					ssend(conn_s, "550 Cannot access directory\r\n");
				}
			}
			else
			if(strcasecmp(cmd, "PWD") == 0)
			{
				sprintf(buffer, "257 \"%s\" is the current directory\r\n", cwd);
				ssend(conn_s, buffer);
			}
			else
			if(strcasecmp(cmd, "MKD") == 0)
			{
				if(split == 1)
				{
					abspath(param, cwd, path);
					
					if(sysFsMkdir(path, 0755) == 0)
					{
						sprintf(buffer, "257 \"%s\" was successfully created\r\n", path);
						ssend(conn_s, buffer);
					}
					else
					{
						ssend(conn_s, "550 Cannot create directory\r\n");
					}
				}
			}
			else
			if(strcasecmp(cmd, "RMD") == 0)
			{
				if(split == 1)
				{
					abspath(param, cwd, path);
					
					if(sysFsRmdir(path) == 0)
					{
						ssend(conn_s, "250 Directory successfully removed\r\n");
					}
					else
					{
						ssend(conn_s, "550 Cannot remove directory\r\n");
					}
				}
				else
				{
					ssend(conn_s, "501 Insufficient parameters\r\n");
				}
			}
			else
			if(strcasecmp(cmd, "CDUP") == 0)
			{
				int c;
				int len = strlen(cwd) - 1;
				
				for(int i = len; i > 0; i--)
				{
					c = cwd[i];
					cwd[i] = '\0';
					
					if(c == '/' && i < len)
					{
						break;
					}
				}
				
				ssend(conn_s, "250 Directory change successful\r\n");
			}
			else
			if(strcasecmp(cmd, "PASV") == 0)
			{
				sclose(&data_s);
				rest = 0;
				
				int list_s = slisten(getPort(p1, p2), 1);
				
				if(list_s > 0)
				{
					ssend(conn_s, pasv_output);
					
					if((data_s = accept(list_s, NULL, NULL)) > 0)
					{
						dataactive = 1;
					}
					else
					{
						ssend(conn_s, "451 Data connection failed\r\n");
					}
					
					sclose(&list_s);
				}
				else
				{
					ssend(conn_s, "451 Cannot create data socket\r\n");
				}
			}
			else
			if(strcasecmp(cmd, "PORT") == 0)
			{
				if(split == 1)
				{
					sclose(&data_s);
					rest = 0;
					
					char data[6][4];
					char *st = strtok(param, ",");
					
					int i = 0;
					while(st != NULL && i < 6)
					{
						strcpy(data[i++], st);
						st = strtok(NULL, ",");
					}
					
					if(i >= 6)
					{
						char ipaddr[16];
						sprintf(ipaddr, "%s.%s.%s.%s", data[0], data[1], data[2], data[3]);
						
						if(sconnect(ipaddr, getPort(atoi(data[4]), atoi(data[5])), &data_s) == 0)
						{
							ssend(conn_s, "200 PORT command successful\r\n");
							dataactive = 1;
						}
						else
						{
							ssend(conn_s, "451 Data connection failed\r\n");
						}
					}
					else
					{
						ssend(conn_s, "501 Insufficient connection info\r\n");
					}
				}
				else
				{
					ssend(conn_s, "501 Insufficient parameters\r\n");
				}
			}
			else
			if(strcasecmp(cmd, "ABOR") == 0)
			{
				ssend(conn_s, "226 ABOR command successful\r\n");
			}
			else
			if(strcasecmp(cmd, "LIST") == 0)
			{
				if(data_s > 0)
				{
					Lv2FsFile fd;
					if(sysFsOpendir(cwd, &fd) == 0)
					{
						Lv2FsDirent entry;
						u64 read;
						
						ssend(conn_s, "150 Accepted data connection\r\n");
						
						while(sysFsReaddir(fd, &entry, &read) == 0 && read > 0)
						{
							abspath(entry.d_name, cwd, path);
							
							if((strcmp(path, "/app_home") == 0 || strcmp(path, "/host_root") == 0) && listall == 0)
							{
								continue;
							}
							
							Lv2FsStat buf;
							sysFsStat(path, &buf);
							
							char tstr[16];
							strftime(tstr, 15, "%b %d %H:%M", localtime(&buf.st_mtime));
							
							sprintf(buffer, "%s%s%s%s%s%s%s%s%s%s   1 root nobody     %llu %s %s\r\n",
								((buf.st_mode & S_IFDIR) != 0) ? "d" : "-", 
								((buf.st_mode & S_IRUSR) != 0) ? "r" : "-",
								((buf.st_mode & S_IWUSR) != 0) ? "w" : "-",
								((buf.st_mode & S_IXUSR) != 0) ? "x" : "-",
								((buf.st_mode & S_IRGRP) != 0) ? "r" : "-",
								((buf.st_mode & S_IWGRP) != 0) ? "w" : "-",
								((buf.st_mode & S_IXGRP) != 0) ? "x" : "-",
								((buf.st_mode & S_IROTH) != 0) ? "r" : "-",
								((buf.st_mode & S_IWOTH) != 0) ? "w" : "-",
								((buf.st_mode & S_IXOTH) != 0) ? "x" : "-",
								(unsigned long long)buf.st_size, tstr, entry.d_name);
							
							ssend(data_s, buffer);
						}
						
						ssend(conn_s, "226 Transfer complete\r\n");
					}
					else
					{
						ssend(conn_s, "550 Cannot access directory\r\n");
					}
					
					sysFsClosedir(fd);
				}
				else
				{
					ssend(conn_s, "425 No data connection\r\n");
				}
			}
			else
			if(strcasecmp(cmd, "MLSD") == 0)
			{
				if(data_s > 0)
				{
					Lv2FsFile fd;
					if(sysFsOpendir(cwd, &fd) == 0)
					{
						Lv2FsDirent entry;
						u64 read;
						
						ssend(conn_s, "150 Accepted data connection\r\n");
						
						while(sysFsReaddir(fd, &entry, &read) == 0 && read > 0)
						{
							abspath(entry.d_name, cwd, path);
							
							if((strcmp(path, "/app_home") == 0 || strcmp(path, "/host_root") == 0) && listall == 0)
							{
								continue;
							}
							
							Lv2FsStat buf;
							sysFsStat(path, &buf);
							
							char tstr[16];
							strftime(tstr, 15, "%Y%m%d%H%M%S", localtime(&buf.st_mtime));
							
							char dirtype[2];
							if(strcmp(entry.d_name, ".") == 0)
							{
								dirtype[0] = 'c';
							}
							else
							if(strcmp(entry.d_name, "..") == 0)
							{
								dirtype[0] = 'p';
							}
							else
							{
								dirtype[0] = '\0';
							}
							
							dirtype[1] = '\0';
							
							sprintf(buffer, "type=%s%s;siz%s=%llu;modify=%s;UNIX.mode=0%i%i%i;UNIX.uid=root;UNIX.gid=nobody; %s\r\n",
								dirtype,
								fis_dir(buf) ? "dir" : "file",
								fis_dir(buf) ? "d" : "e", (unsigned long long)buf.st_size, tstr,
								(((buf.st_mode & S_IRUSR) != 0) * 4 + ((buf.st_mode & S_IWUSR) != 0) * 2 + ((buf.st_mode & S_IXUSR) != 0) * 1),
								(((buf.st_mode & S_IRGRP) != 0) * 4 + ((buf.st_mode & S_IWGRP) != 0) * 2 + ((buf.st_mode & S_IXGRP) != 0) * 1),
								(((buf.st_mode & S_IROTH) != 0) * 4 + ((buf.st_mode & S_IWOTH) != 0) * 2 + ((buf.st_mode & S_IXOTH) != 0) * 1),
								entry.d_name);
							
							ssend(data_s, buffer);
						}
						
						ssend(conn_s, "226 Transfer complete\r\n");
					}
					else
					{
						ssend(conn_s, "550 Cannot access directory\r\n");
					}
					
					sysFsClosedir(fd);
				}
				else
				{
					ssend(conn_s, "425 No data connection\r\n");
				}
			}
			else
			if(strcasecmp(cmd, "MLST") == 0)
			{
				Lv2FsFile fd;
				if(sysFsOpendir(cwd, &fd) == 0)
				{
					Lv2FsDirent entry;
					u64 read;
					
					ssend(conn_s, "250 Directory Listing:\r\n");
					
					while(sysFsReaddir(fd, &entry, &read) == 0 && read > 0)
					{
						abspath(entry.d_name, cwd, path);
						
						if((strcmp(path, "/app_home") == 0 || strcmp(path, "/host_root") == 0) && listall == 0)
						{
							continue;
						}
						
						Lv2FsStat buf;
						sysFsStat(path, &buf);
						
						char tstr[16];
						strftime(tstr, 15, "%Y%m%d%H%M%S", localtime(&buf.st_mtime));
						
						char dirtype[2];
						if(strcmp(entry.d_name, ".") == 0)
						{
							dirtype[0] = 'c';
						}
						else
						if(strcmp(entry.d_name, "..") == 0)
						{
							dirtype[0] = 'p';
						}
						else
						{
							dirtype[0] = '\0';
						}
						
						dirtype[1] = '\0';
						
						sprintf(buffer, " type=%s%s;siz%s=%llu;modify=%s;UNIX.mode=0%i%i%i;UNIX.uid=root;UNIX.gid=nobody; %s\r\n",
							dirtype,
							fis_dir(buf) ? "dir" : "file",
							fis_dir(buf) ? "d" : "e", (unsigned long long)buf.st_size, tstr,
							(((buf.st_mode & S_IRUSR) != 0) * 4 + ((buf.st_mode & S_IWUSR) != 0) * 2 + ((buf.st_mode & S_IXUSR) != 0) * 1),
							(((buf.st_mode & S_IRGRP) != 0) * 4 + ((buf.st_mode & S_IWGRP) != 0) * 2 + ((buf.st_mode & S_IXGRP) != 0) * 1),
							(((buf.st_mode & S_IROTH) != 0) * 4 + ((buf.st_mode & S_IWOTH) != 0) * 2 + ((buf.st_mode & S_IXOTH) != 0) * 1),
							entry.d_name);
						
						ssend(conn_s, buffer);
					}
					
					ssend(conn_s, "250 End\r\n");
				}
				else
				{
					ssend(conn_s, "550 Cannot access directory\r\n");
				}
				
				sysFsClosedir(fd);
			}
			else
			if(strcasecmp(cmd, "NLST") == 0)
			{
				if(data_s > 0)
				{
					Lv2FsFile fd;
					if(sysFsOpendir(cwd, &fd) == 0)
					{
						Lv2FsDirent entry;
						u64 read;
						
						ssend(conn_s, "150 Accepted data connection\r\n");
						
						while(sysFsReaddir(fd, &entry, &read) == 0 && read > 0)
						{
							sprintf(buffer, "%s\r\n", entry.d_name);
							ssend(data_s, buffer);
						}
						
						ssend(conn_s, "226 Transfer complete\r\n");
					}
					else
					{
						ssend(conn_s, "550 Cannot access directory\r\n");
					}
					
					sysFsClosedir(fd);
				}
				else
				{
					ssend(conn_s, "425 No data connection\r\n");
				}
			}
			else
			if(strcasecmp(cmd, "STOR") == 0)
			{
				if(data_s > 0)
				{
					if(split == 1)
					{
						abspath(param, cwd, path);
						
						ssend(conn_s, "150 Accepted data connection\r\n");
						
						if(recvfile(path, data_s, rest) == 0)
						{
							ssend(conn_s, "226 Transfer complete\r\n");
						}
						else
						{
							ssend(conn_s, "451 Transfer failed\r\n");
						}
					}
					else
					{
						ssend(conn_s, "501 No filename specified\r\n");
					}
				}
				else
				{
					ssend(conn_s, "425 No data connection\r\n");
				}
			}
			else
			if(strcasecmp(cmd, "RETR") == 0)
			{
				if(data_s > 0)
				{
					if(split == 1)
					{
						abspath(param, cwd, path);
						
						if(exists(path) == 0)
						{
							ssend(conn_s, "150 Accepted data connection\r\n");
							
							if(sendfile(path, data_s, rest) == 0)
							{
								ssend(conn_s, "226 Transfer complete\r\n");
							}
							else
							{
								ssend(conn_s, "451 Transfer failed\r\n");
							}
						}
						else
						{
							ssend(conn_s, "550 File does not exist\r\n");
						}
					}
					else
					{
						ssend(conn_s, "501 No filename specified\r\n");
					}
				}
				else
				{
					ssend(conn_s, "425 No data connection\r\n");
				}
			}
			else
			if(strcasecmp(cmd, "TYPE") == 0)
			{
				ssend(conn_s, "200 TYPE command successful\r\n");
				dataactive = 1;
			}
			else
			if(strcasecmp(cmd, "REST") == 0)
			{
				if(split == 1)
				{
					unsigned long long i = atoll(param);
					
					if(i >= 0)
					{
						rest = i;
						dataactive = 1;
						ssend(conn_s, "350 REST command successful\r\n");
					}
					else
					{
						ssend(conn_s, "501 Invalid restart point\r\n");
					}
				}
				else
				{
					ssend(conn_s, "501 No restart point\r\n");
				}
			}
			else
			if(strcasecmp(cmd, "DELE") == 0)
			{
				if(split == 1)
				{
					abspath(param, cwd, path);
					
					if(sysFsUnlink(path) == 0)
					{
						ssend(conn_s, "250 File successfully deleted\r\n");
					}
					else
					{
						ssend(conn_s, "550 Cannot delete file\r\n");
					}
				}
				else
				{
					ssend(conn_s, "501 No filename specified\r\n");
				}
			}
			else
			if(strcasecmp(cmd, "RNFR") == 0)
			{
				if(split == 1)
				{
					abspath(param, cwd, path);
					
					if(exists(path) == 0)
					{
						strcpy(rnfr, path);
						ssend(conn_s, "350 RNFR accepted - ready for destination\r\n");
					}
					else
					{
						ssend(conn_s, "550 RNFR failed - file does not exist\r\n");
					}
				}
				else
				{
					ssend(conn_s, "501 No file specified\r\n");
				}
			}
			else
			if(strcasecmp(cmd, "RNTO") == 0)
			{
				if(split == 1)
				{
					abspath(param, cwd, path);
					
					if(rename(rnfr, path) == 0)
					{
						ssend(conn_s, "250 File was successfully renamed or moved\r\n");
					}
					else
					{
						ssend(conn_s, "550 Cannot rename or move file\r\n");
					}
				}
				else
				{
					ssend(conn_s, "501 No file specified\r\n");
				}
			}
			else
			if(strcasecmp(cmd, "SITE") == 0)
			{
				char param2[256];
				split = ssplit(param, cmd, 15, param2, 255);
				
				if(strcasecmp(cmd, "CHMOD") == 0)
				{
					char param3[4], filename[256];
					split = ssplit(param2, param3, 3, filename, 255);
					
					if(split == 1)
					{
						char perms[5];
						sprintf(perms, "0%s", param3);
						
						abspath(filename, cwd, path);
						
						if(sysFsChmod(path, strtol(perms, NULL, 8)) == 0)
						{
							ssend(conn_s, "250 File permissions successfully set\r\n");
						}
						else
						{
							ssend(conn_s, "550 Cannot set file permissions\r\n");
						}
					}
					else
					{
						ssend(conn_s, "501 No filename specified\r\n");
					}
				}
				else
				if(strcasecmp(cmd, "PASSWD") == 0)
				{
					if(split == 1)
					{
						Lv2FsFile fd;
						if(sysFsOpen(PASSWD_FILE, LV2_O_WRONLY | LV2_O_CREAT | LV2_O_TRUNC, &fd, NULL, 0) == 0)
						{
							u64 written;
							strcpy(userpass, param2);
							sysFsWrite(fd, param2, strlen(param2), &written);
							
							ssend(conn_s, "200 Password successfully set\r\n");
						}
						
						sysFsClose(fd);
						sysFsChmod(PASSWD_FILE, 0660);
					}
					else
					{
						ssend(conn_s, "501 No password given\r\n");
					}
				}
				else
				if(strcasecmp(cmd, "EXITAPP") == 0)
				{
					exitapp = 1;
					ssend(conn_s, "221 Exiting...\r\n");
					break;
				}
				else
				if(strcasecmp(cmd, "HELP") == 0)
				{
					ssend(conn_s, "214 Special commands:\r\n");
					ssend(conn_s, " SITE PASSWD <newpassword> - Change your password\r\n");
					ssend(conn_s, " SITE EXITAPP - Remote quit\r\n");
					ssend(conn_s, " SITE HELP - Show this message\r\n");
					ssend(conn_s, "214 End\r\n");
				}
				else
				{
					ssend(conn_s, "500 Unrecognized SITE command\r\n");
				}
			}
			else
			if(strcasecmp(cmd, "SIZE") == 0)
			{
				if(split == 1)
				{
					abspath(param, cwd, path);
					
					Lv2FsStat buf;
					if(sysFsStat(path, &buf) == 0)
					{
						sprintf(buffer, "213 %llu\r\n", (unsigned long long)buf.st_size);
						ssend(conn_s, buffer);
					}
					else
					{
						ssend(conn_s, "550 File does not exist\r\n");
					}
				}
				else
				{
					ssend(conn_s, "501 No file specified\r\n");
				}
			}
			else
			if(strcasecmp(cmd, "MDTM") == 0)
			{
				if(split == 1)
				{
					abspath(param, cwd, path);
					
					Lv2FsStat buf;
					if(sysFsStat(path, &buf) == 0)
					{
						char tstr[32];
						strftime(tstr, 31, "213 %Y%m%d%H%M%S\r\n", localtime(&buf.st_mtime));
						ssend(conn_s, tstr);
					}
					else
					{
						ssend(conn_s, "550 File does not exist\r\n");
					}
				}
				else
				{
					ssend(conn_s, "501 No file specified\r\n");
				}
			}
			else
			if(strcasecmp(cmd, "USER") == 0 || strcasecmp(cmd, "PASS") == 0)
			{
				ssend(conn_s, "230 You are already logged in\r\n");
			}
			else
			{
				ssend(conn_s, "500 Unrecognized command\r\n");
			}
			
			if(dataactive == 1)
			{
				dataactive = 0;
			}
			else
			{
				sclose(&data_s);
			}
		}
		else
		{
			if(strcasecmp(cmd, "USER") == 0)
			{
				if(split == 1)
				{
					strcpy(user, param);
					sprintf(buffer, "331 User %s OK. Password required\r\n", user);
					ssend(conn_s, buffer);
				}
				else
				{
					ssend(conn_s, "501 Insufficient parameters\r\n");
				}
			}
			else
			if(strcasecmp(cmd, "PASS") == 0)
			{
				if(split == 1)
				{
					if(anonymous || (strcmp(user, DEFAULT_USER) == 0 && strcmp(param, userpass) == 0))
					{
						loggedin = 1;
						ssend(conn_s, "230 Login successful\r\n");
					}
					else
					{
						ssend(conn_s, "430 Invalid username or password\r\n");
					}
				}
				else
				{
					ssend(conn_s, "501 Insufficient parameters\r\n");
				}
			}
			else
			{
				ssend(conn_s, "530 Not logged in\r\n");
			}
		}
	}
	
	sclose(&conn_s);
	sclose(&data_s);
	sys_ppu_thread_exit(0);	
}

void opf_connectionhandler(u64 arg)
{
	union net_ctl_info info;
	
	strcpy(ipport, "No Network Connection");
	while(netCtlGetInfo(NET_CTL_INFO_IP_ADDRESS, &info) < 0 && exitapp == 0);
	
	int list_s = slisten(LISTEN_PORT, 5);
	
	if(list_s > 0)
	{
		u64 conn_s;
		sys_ppu_thread_t id;
		
		sprintf(ipport, "IP: %s\nPort: %i", info.ip_address, LISTEN_PORT);
		
		while(exitapp == 0)
		{
			if((conn_s = (u64)accept(list_s, NULL, NULL)) > 0)
			{
				sys_ppu_thread_yield();
				sys_ppu_thread_create(&id, opf_clienthandler, conn_s, 1500, 0x2000, 0, "FTP Client");
			}
		}
		
		sclose(&list_s);
	}
	
	sys_ppu_thread_exit(0);
}

int main()
{
	netInitialize();
	netCtlInit();
	
	sys_ppu_thread_t id;
	sys_ppu_thread_create(&id, opf_connectionhandler, 0, 1500, 0x400, 0, "FTP Server");
	
	sysRegisterCallback(EVENT_SLOT0, sysevent_callback, NULL);
	
	Lv2FsFile fd; //load password file
	
	if(sysFsOpen(PASSWD_FILE, LV2_O_RDONLY, &fd, NULL, 0) == 0)
	{
		u64 read;
		sysFsRead(fd, userpass, 63, &read);
	}
	else
	{
		strcpy(userpass, DEFAULT_PASS);
	}
	
	sysFsClose(fd);
	
	init_screen();
	sconsoleInit(FONT_COLOR_BLACK, FONT_COLOR_WHITE, res.width, res.height);
	
	char toptext[128], statustext[128];
	sprintf(toptext, "%s %s by %s", TITLE, VERSION, AUTHOR);
	strcpy(statustext, "");
	
	int rwflash = (exists("/dev_blind") == 0 || exists("/dev_rwflash") == 0 || exists("/dev_fflash") == 0 || exists("/dev_Alejandro") == 0);
	
	ioPadInit(7);
	PadInfo padinfo;
	PadData paddata;
	int error, i, x, y;
	
	while(exitapp == 0)
	{
		waitFlip();
		sysCheckCallback();
		
		if(xmbopen != 1)
		{
			ioPadGetInfo(&padinfo);
			
			for(i = 0; i < MAX_PADS; i++)
			{
				if(padinfo.status[i])
				{
					ioPadGetData(i, &paddata);
					
					if(paddata.BTN_CROSS)
					{
						exitapp = 1; //Quit application
					}
					
					if(paddata.BTN_CIRCLE)
					{
						if(rwflash) //Unmount dev_rwflash
						{
							if(exists("/dev_blind") == 0)
								error = Lv2Syscall1(838, (u64)"/dev_blind");
							if(exists("/dev_rwflash") == 0)
								error = Lv2Syscall1(838, (u64)"/dev_rwflash");
							if(exists("/dev_fflash") == 0)
								error = Lv2Syscall1(838, (u64)"/dev_fflash");
							if(exists("/dev_Alejandro") == 0)
								error = Lv2Syscall1(838, (u64)"/dev_Alejandro");
							
							if(error == 0)
							{
								rwflash = 0;
								strcpy(statustext, "Successfully unmounted writable flash.");
							}
							else
							{
								error = 0;
								strcpy(statustext, "Error while unmounting writable flash.");
							}
						}
						else //Mount writable flash
						{
							if(Lv2Syscall8(837, (u64)"CELL_FS_IOS:BUILTIN_FLSH1", (u64)"CELL_FS_FAT", (u64)"/dev_rwflash", 0, 0, 0, 0, 0) == 0)
							{
								rwflash = 1;
								strcpy(statustext, "Successfully mounted writable flash.");
							}
							else
							{
								strcpy(statustext, "Error while mounting writable flash.");
							}
						}
						
						sleep(1);
					}
					
					if(paddata.BTN_SQUARE)
					{
						if(anonymous) //disable anonymous
						{
							anonymous = 0;
							strcpy(statustext, "Successfully disabled anonymous.");
						}
						else //enable anonymous
						{
							anonymous = 1;
							strcpy(statustext, "Successfully enabled anonymous.");
						}
						
						sleep(1);
					}
					
					if(paddata.BTN_TRIANGLE)
					{
						if(listall) //disable listall
						{
							listall = 0;
							strcpy(statustext, "Successfully disabled \"list all\".");
						}
						else //enable listall
						{
							listall = 1;
							strcpy(statustext, "Successfully enabled \"list all\".");
						}
						
						sleep(1);
					}
				}
			}
			
			for(y = 0; y < res.height; y++)
			{
				for(x = 0; x < res.width; x++)
				{
					buffers[currentBuffer]->ptr[y * res.width + x] = FONT_COLOR_BLACK; // background
				}
			}
			
			print(50, 50, toptext, buffers[currentBuffer]->ptr);
			print(100, 100, ipport, buffers[currentBuffer]->ptr);
			print(50, 200, statustext, buffers[currentBuffer]->ptr);
			
			if(rwflash)
				print(50, 250, "Be careful with the writable flash !", buffers[currentBuffer]->ptr);
			
			print(100, 350, "Press CROSS to quit the application.", buffers[currentBuffer]->ptr);
			print(100, 380, rwflash ? "Press CIRCLE to unmount writable flash." : "Press CIRCLE to mount writable flash.", buffers[currentBuffer]->ptr);
			print(100, 410, anonymous ? "Press SQUARE to disable anonymous." : "Press SQUARE to enable anonymous.", buffers[currentBuffer]->ptr);
			print(100, 440, listall ? "Press TRIANGLE to disable \"list all\"." : "Press TRIANGLE to enable \"list all\".", buffers[currentBuffer]->ptr);
			print(50, 510, "Support us : bit.ly/gmzGcI and Axtux.tk", buffers[currentBuffer]->ptr);
		}
		
		flip(currentBuffer);
		currentBuffer = !currentBuffer;
	}
	
	netDeinitialize();
	return 0;
}
