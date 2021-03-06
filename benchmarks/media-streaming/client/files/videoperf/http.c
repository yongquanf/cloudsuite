/*
   httperf -- a tool for measuring web server performance
   Copyright (C) 2000  Hewlett-Packard Company
   Contributed by David Mosberger-Tang <davidm@hpl.hp.com>

   This file is part of httperf, a web server performance measurment
   tool.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
   02111-1307 USA
 */

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

#include <httperf.h>
#include <http.h>
#include <call.h>
#include <event.h>
#include <conn.h>

/* Read a CRLF terminated line of characters into c->reply.line.
   Returns 1 when the line is complete, 0 when the line is incomplete
   and more data is needed.  */
	static int
get_line (Call *c, char **bufp, size_t *buf_lenp)
{
	size_t to_copy, buf_len = *buf_lenp;
	Conn *s = c->conn;
	char *buf = *bufp;
	const char *eol;
	int has_lf;

	if (buf_len <= 0)
		return 0;

	/* Note that core.c guarantees that BUF is '\0' terminated. */
	eol = strchr (buf, '\n');
	if (eol)
		++eol;
	else
		eol = buf + buf_len;

	to_copy = eol - buf;
	buf_len -= to_copy;
	if (s->line.iov_len + to_copy >= sizeof (s->line_buf))
	{
		fprintf (stderr,
				"%s.get_line: truncating header from %lu to %lu bytes\n",
				prog_name, (u_long) (s->line.iov_len + to_copy),
				(u_long) sizeof (s->line_buf));
		to_copy = sizeof (s->line_buf) - 1 - s->line.iov_len;
	}
	memcpy ((char *) s->line.iov_base + s->line.iov_len, buf, to_copy);
	s->line.iov_len += to_copy;

	has_lf = ((char *) s->line.iov_base)[s->line.iov_len - 1] == '\n';

	*bufp = (char *) eol;
	*buf_lenp = buf_len;

	if (has_lf || s->line.iov_len == sizeof (s->line_buf) - 1)
	{
		/* We got a full header line.  Chop off \r\n at the tail if
		   necessary.  */
		if (((char *) s->line.iov_base)[s->line.iov_len - 1] == '\n')
		{
			--s->line.iov_len;
			if (((char *) s->line.iov_base)[s->line.iov_len - 1] == '\r')
				--s->line.iov_len;
		}
		((char *) s->line.iov_base)[s->line.iov_len] = '\0';
		return 1;
	}
	return 0;
}

	static void
parse_status_line (Call *c, char **bufp, size_t *buf_lenp)
{
	char *buf, *buf_start = *bufp;
	u_int major, minor, status;
	Conn *s = c->conn;
	Any_Type arg;

	s->is_chunked = 0;

	/* default to "infinite" content length: */
	s->content_length = ~(size_t) 0;

	if (!get_line (c, bufp, buf_lenp))
		return;

	buf = c->conn->line.iov_base;
	if (sscanf (buf, "HTTP/%u.%u %u ", &major, &minor, &status) == 3)
	{
		c->reply.version = 0x10000*major + minor;
		c->reply.status = status;
	}
	else
	{
		c->reply.version = 0x10000;		/* default to 1.0 */
		c->reply.status = 599;
		if (c->reply.status == 599) {
			fprintf (stderr, "%s.parse_status_line: invalid status line `%s'!!\n",
					prog_name, buf);
			fprintf (stdout, "%s.parse_status_line: invalid status line `%s'!!\n",
					prog_name, buf);
		}
	}
	if (DBG > 0)
		fprintf (stderr,
				"parse_status_line.%lu: reply is HTTP/%u.%u, status = %d\n",
				c->id, c->reply.version / 0x10000, c->reply.version & 0xffff,
				c->reply.status);

	/* Determine whether we should be expecting a message body.  HEAD
	   never includes an entity.  For other methods, things depend on
	   the status code.  */

	if (strcmp ((char *) c->req.iov[IE_METHOD].iov_base, "HEAD") == 0)
		s->has_body = 0;
	else
	{
		s->has_body = 1;
		switch (status / 100)
		{
			case 1: /* informational */
				s->has_body = 0;
				if (status == 100)
				{
					arg.l = c->reply.status;
					event_signal (EV_CALL_RECV_START, (Object *) c, arg);
					s->state = S_REPLY_CONTINUE;
					goto done;
				}
				break;

			case 2: /* success */
			case 3: /* redirection */
				switch (status)
				{
					case 204: /* No Content */
					case 205: /* Reset Content */
					case 304: /* Not Modified */
						s->has_body = 0;
						break;
				}
				break;

			case 4: /* client errors */
			case 5: /* server errors */
				break;

			default:
				fprintf (stderr, "%s.parse_status_line: bad status %u\n",
						prog_name, status);
				fprintf (stdout, "%s.parse_status_line: bad status %u\n",
						prog_name, status);
				break;
		}
	}
	arg.l = c->reply.status;
	event_signal (EV_CALL_RECV_START, (Object *) c, arg);
	if (s->state >= S_CLOSING)
		return;
	s->state = S_REPLY_HEADER;

done:
	c->reply.header_bytes += *bufp - buf_start;
	s->line.iov_len = 0;		
}

	static void
parse_headers (Call *c, char **bufp, size_t *buf_lenp)
{
	char *hdr, *buf_start = *bufp;
	Conn *s = c->conn;
	size_t hdr_len;
	Any_Type arg;

	while (get_line (c, bufp, buf_lenp) > 0)
	{
		hdr = s->line.iov_base;
		hdr_len = s->line.iov_len;

		if (!hdr_len)
		{
			/* empty header implies end of headers */
			if (s->has_body)
				if (s->is_chunked)
				{
					s->content_length = 0;
					s->state = S_REPLY_CHUNKED;
				}
				else
					s->state = S_REPLY_DATA;
			else if (s->state == S_REPLY_CONTINUE)
				s->state = S_REPLY_HEADER;
			else
				s->state = S_REPLY_DONE;
			break;
		}

		/* process line as a regular header: */
		switch (tolower (*hdr))
		{
			case 'c':
				if (strncasecmp (hdr, "content-length:", 15) == 0)
				{
					hdr += 15;
					s->content_length = strtoul (hdr, 0, 10);
					if (DBG > 0) {
						fprintf(stderr, "parse_headers: content-length = %zd\n", s->content_length);
					}
				}
				break;

			case 't':
				if (strncasecmp (hdr, "transfer-encoding:", 18) == 0)
				{
					hdr += 18;
					while (isspace (*hdr))
						++hdr;
					if (strcasecmp (hdr, "chunked") == 0)
						s->is_chunked = 1;
					else
						fprintf (stderr, "%s.parse_headers: unknown transfer "
								"encoding `%s'\n", prog_name, hdr);
				}
				break;
		}
		arg.vp = &s->line;
		event_signal (EV_CALL_RECV_HDR, (Object *) c, arg);
		if (s->state >= S_CLOSING)
			return;
		s->line.iov_len = 0;		
	}
	c->reply.header_bytes += *bufp - buf_start;
}

	static void
parse_footers (Call *c, char **bufp, size_t *buf_lenp)
{
	char *buf_start = *bufp;
	Conn *s = c->conn;
	size_t hdr_len;
	Any_Type arg;

	if (DBG > 0) {
		fprintf(stderr, "parse_footers: c = %p, bufp = %p buf_len = %d\n",
				c, *bufp, (int) *buf_lenp);
	}

	while (get_line (c, bufp, buf_lenp) > 0)
	{
		hdr_len = s->line.iov_len;

		if (!hdr_len)
		{
			/* empty footer implies end of footers */
			s->state = S_REPLY_DONE;
			break;
		}
		/* process line as a regular footer: */
		arg.vp = &s->line;
		event_signal (EV_CALL_RECV_FOOTER, (Object *) c, arg);
		if (s->state >= S_CLOSING)
			return;
		s->line.iov_len = 0;		
	}
	c->reply.footer_bytes += *bufp - buf_start;
}

	static int
parse_data (Call *c, char **bufp, size_t *buf_lenp)
{
	size_t bytes_needed, buf_len = *buf_lenp;
	Conn *s = c->conn;
	char *buf = *bufp;
	struct iovec iov;
	Any_Type arg;

	bytes_needed = (s->content_length - c->reply.content_bytes);

	if (buf_len > bytes_needed)
		buf_len = bytes_needed;

	if (param.verify_reply) {
		memcpy(c->reply.content + c->reply.content_bytes, buf, buf_len );
	}

	iov.iov_base = (caddr_t) buf;
	iov.iov_len = buf_len;
	arg.vp = &iov;
	event_signal (EV_CALL_RECV_DATA, (Object *) c, arg);

	c->reply.content_bytes += buf_len;
	*bufp = buf + buf_len;
	*buf_lenp -= buf_len;

	return (buf_len == bytes_needed);
}

	static void
xfer_chunked  (Call *c, char **bufp, size_t *buf_lenp)
{
	Conn *s = c->conn;
	size_t chunk_length;
	char *end;

	while (*buf_lenp > 0 && s->state < S_CLOSING)
	{
		if (c->reply.content_bytes >= s->content_length)
		{
			/* need to parse next chunk length line: */
			if (!get_line (c, bufp, buf_lenp))
				return;				/* need more data */
			if (s->line.iov_len == 0)
				continue;				/* skip over empty line */

			errno = 0;
			chunk_length = strtoul (s->line.iov_base, &end, 16);
			s->line.iov_len = 0;
			if (errno == ERANGE || end == s->line.iov_base)
			{
				fprintf (stderr, "%s.xfer_chunked: bad chunk line `%s'\n",
						prog_name, (char *) s->line.iov_base);
				continue;
			}

			if (chunk_length == 0)
			{
				/* a final chunk of zero bytes indicates the end of the reply */
				s->state = S_REPLY_FOOTER;
				return;
			}
			s->content_length += chunk_length;
		}
		parse_data (c, bufp, buf_lenp);
	}
}

#define PATHLEN 256

void
verify_reply_data (Call * c ) {
	int fd;                 /* File descriptor */
	struct stat fstat_info; /* For fstat information */
	int size;               /* Size of the file (in bytes) */
	char * file_content;    /* Read the file off disk into this buf */
	int rc;                 /* Syscall return codes */  
	char path[PATHLEN];     /* Path to the file */
	char * uri;             /* Request uri */
	int len;                /* Used to calculate path lengths */


	uri = (char *) c->req.iov[IE_URI].iov_base;
	assert(uri != NULL);

	/* Build path to file */
	len = strlen(param.verify_dir);
	strncpy(path, param.verify_dir, len);
	strncpy(path+len, uri, PATHLEN - len);
	path[PATHLEN-1] = '\0';

	/* Open the reference file */
	fd = open(path, O_RDONLY);
	if ( fd < 0 ) {
		printf("Unable to open reference file for reading\n");
		exit(1);
	}

	/* fstat the reference file */
	rc = fstat(fd, &fstat_info);
	if ( rc < 0 ) {
		printf("Unable to fstat file in reference fileset\n");
		exit(1);
	}

	size = fstat_info.st_size;
	/* Compare the file size to the length of the server's reply */
	if ( c->reply.content_bytes != size ) {
		printf("Error:Server reply contains %lld bytes "
				"but the referenced file contains %lld bytes\n",
				(long long) c->reply.content_bytes, 
				(long long) fstat_info.st_size);
		exit(1);
	}

	/* Allocate a buffer for reading */
	file_content = (char*) malloc( size );
	assert( file_content != NULL );    

	/* Read the file off the disk */
	rc = read( fd, file_content, size );
	if ( rc < 0 ) {
		printf("Error: Unable to read reference file\n");
		exit(1);      
	}

	/* Compare the file data to the server's reply data */
	if ( strncmp( c->reply.content, file_content, size) != 0 ) {
		printf("Error: Server reply does not match reference file!\n");
		exit(1);
	}

	/* Cleanup - should also be done in error cases above */
	close(fd);
	free(file_content);  
}



	void
http_process_reply_bytes (Call *c, char **bufp, size_t *buf_lenp)
{
	Conn *s = c->conn;
	struct iovec iov;
	Any_Type arg;

	iov.iov_base = *bufp;
	iov.iov_len = *buf_lenp;
	arg.vp = &iov;
	event_signal (EV_CALL_RECV_RAW_DATA, (Object *) c, arg);

	do
	{
		switch (s->state)
		{
			case S_REPLY_STATUS:
				parse_status_line (c, bufp, buf_lenp);
				break;

			case S_REPLY_HEADER:
				parse_headers (c, bufp, buf_lenp);
				/* Allocate a buffer to hold the reply. 
				   Later on we'll verify the bytes */
				if ( param.verify_reply && s->content_length > 0
						&& c->reply.content == NULL ) {
					c->reply.content = (char*) malloc(s->content_length);
					assert( c->reply.content != NULL );
				}
				break;

			case S_REPLY_FOOTER:
				parse_footers (c, bufp, buf_lenp);
				break;

			case S_REPLY_DATA:
				if (parse_data (c, bufp, buf_lenp) && s->state < S_CLOSING) {
					if (param.verify_reply ) verify_reply_data(c);
					s->state = S_REPLY_DONE;
				}
				break;

			case S_REPLY_CONTINUE:
				parse_headers (c, bufp, buf_lenp);
				break;

			case S_REPLY_CHUNKED:
				xfer_chunked (c, bufp, buf_lenp);
				break;

			case S_REPLY_DONE:
				return;

			default:
				fprintf (stderr, "%s.http_process_reply_bytes: bad state %d\n",
						prog_name, s->state);
				exit (1);
		}
	}
	while (*buf_lenp > 0 && s->state < S_CLOSING);
}
