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

//------------------------------------------
// Internal useful util functions.
// you may use them in your own sendfile_impl

// get the beginning char* of filename
static char* strfilename(char* fullpath)
{
	char* end = fullpath + strlen(fullpath);
	while (--end != fullpath && end[-1] != PATH_SEPARATOR);
	return end;
}

//------------------------------------------
// all implements of sendfile_impl

int sendfile_impl_base64(char*);

typedef int sendfile_impl_t(char* filename); // send a file. return 0 if success.
static sendfile_impl_t *sendfile_impl = sendfile_impl_base64;

//------------------------------------------

int sendfile_impl_base64(char* fn)
{
	char out[4], buf[3];
	char *basename, *sendingCommand;
	FILE* fp = fopen(fn, "rb");
	subthd_back_read_handle back_reader;
	long fsize, readBytes, lineLen=0, packLines = 0, sentBytes = 0;
#define MAX_LINE_LENGTH 64
#define LINES_PER_PACK 3
	float _div_fsize_percent;

	if (!fp) return 1; // cannot open the file
	back_reader = subthd_back_read_open();

	basename = strfilename(fn);
	sendingCommand = dupcat("\nstty -echo;printf '::\\r';base64 -d >", basename, " ;stty echo;\n", NULL);
	subthd_back_write(sendingCommand, strlen(sendingCommand));
	sfree(sendingCommand);

	// waiting for leading chars (::\r)

	int colonCounter = 0, selectSuccess = 0, gotLeading = 0;
	while (!gotLeading)
	{
		selectSuccess = subthd_back_read_select(back_reader, 1000);
		if (!selectSuccess) break;
		subthd_back_read_read(back_reader, buf, 1);
		switch (buf[0])
		{
		case ':':
			colonCounter++;
			break;

		case '\r':
			if (colonCounter >= 2) gotLeading = 1;

		default:
			colonCounter = 0;
			break;
		}
	}
	if (!gotLeading) { // timeout while waiting for leading...
		fclose(fp);
		subthd_back_read_close(back_reader);
		return 2;
	}

	// start sending data

	fseek(fp, 0, SEEK_END); fsize = ftell(fp);
	fseek(fp, 0, SEEK_SET); _div_fsize_percent = 100.0f / fsize;

	clock_t 
		sendSince = clock(),
		nextReportTime = 0,
		reportDuration = 1 * CLOCKS_PER_SEC;

	while (1) {
		readBytes = fread(buf, 1, 3, fp);
		base64_encode_atom(buf, readBytes, out);
		subthd_back_write(out, 4);
		
		sentBytes += readBytes;
		lineLen += 4;

		if (lineLen >= MAX_LINE_LENGTH) {
			lineLen = 0;
			subthd_back_write("\n", 1);

			if (++packLines >= LINES_PER_PACK) {
				subthd_back_flush();
				packLines = 0;
			}
		}

		if (clock() >= nextReportTime) {
			nextReportTime = clock() + reportDuration;
			subthd_ldisc_printf("\rSent %d%%, avg %2.2f kB/s    \r",
				(int)(sentBytes*_div_fsize_percent),
				(float)sentBytes / (clock() - sendSince) * CLOCKS_PER_SEC / 1000
			);
		}

		if (sentBytes >= fsize) {
			break;
		}
	}

	// end of sending

	subthd_back_write("\n\x4", 2);
	subthd_back_flush();

	subthd_ldisc_printf("\rSent, speed: %2.2f kB/s. Decoding... \r\n",
		(float)sentBytes / (clock() - sendSince) * CLOCKS_PER_SEC / 1000
	);

	// done!

	fclose(fp);
	subthd_back_read_close(back_reader);
	return 0;
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
