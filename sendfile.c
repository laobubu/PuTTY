/*
 * sendfile.c: drag-n-drop to send files
 */

#include <stdio.h>

#include "putty.h"
#include "subthd.h"

#if defined(WIN32) || defined(_WIN32) 
#define PATH_SEPARATOR '\\'
#else 
#define PATH_SEPARATOR '/' 
#endif 

int sendfile_impl_base64(char*);

typedef int sendfile_impl_t(char* filename); // send a file
static sendfile_impl_t *sendfile_impl = sendfile_impl_base64;


int sendfile_impl_base64(char* fn)
{
	char out[4], buf[3];
	FILE* fp = fopen(fn, "rb");
	long fsize, readBytes, lineLen=0, sentBytes = 0;
#define MAX_LINE_LENGTH 64
	float _div_fsize_percent;

	fseek(fp, 0, SEEK_END); fsize = ftell(fp);
	fseek(fp, 0, SEEK_SET); _div_fsize_percent = 100.0f / fsize;

	while (1) {
		readBytes = fread(buf, 1, 3, fp);
		base64_encode_atom(buf, readBytes, out);
		subthd_back_write(out, 4);
		
		sentBytes += readBytes;
		lineLen += 4;

		if (lineLen >= MAX_LINE_LENGTH) {
			lineLen = 0;
			subthd_back_write("\n", 1);
			subthd_knock();
		}

		if (sentBytes >= fsize) {
			break;
		}
	}
}


// this is the subthread function that send files.
// filelist is in GTK style. e.g.
//
//    file:///home/user/1.txt
//    file:///home/user/another.txt
//    (empty line as the end)
//
static int sendFileThread(struct subthd_tag *self, void* filelist1)
{
	char 
		*fns = (char*)filelist1, // the end char '\0' or '\n'
		*fnprev = fns; // the beginning of current filename
	int fnlen;
	char filename[260];

	while (1) {
		if ((fns = strchr(fnprev, '\n')) == NULL) fns = strchr(fnprev, '\0');
		else if (fns == fnprev) break; // "\n\n" as the end

		if (memcmp(fnprev, "file://", 7) == 0) fnprev += 7; // skip prefix
		fnlen = fns - fnprev;

		memcpy(filename, fnprev, fnlen);
		filename[fnlen] = '\0';

		(*sendfile_impl)(filename);

		if (*fns == '\0') break;
		fnprev = ++fns;
	}
}

void sendfile_start(char* list)
{
	subthd_create(sendFileThread, dupstr(list));
}
