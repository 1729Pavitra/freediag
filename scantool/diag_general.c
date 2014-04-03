/*
 *	freediag - Vehicle Diagnostic Utility
 *
 *
 * Copyright (C) 2001 Richard Almeida & Ibex Ltd (rpa@ibex.co.uk)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 *************************************************************************
 *
 *
 * General routines
 *
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "diag.h"
#include "diag_err.h"
#include "diag_dtc.h"
#include "diag_l1.h"
#include "diag_l2.h"

#define ERR_STR_LEN 30	//length of "i
CVSID("$Id$");

static int diag_initialized=0;

int diag_init(void)	//returns 0 if normal exit
{
	int rv;

	if (diag_initialized)
		return 0;

	/* Add the supported protocols and links */
	// (these fill in l0dev_list and l2proto_list)

	if ((rv=diag_l0_config()))
		return diag_iseterr(rv);

	if ((rv=diag_l2_config()))
		return diag_iseterr(rv);

	//XXX This is interesting: the following functions only ever return 0...

	if ((rv = diag_l1_init()))
		return diag_iseterr(rv);
	if ((rv = diag_l2_init()))
		return diag_iseterr(rv);
	if ((rv = diag_os_init()))
		return diag_iseterr(rv);

	diag_dtc_init();
	diag_initialized = 1;

	return 0;
}

//must be called before exiting. Ret 0 if ok
//this is the "opposite" of diag_init
int diag_end(void) {
	int rv=0;
	if (! diag_initialized)
		return 0;

	//we don't empty the l0dev_list and l2proto_list linked lists.
	if (diag_l2_end()) {
		fprintf(stderr, FLFMT "Could not close L2 level\n", FL);
		rv=-1;
	}
	if (diag_l1_end()) {
		fprintf(stderr, FLFMT "Could not close L1 level\n", FL);
		rv=-1;
	}
	if (diag_os_close()) {
		fprintf(stderr, FLFMT "Could not close OS functions!\n", FL);
		rv=-1;
	}
	//nothing to do for diag_dtc_init

	diag_initialized=0;
	return rv;
}


/*
 * Message handling
 * diag_allocmsg : alloc a new diag_msg and its diag_msg->data if datalen>0.
 */
struct diag_msg *
diag_allocmsg(size_t datalen)
{
	struct diag_msg *newmsg;

	if (diag_calloc(&newmsg, 1))
		return (struct diag_msg *)diag_pseterr(DIAG_ERR_NOMEM);

	newmsg->iflags |= DIAG_MSG_IFLAG_MALLOC;

	if (datalen)
	{
		if (diag_calloc(&newmsg->data, datalen))
		{
			free(newmsg);
			return (struct diag_msg *)diag_pseterr(DIAG_ERR_NOMEM);
		}
	}
	else
		newmsg->data = NULL;

	newmsg->next=NULL;
	newmsg->idata = newmsg->data;	/* Keep tab as users change newmsg->data */
	// i.e. some functions do (diagmsg->data += skiplen) which would prevent us
	// from doing free(diagmsg->data)  (the pointer was changed).

	return newmsg;
}

/* Duplicate a message, and its contents */
struct diag_msg *
diag_dupmsg(struct diag_msg *msg)
{
	struct diag_msg	*newmsg = NULL, *tmsg, *cmsg;

	/*
	 * Dup first msg
	 * -- we don't copy "iflags" as that is about how this message
	 * was created, and not about the message we are duplicating
	 */

	newmsg = diag_allocmsg(msg->len);
	if (!newmsg)
		return 0;

	newmsg->fmt = msg->fmt;
	newmsg->type = msg->type;
	newmsg->dest = msg->dest;
	newmsg->src = msg->src;
	newmsg->len = msg->len;
	newmsg->rxtime = msg->rxtime;

	/* Dup data */
	memcpy(newmsg->data, msg->data, msg->len);

	/* And any on chain */
	cmsg = newmsg;
	msg = msg->next;
	while (msg)
	{
		tmsg = diag_allocmsg(msg->len);
		if (tmsg == NULL)
			return NULL;	/* Out of memory */

		tmsg->fmt = msg->fmt;
		tmsg->type = msg->type;
		tmsg->dest = msg->dest;
		tmsg->src = msg->src;
		tmsg->len = msg->len;
		tmsg->rxtime = msg->rxtime;
		tmsg->next = NULL;	/* Except next message pointer */
		/* Dup data */
		memcpy(tmsg->data, msg->data, msg->len);

		/* Attach tmsg, and update cmsg */
		cmsg->next = tmsg;
		cmsg = cmsg->next;

		/* And process next in list */
		msg = msg->next;
	}

	return newmsg;
}

/* Duplicate a single message, don't follow the chain */
struct diag_msg *
diag_dupsinglemsg(struct diag_msg *msg)
{
	struct diag_msg	*newmsg = NULL;

	/* Dup first msg */

	newmsg = diag_allocmsg(msg->len);
	if (!newmsg)
		return 0;

	newmsg->fmt = msg->fmt;
	newmsg->type = msg->type;
	newmsg->dest = msg->dest;
	newmsg->src = msg->src;
	newmsg->len = msg->len;
	newmsg->rxtime = msg->rxtime;
	newmsg->iflags = msg->iflags;
	/* Dup data */
	memcpy(newmsg->data, msg->data, msg->len);

	return newmsg;
}

/* Free a msg that we dup'd */
void
diag_freemsg(struct diag_msg *msg)
{
	struct diag_msg *nextmsg;

	if ( (msg->iflags & DIAG_MSG_IFLAG_MALLOC) == 0 )
	{
		fprintf(stderr,
			FLFMT "diag_freemsg called for non diag_allocmsg()'d message %p\n",
			FL, (void *)msg);
		return;
	}

	while (msg)
	{
		nextmsg = msg->next;

		if (msg->idata)
			free(msg->idata);
		free(msg);
		msg = nextmsg;
	}
	return;
}


// diag_cks1: return simple 8-bit checksum of
// [len] bytes at *data. Everybody needs this !
uint8_t diag_cks1(uint8_t * data, unsigned int len) {
	uint8_t rv=0;

	while (len > 0) {
		len--;
		rv += data[len];
	}
	return rv;
}

//diag_data_dump : print (len) bytes of uint8_t *data
//to the specified FILE (stderr, etc.)
void
diag_data_dump(FILE *out, const void *data, size_t len)
{
	const uint8_t *p = (const uint8_t *)data;
	size_t i;
	for (i=0; i<len; i++)
		fprintf(out, "0x%02X ", p[i]);
}

//smartcat() : make sure s1 is not too large, then strncat
//it does NOT verify if *p1 is large enough !
void smartcat(char *p1, const size_t s1, const char *p2 )
{
	assert ( s1 > strlen(p1) + strlen (p2) + 1 ) ;
	strncat(p1, p2, s1);
}

/*
 * Error code latching.
 * "diag_seterr" returns NULL so you can call it from a function
 * that returns a NULL pointer on error.
 */
static int latchedCode;

static const struct {
	const int code;
	const char *desc;
} edesc[] = {
	{ DIAG_ERR_GENERAL, "Unspecified Error" },
	{ DIAG_ERR_BADFD, "Invalid FileDescriptor passed to routine" },
	{ DIAG_ERR_NOMEM, "Malloc/Calloc/Strdup/etc failed - ran out of memory " },

	{ DIAG_ERR_INIT_NOTSUPP, "Initbus type not supported by H/W" },
	{ DIAG_ERR_PROTO_NOTSUPP, "Initbus type not supported by H/W" },
	{ DIAG_ERR_IOCTL_NOTSUPP, "Ioctl type not supported" },
	{ DIAG_ERR_BADIFADAPTER, "L0 adapter comms failed" },

	{ DIAG_ERR_TIMEOUT, "Read/Write timeout" },

	{ DIAG_ERR_BUSERROR, "We detected write error on diag bus" },
	{ DIAG_ERR_BADLEN, "Bad length for this i/f" },
	{ DIAG_ERR_BADDATA, "Cant decode msg (ever)" },
	{ DIAG_ERR_BADCSUM, "Bad checksum in recvd message" },
	{ DIAG_ERR_INCDATA, "Incomplete data, need to receive more" },
	{ DIAG_ERR_WRONGKB, "Wrong KeyBytes received" },
	{ DIAG_ERR_BADRATE, "Bit rate specified doesn't match ECU" },

	{ DIAG_ERR_ECUSAIDNO, "Ecu returned negative" },
	{ DIAG_ERR_RCFILE, "Trouble loading .rc or .ini file" },
	{ DIAG_ERR_CMDFILE, "Trouble with sourcing commands" },
};

const char *
diag_errlookup(const int code) {
	unsigned i;
	static char ill_str[ERR_STR_LEN];
	for (i = 0; i < ARRAY_SIZE(edesc); i++)
		if (edesc[i].code == code)
			return edesc[i].desc;

	snprintf(ill_str,ERR_STR_LEN,"Illegal error code: 0x%.2X\n",code);
	return ill_str;
}

//do not call diag_pflseterr; refer to diag.h for related macros
void *
diag_pflseterr(const char *name, const int line, const int code) {
	fprintf(stderr, "%s:%d: %s.\n", name, line, diag_errlookup(code));
	if (latchedCode == 0)
		latchedCode = code;

	return NULL;
}

int
diag_iflseterr(const char *name, const int line, const int code) {
	fprintf(stderr, "%s:%d: %s.\n", name, line, diag_errlookup(code));
	if (latchedCode == 0)
		latchedCode = code;

	return code;
}

/*
 * Return and clear the error.
 */
int
diag_geterr(void) {
	int oldCode = latchedCode;
	latchedCode = 0;
	return oldCode;
}

/* Memory allocation */

//diag_flcalloc (srcfilename, srcfileline, ptr, num,size) = allocate (num*size) bytes
// also checks for size !=0
int diag_flcalloc(const char *name, const int line,
void **pp, size_t n, size_t s) {
	void *p;

	//sanity check: make sure we weren't given a null ptr
	//or a null size.

	if ((s !=0) && (pp != NULL)) {
		p = calloc(n, s);
	} else {
		p=NULL;
	}

	*pp = p;

	if (p == NULL) {
		fprintf(stderr,
			"%s:%d: calloc(%ld, %ld) failed: %s\n", name, line,
			(long)n, (long)s, strerror(errno));
		return diag_iseterr(DIAG_ERR_NOMEM);
	}

	return 0;
}


//flmalloc = malloc with logging (filename+line) and size check (!=0)
int diag_flmalloc(const char *name, const int line, void **pp, size_t s) {
	void *p;

	if ((s !=0) && (pp != NULL)) {
		p = malloc(s);
	} else {
		p=NULL;
	}
	*pp = p;
	if (p == NULL) {
		fprintf(stderr,
			"%s:%d: malloc(%ld) failed: %s\n", name, line,
			(long)s, strerror(errno));
		return diag_iseterr(DIAG_ERR_NOMEM);
	}

	return 0;
}

