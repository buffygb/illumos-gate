/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2007 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#pragma ident	"%Z%%M%	%I%	%E% SMI"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <unistd.h>
#include <string.h>
#include <arpa/nameser.h>
#include <resolv.h>
#include <netdb.h>
#include <rpc/rpc.h>
#include <syslog.h>
#include <gssapi/gssapi.h>
#include <kerberosv5/krb5.h>
#include <net/if.h>

#include <smbns_dyndns.h>
#include <smbns_krb.h>

/* internal use, in dyndns_add_entry */
#define	DEL_NONE		2
/* Maximum retires if not authoritative */
#define	MAX_AUTH_RETRIES 3


static int
dyndns_enabled(void)
{
	int enabled;

	smb_config_rdlock();
	enabled = smb_config_getyorn(SMB_CI_DYNDNS_ENABLE);
	smb_config_unlock();

	return ((enabled) ? 1 : 0);
}

/*
 * XXX The following should be removed once head/arpa/nameser_compat.h
 * defines BADSIG, BADKEY, BADTIME macros
 */
#ifndef	BADSIG
#define	BADSIG ns_r_badsig
#endif /* BADSIG */

#ifndef	BADKEY
#define	BADKEY ns_r_badkey
#endif /* BADKEY */

#ifndef	BADTIME
#define	BADTIME ns_r_badtime
#endif /* BADTIME */

/*
 * dyndns_msg_err
 * Display error message for DNS error code found in the DNS header in
 * reply message.
 * Parameters:
 *   err: DNS errer code
 * Returns:
 *   None
 */
void
dyndns_msg_err(int err)
{
	switch (err) {
	case NOERROR:
		break;
	case FORMERR:
		syslog(LOG_ERR, "DNS message format error\n");
		break;
	case SERVFAIL:
		syslog(LOG_ERR, "DNS server internal error\n");
		break;
	case NXDOMAIN:
		syslog(LOG_ERR, "DNS entry should exist but does not exist\n");
		break;
	case NOTIMP:
		syslog(LOG_ERR, "DNS opcode not supported\n");
		break;
	case REFUSED:
		syslog(LOG_ERR, "DNS operation refused\n");
		break;
	case YXDOMAIN:
		syslog(LOG_ERR, "DNS entry shouldn't exist but does exist\n");
		break;
	case YXRRSET:
		syslog(LOG_ERR, "DNS RRSet shouldn't exist but does exist\n");
		break;
	case NXRRSET:
		syslog(LOG_ERR, "DNS RRSet should exist but does not exist\n");
		break;
	case NOTAUTH:
		syslog(LOG_ERR, "DNS server is not authoritative "
		    "for specified zone\n");
		break;
	case NOTZONE:
		syslog(LOG_ERR, "Name in Prereq or Update section not "
		    "within specified zone\n");
		break;
	case BADSIG:
		syslog(LOG_ERR, "Bad transaction signature (TSIG)");
		break;
	case BADKEY:
		syslog(LOG_ERR, "Bad transaction key (TKEY)");
		break;
	case BADTIME:
		syslog(LOG_ERR, "Time not synchronized");
		break;

	default:
		syslog(LOG_ERR, "Unknown DNS error\n");
	}
}

/*
 * display_stat
 * Display GSS error message from error code.  This routine is used to display
 * the mechanism independent and mechanism specific error messages for GSS
 * routines.  The major status error code is the mechanism independent error
 * code and the minor status error code is the mechanism specific error code.
 * Parameters:
 *   maj: GSS major status
 *   min: GSS minor status
 * Returns:
 *   None
 */
static void
display_stat(OM_uint32 maj, OM_uint32 min)
{
	gss_buffer_desc msg;
	OM_uint32 msg_ctx = 0;
	OM_uint32 min2;
	(void) gss_display_status(&min2, maj, GSS_C_GSS_CODE, GSS_C_NULL_OID,
	    &msg_ctx, &msg);
	syslog(LOG_ERR, "dyndns: GSS major status error: %s\n",
	    (char *)msg.value);
	(void) gss_display_status(&min2, min, GSS_C_MECH_CODE, GSS_C_NULL_OID,
	    &msg_ctx, &msg);
	syslog(LOG_ERR, "dyndns: GSS minor status error: %s\n",
	    (char *)msg.value);
}

static char *
dyndns_put_nshort(char *buf, uint16_t val)
{
	uint16_t nval;

	nval = htons(val);
	(void) memcpy(buf, &nval, sizeof (uint16_t));
	buf += sizeof (uint16_t);
	return (buf);
}

char *
dyndns_get_nshort(char *buf, uint16_t *val)
{
	uint16_t nval;

	(void) memcpy(&nval, buf, sizeof (uint16_t));
	*val = ntohs(nval);
	buf += sizeof (uint16_t);
	return (buf);
}

static char *
dyndns_put_nlong(char *buf, uint32_t val)
{
	uint32_t lval;

	lval = htonl(val);
	(void) memcpy(buf, &lval, sizeof (uint32_t));
	buf += sizeof (uint32_t);
	return (buf);
}

static char *
dyndns_put_byte(char *buf, char val)
{
	*buf = val;
	buf++;
	return (buf);
}

static char *
dyndns_put_int(char *buf, int val)
{
	(void) memcpy(buf, &val, sizeof (int));
	buf += sizeof (int);
	return (buf);
}

char *
dyndns_get_int(char *buf, int *val)
{
	(void) memcpy(val, buf, sizeof (int));
	buf += sizeof (int);
	return (buf);
}


/*
 * dyndns_stuff_str
 * Converts a domain string by removing periods and replacing with a byte value
 * of how many characters following period.  A byte value is placed in front
 * to indicate how many characters before first period.  A NULL character is
 * placed at the end. i.e. host.procom.com -> 4host5procom3com0
 * Buffer space checking is done by caller.
 * Parameters:
 *   ptr : address of pointer to buffer to store converted string
 *   zone: domain name string
 * Returns:
 *   ptr: address of pointer to next available buffer space
 *   -1 : error
 *    0 : success
 */
static int
dyndns_stuff_str(char **ptr, char *zone)
{
	int len;
	char *lenPtr, *zonePtr;

	for (zonePtr = zone; *zonePtr; ) {
		lenPtr = *ptr;
		*ptr = *ptr + 1;
		len = 0;
		while (*zonePtr != '.' && *zonePtr != 0) {
			*ptr = dyndns_put_byte(*ptr, *zonePtr);
			zonePtr++;
			len++;
		}
		*lenPtr = len;
		if (*zonePtr == '.')
			zonePtr++;
	}
	*ptr = dyndns_put_byte(*ptr, 0);
	return (0);
}

/*
 * dyndns_build_header
 * Build the header for DNS query and DNS update request message.
 * Parameters:
 *   ptr               : address of pointer to buffer to store header
 *   buf_len           : buffer length
 *   msg_id            : message id
 *   query_req         : use REQ_QUERY for query message or REQ_UPDATE for
 *                       update message
 *   quest_zone_cnt    : number of question record for query message or
 *                       number of zone record for update message
 *   ans_prereq_cnt    : number of answer record for query message or
 *                       number of prerequisite record for update message
 *   nameser_update_cnt: number of name server for query message or
 *                       number of update record for update message
 *   addit_cnt         : number of additional record
 *   flags             : query flags word
 * Returns:
 *   ptr: address of pointer to next available buffer space
 *   -1 : error
 *    0 : success
 */
int
dyndns_build_header(char **ptr, int buf_len, uint16_t msg_id, int query_req,
    uint16_t quest_zone_cnt, uint16_t ans_prereq_cnt,
    uint16_t nameser_update_cnt, uint16_t addit_cnt, int flags)
{
	uint16_t opcode;

	if (buf_len < 12) {
		syslog(LOG_ERR, "dyndns: no more buf for header section\n");
		return (-1);
	}

	*ptr = dyndns_put_nshort(*ptr, msg_id);	/* mesg ID */
	if (query_req == REQ_QUERY)
		opcode = ns_o_query;	/* query msg */
	else
		opcode = ns_o_update << 11;	/* update msg */
	opcode |= flags;
	/* mesg opcode */
	*ptr = dyndns_put_nshort(*ptr, opcode);
	/* zone record count */
	*ptr = dyndns_put_nshort(*ptr, quest_zone_cnt);
	/* prerequiste record count */
	*ptr = dyndns_put_nshort(*ptr, ans_prereq_cnt);
	/* update record count */
	*ptr = dyndns_put_nshort(*ptr, nameser_update_cnt);
	/* additional record count */
	*ptr = dyndns_put_nshort(*ptr, addit_cnt);

	return (0);
}

/*
 * dyndns_build_quest_zone
 * Build the question section for query message or zone section for
 * update message.
 * Parameters:
 *   ptr    : address of pointer to buffer to store question or zone section
 *   buf_len: buffer length
 *   name   : question or zone name
 *   type   : type of question or zone
 *   class  : class of question or zone
 * Returns:
 *   ptr: address of pointer to next available buffer space
 *   -1 : error
 *    0 : success
 */
int
dyndns_build_quest_zone(char **ptr, int buf_len, char *name, int type,
	int class)
{
	char *zonePtr;

	if ((strlen(name) + 6) > buf_len) {
		syslog(LOG_ERR, "dyndns: no more buf "
		    "for question/zone section\n");
		return (-1);
	}

	zonePtr = *ptr;
	(void) dyndns_stuff_str(&zonePtr, name);
	*ptr = zonePtr;
	*ptr = dyndns_put_nshort(*ptr, type);
	*ptr = dyndns_put_nshort(*ptr, class);
	return (0);
}

/*
 * dyndns_build_update
 * Build update section of update message for adding and removing a record.
 * If the ttl value is 0 then this message is for record deletion.
 *
 * Parameters:
 *   ptr     : address of pointer to buffer to store update section
 *   buf_len : buffer length
 *   name    : resource name of this record
 *   type    : type of this record
 *   class   : class of this record
 *   ttl     : time-to-live, cached time of this entry by others and not
 *             within DNS database, a zero value for record(s) deletion
 *   data    : data of this resource record
 *   forw_rev: UPDATE_FORW for forward zone, UPDATE_REV for reverse zone
 *   add_del : UPDATE_ADD for adding entry, UPDATE_DEL for removing zone
 *   del_type: DEL_ONE for deleting one entry, DEL_ALL for deleting all
 *             entries of the same resource name.  Only valid for UPDATE_DEL.
 * Returns:
 *   ptr: address of pointer to next available buffer space
 *   -1 : error
 *    0 : success
 */
static int
dyndns_build_update(char **ptr, int buf_len, char *name, int type, int class,
	uint32_t ttl, char *data, int forw_rev, int add_del, int del_type)
{
	char *namePtr;
	int rec_len, data_len;

	rec_len = strlen(name) + 10;
	if (add_del == UPDATE_ADD) {
		if (forw_rev == UPDATE_FORW)
			data_len = 4;
		else
			data_len = strlen(data) + 2;
	} else {
		if (del_type == DEL_ALL)
			data_len = 0;
		else if (forw_rev == UPDATE_FORW)
			data_len = 4;
		else
			data_len = strlen(data) + 2;
	}

	if (rec_len + data_len > buf_len) {
		syslog(LOG_ERR, "dyndns: no more buf for update section\n");
		return (-1);
	}

	namePtr = *ptr;
	(void) dyndns_stuff_str(&namePtr, name);
	*ptr = namePtr;
	*ptr = dyndns_put_nshort(*ptr, type);
	*ptr = dyndns_put_nshort(*ptr, class);
	*ptr = dyndns_put_nlong(*ptr, ttl);

	if (add_del == UPDATE_DEL && del_type == DEL_ALL) {
		*ptr = dyndns_put_nshort(*ptr, 0);
		return (0);
	}

	if (forw_rev == UPDATE_FORW) {
		*ptr = dyndns_put_nshort(*ptr, 4);
		*ptr = dyndns_put_int(*ptr, inet_addr(data));	/* ip address */
	} else {
		*ptr = dyndns_put_nshort(*ptr, strlen(data)+2);
		namePtr = *ptr;
		(void) dyndns_stuff_str(&namePtr, data);	/* hostname */
		*ptr = namePtr;
	}
	return (0);
}

/*
 * dyndns_build_tkey
 * Build TKEY section to establish security context for secure dynamic DNS
 * update.  DNS header and question sections need to be build before this
 * section.  The TKEY data are the tokens generated during security context
 * establishment and the TKEY message is used to transmit those tokens, one
 * at a time, to the DNS server.
 * Parameters:
 *   ptr       : address of pointer to buffer to store TKEY
 *   buf_len   : buffer length
 *   name      : key name, must be unique and same as for TSIG record
 *   key_expire: expiration time of this key in second
 *   data      : TKEY data
 *   data_size : data size
 * Returns:
 *   ptr: address of the pointer to the next available buffer space
 *   -1 : error
 *    0 : success
 */
static int
dyndns_build_tkey(char **ptr, int buf_len, char *name, int key_expire,
	char *data, int data_size)
{
	char *namePtr;
	struct timeval tp;

	if (strlen(name)+2 + 45 + data_size > buf_len) {
		syslog(LOG_ERR, "dyndns: no more buf for TKEY record\n");
		return (-1);
	}

	namePtr = *ptr;
	(void) dyndns_stuff_str(&namePtr, name);	/* unique global name */
	*ptr = namePtr;
	*ptr = dyndns_put_nshort(*ptr, ns_t_tkey);
	*ptr = dyndns_put_nshort(*ptr, ns_c_any);
	*ptr = dyndns_put_nlong(*ptr, 0);
	/* 19 + 14 + data_size + 2 */
	*ptr = dyndns_put_nshort(*ptr, 35 + data_size);
	namePtr = *ptr;
	(void) dyndns_stuff_str(&namePtr, "gss.microsoft.com");
	*ptr = namePtr;
	(void) gettimeofday(&tp, 0);
	*ptr = dyndns_put_nlong(*ptr, tp.tv_sec);	/* inception */
	/* expiration, 86400 */
	*ptr = dyndns_put_nlong(*ptr, tp.tv_sec + key_expire);
	*ptr = dyndns_put_nshort(*ptr, MODE_GSS_API);	/* mode: gss-api */
	*ptr = dyndns_put_nshort(*ptr, 0);		/* error */
	*ptr = dyndns_put_nshort(*ptr, data_size);	/* key size */
	(void) memcpy(*ptr, data, data_size);	/* key data */
	*ptr += data_size;
	*ptr = dyndns_put_nshort(*ptr, 0);	/* other */
	return (0);
}

/*
 * dyndns_build_tsig
 * Build TSIG section for secure dynamic DNS update.  This routine will be
 * called twice.  First called with TSIG_UNSIGNED, and second with TSIG_SIGNED.
 * The TSIG data is NULL and ignored for TSIG_UNSIGNED and is the update request
 * message encrypted for TSIG_SIGNED.  The message id must be the same id as the
 * one in the update request before it is encrypted.
 * Parameters:
 *   ptr        : address of pointer to buffer to store TSIG
 *   buf_len    : buffer length
 *   msg_id     : message id
 *   name       : key name, must be the same as in TKEY record
 *   fudge_time : amount of error time allow in seconds
 *   data       : TSIG data if TSIG_SIGNED, otherwise NULL
 *   data_size  : size of data, otherwise 0 if data is NULL
 *   data_signed: TSIG_SIGNED to indicate data is signed and encrypted,
 *                otherwise TSIG_UNSIGNED
 * Returns:
 *   ptr: address of pointer to next available buffer space
 *   -1 : error
 *    0 : success
 */
static int
dyndns_build_tsig(char **ptr, int buf_len, int msg_id, char *name,
	int fudge_time, char *data, int data_size, int data_signed)
{
	char *namePtr;
	struct timeval tp;
	int signtime, fudge, rec_len;

	if (data_signed == TSIG_UNSIGNED)
		rec_len = strlen(name)+2 + 37;
	else
		rec_len = strlen(name)+2 + 45 + data_size;

	if (rec_len > buf_len) {
		syslog(LOG_ERR, "dyndns: no more buf for TSIG record\n");
		return (-1);
	}

	namePtr = *ptr;
	(void) dyndns_stuff_str(&namePtr, name);	/* unique global name */
	*ptr = namePtr;
	if (data_signed == TSIG_SIGNED)
		*ptr = dyndns_put_nshort(*ptr, ns_t_tsig);
	*ptr = dyndns_put_nshort(*ptr, ns_c_any);
	*ptr = dyndns_put_nlong(*ptr, 0);
	if (data_signed == TSIG_SIGNED) {
		/* 19 + 10 + data_size + 6 */
		*ptr = dyndns_put_nshort(*ptr, 35 + data_size);
	}
	namePtr = *ptr;
	(void) dyndns_stuff_str(&namePtr, "gss.microsoft.com");
	*ptr = namePtr;
	(void) gettimeofday(&tp, 0);
	signtime = tp.tv_sec >> 16;
	*ptr = dyndns_put_nlong(*ptr, signtime);	/* sign time */
	fudge = tp.tv_sec << 16;
	fudge |= fudge_time;
	*ptr = dyndns_put_nlong(*ptr, fudge);	/* fudge time */
	if (data_signed == TSIG_SIGNED) {
		/* signed data size */
		*ptr = dyndns_put_nshort(*ptr, data_size);
		(void) memcpy(*ptr, data, data_size);	/* signed data */
		*ptr += data_size;
		*ptr = dyndns_put_nshort(*ptr, msg_id);	/* original id */
	}
	*ptr = dyndns_put_nshort(*ptr, 0);	/* error */
	*ptr = dyndns_put_nshort(*ptr, 0);	/* other */
	return (0);
}

/*
 * dyndns_open_init_socket
 * This routine creates a SOCK_STREAM or SOCK_DGRAM socket and initializes it
 * by doing bind() and setting linger option to off.
 *
 * Parameters:
 *   sock_type: SOCK_STREAM for TCP or SOCK_DGRAM for UDP
 *   dest_addr: destination address in network byte order
 *   port     : destination port number
 * Returns:
 *   descriptor: descriptor referencing the created socket
 *   -1        : error
 */
int
dyndns_open_init_socket(int sock_type, unsigned long dest_addr, int port)
{
	int s;
	struct sockaddr_in my_addr;
	struct linger l;
	struct sockaddr_in serv_addr;

	if ((s = socket(AF_INET, sock_type, 0)) == -1) {
		syslog(LOG_ERR, "dyndns: socket err\n");
		return (-1);
	}

	l.l_onoff = 0;
	if (setsockopt(s, SOL_SOCKET, SO_LINGER,
	    (char *)&l, sizeof (l)) == -1) {
		syslog(LOG_ERR, "dyndns: setsocket err\n");
		(void) close(s);
		return (-1);
	}

	bzero(&my_addr, sizeof (my_addr));
	my_addr.sin_family = AF_INET;
	my_addr.sin_port = htons(0);
	my_addr.sin_addr.s_addr = htonl(INADDR_ANY);

	if (bind(s, (struct sockaddr *)&my_addr, sizeof (my_addr)) < 0) {
		syslog(LOG_ERR, "dyndns: client bind err\n");
		(void) close(s);
		return (-1);
	}

	serv_addr.sin_family = AF_INET;
	serv_addr.sin_port = htons(port);
	serv_addr.sin_addr.s_addr = dest_addr;

	if (connect(s, (struct sockaddr *)&serv_addr,
	    sizeof (struct sockaddr_in)) < 0) {
		syslog(LOG_ERR, "dyndns: client connect err (%s)\n",
		    strerror(errno));
		(void) close(s);
		return (-1);
	}

	return (s);
}

/*
 * dyndns_acquire_cred
 * This routine is used to acquire a GSS credential handle to a user's Kerberos
 * ticket-granting ticket (TGT) stored locally on the system.  If getting a
 * handle fails, then a new TGT will be obtained again before trying to get a
 * handle once more.
 * The user's password is taken from the environment variable
 * lookup.dns.dynamic.passwd and is encrypted.
 * Paramaters:
 *   kinit_retry: if 0 then a new TGT can be obtained before second attempt to
 *                get a handle to TGT if first attempt fails
 * Returns:
 * user_name  : name of user to get credential handle from
 * credHandle : handle to user's credential (TGT)
 * oid        : contains Kerberos 5 object identifier
 * kinit_retry: 1 if a new TGT has been acquired in this routine, otherwise 0
 *   -1         : error
 */
static int
dyndns_acquire_cred(gss_cred_id_t *credHandle, char *user_name,
	gss_OID *oid, int *kinit_retry)
{
	char *p, pwd[100];

	smb_config_rdlock();
	p = smb_config_getstr(SMB_CI_ADS_USER);
	if (p == NULL || *p == 0) {
		syslog(LOG_ERR, "No user configured for "
		    "secure dynamic DNS update.\n");
		smb_config_unlock();
		return (-1);
	}
	(void) strcpy(user_name, p);

	p = smb_config_getstr(SMB_CI_ADS_PASSWD);
	if (p == NULL || *p == 0) {
		syslog(LOG_ERR, "No password configured for "
		    "secure dynamic DNS update.\n");
		smb_config_unlock();
		return (-1);
	}
	smb_config_unlock();

	(void) strcpy(pwd, p);

	return krb5_acquire_cred_kinit(user_name, pwd, credHandle,
	    oid, kinit_retry, "dyndns");
}

/*
 * dyndns_build_tkey_msg
 * This routine is used to build the TKEY message to transmit GSS tokens
 * during GSS security context establishment for secure DNS update.  The
 * TKEY message format uses the DNS query message format.  The TKEY section
 * is the answer section of the query message format.
 * Microsoft uses a value of 86400 seconds (24 hours) for key expiration time.
 * Parameters:
 *   buf     : buffer to build and store TKEY message
 *   key_name: a unique key name, this same key name must be also be used in
 *             the TSIG message
 *   out_tok : TKEY message data (GSS tokens)
 * Returns:
 *   id          : message id of this TKEY message
 *   message size: the size of the TKEY message
 *   -1          : error
 */
static int
dyndns_build_tkey_msg(char *buf, char *key_name, uint16_t *id,
	gss_buffer_desc *out_tok)
{
	int queryReq, zoneCount, preqCount, updateCount, additionalCount;
	int zoneType, zoneClass;
	char *bufptr;

	queryReq = REQ_QUERY;
	/* query section of query request */
	zoneCount = 1;
	/* answer section of query request */
	preqCount = 1;
	updateCount = 0;
	additionalCount = 0;

	(void) memset(buf, 0, MAX_TCP_SIZE);
	bufptr = buf;
	*id = smb_get_next_resid();

	/* add TCP length info that follows this field */
	bufptr = dyndns_put_nshort(bufptr,
	    26 + (strlen(key_name)+2)*2 + 35 + out_tok->length);

	if (dyndns_build_header(&bufptr, BUFLEN_TCP(bufptr, buf), *id, queryReq,
	    zoneCount, preqCount, updateCount, additionalCount, 0) == -1) {
		return (-1);
	}

	zoneType = ns_t_tkey;
	zoneClass = ns_c_in;
	if (dyndns_build_quest_zone(&bufptr, BUFLEN_TCP(bufptr, buf), key_name,
	    zoneType, zoneClass) == -1) {
		return (-1);
	}

	if (dyndns_build_tkey(&bufptr, BUFLEN_TCP(bufptr, buf), key_name,
	    86400, out_tok->value, out_tok->length) == -1) {
		return (-1);
	}

	return (bufptr - buf);
}

/*
 * dyndns_establish_sec_ctx
 * This routine is used to establish a security context with the DNS server
 * by building TKEY messages and sending them to the DNS server.  TKEY messages
 * are also received from the DNS server for processing.   The security context
 * establishment is done with the GSS client on the system producing a token
 * and sending the token within the TKEY message to the GSS server on the DNS
 * server.  The GSS server then processes the token and then send a TKEY reply
 * message with a new token to be processed by the GSS client.  The GSS client
 * processes the new token and then generates a new token to be sent to the
 * GSS server.  This cycle is continued until the security establishment is
 * done.  TCP is used to send and receive TKEY messages.
 * If gss_init_sec_context fails then a new TGT will be acquired so that
 * security establishment can be retry once more by the caller after getting
 * a handle to the new TGT (credential).
 * Parameters:
 *   credHandle  : handle to credential
 *   s           : socket descriptor to DNS server
 *   key_name    : TKEY key name
 *   dns_hostname: fully qualified DNS hostname
 *   oid         : contains Kerberos 5 object identifier
 *   user_name   : name of user to perform DNS update
 *   kinit_retry : if 0 and gss_init_sec_context fails then get new TGT so
 *                 the caller can restart doing security context establishment
 * Returns:
 *   gss_context    : handle to security context
 *   kinit_retry    : 1 if a new TGT has been acquired in this routine,
 *                    otherwise 0
 *   do_acquire_cred: if 1 then caller will restart security context
 *                    establishment
 *   -1             : error
 */
static int
dyndns_establish_sec_ctx(gss_ctx_id_t *gss_context, gss_cred_id_t credHandle,
    int s, char *key_name, char *dns_hostname, gss_OID oid, char *user_name,
    int *kinit_retry, int *do_acquire_cred)
{
	uint16_t id, rid, rsz;
	char buf[MAX_TCP_SIZE], buf2[MAX_TCP_SIZE];
	int ret;
	char *service_name, *tmpptr;
	int service_sz;
	OM_uint32 min, maj, time_rec;
	gss_buffer_desc service_buf, in_tok, out_tok;
	gss_name_t target_name;
	gss_buffer_desc *inputptr;
	int gss_flags;
	OM_uint32 ret_flags;
	int buf_sz;
	char *p, pwd[100];

	smb_config_rdlock();
	p = smb_config_getstr(SMB_CI_ADS_PASSWD);
	if (p == NULL || *p == 0) {
		syslog(LOG_ERR, "No password configured for "
		    "secure dynamic DNS update.\n");
		smb_config_unlock();
		return (-1);
	}
	smb_config_unlock();
	(void) strcpy(pwd, p);

	service_sz = strlen(dns_hostname) + 5;
	service_name = (char *)malloc(sizeof (char) * service_sz);
	if (service_name == NULL) {
		syslog(LOG_ERR, "Malloc failed for %d bytes ", service_sz);
		smb_config_unlock();
		return (-1);
	}
	(void) snprintf(service_name, service_sz, "DNS@%s", dns_hostname);
	service_buf.value = service_name;
	service_buf.length = strlen(service_name)+1;
	if ((maj = gss_import_name(&min, &service_buf,
	    (gss_OID) gss_nt_service_name,
	    &target_name)) != GSS_S_COMPLETE) {
		display_stat(maj, min);
		(void) gss_release_oid(&min, &oid);
		(void) free(service_name);
		return (-1);
	}
	(void) free(service_name);

	inputptr = GSS_C_NO_BUFFER;
	*gss_context = GSS_C_NO_CONTEXT;
	gss_flags = GSS_C_MUTUAL_FLAG | GSS_C_DELEG_FLAG | GSS_C_REPLAY_FLAG |
	    GSS_C_SEQUENCE_FLAG | GSS_C_CONF_FLAG | GSS_C_INTEG_FLAG;
	do {
		if (krb5_establish_sec_ctx_kinit(user_name, pwd, credHandle,
		    gss_context, target_name, oid, gss_flags, inputptr,
		    &out_tok, &ret_flags, &time_rec, kinit_retry,
		    do_acquire_cred, &maj, "dyndns") == -1) {
			(void) gss_release_oid(&min, &oid);
			(void) gss_release_name(&min, &target_name);
			return (-1);
		}

		if ((maj == GSS_S_COMPLETE) &&
		    !(ret_flags & GSS_C_REPLAY_FLAG)) {
			syslog(LOG_ERR, "dyndns: No GSS_C_REPLAY_FLAG\n");
			if (out_tok.length > 0)
				(void) gss_release_buffer(&min, &out_tok);
			(void) gss_release_oid(&min, &oid);
			(void) gss_release_name(&min, &target_name);
			return (-1);
		}

		if ((maj == GSS_S_COMPLETE) &&
		    !(ret_flags & GSS_C_MUTUAL_FLAG)) {
			syslog(LOG_ERR, "dyndns: No GSS_C_MUTUAL_FLAG\n");
			if (out_tok.length > 0)
				(void) gss_release_buffer(&min, &out_tok);
			(void) gss_release_oid(&min, &oid);
			(void) gss_release_name(&min, &target_name);
			return (-1);
		}

		if (out_tok.length > 0) {
			if ((buf_sz = dyndns_build_tkey_msg(buf, key_name,
			    &id, &out_tok)) <= 0) {
				(void) gss_release_buffer(&min, &out_tok);
				(void) gss_release_oid(&min, &oid);
				(void) gss_release_name(&min, &target_name);
				return (-1);
			}

			(void) gss_release_buffer(&min, &out_tok);

			if (send(s, buf, buf_sz, 0) == -1) {
				syslog(LOG_ERR, "dyndns: TKEY send error\n");
				(void) gss_release_oid(&min, &oid);
				(void) gss_release_name(&min, &target_name);
				return (-1);
			}

			bzero(buf2, MAX_TCP_SIZE);
			if (recv(s, buf2, MAX_TCP_SIZE, 0) == -1) {
				syslog(LOG_ERR, "dyndns: TKEY "
				    "reply recv error\n");
				(void) gss_release_oid(&min, &oid);
				(void) gss_release_name(&min, &target_name);
				return (-1);
			}

			ret = buf2[5] & 0xf;	/* error field in TCP */
			if (ret != NOERROR) {
				syslog(LOG_ERR, "dyndns: Error in "
				    "TKEY reply: %d: ", ret);
				dyndns_msg_err(ret);
				(void) gss_release_oid(&min, &oid);
				(void) gss_release_name(&min, &target_name);
				return (-1);
			}

			tmpptr = &buf2[2];
			(void) dyndns_get_nshort(tmpptr, &rid);
			if (id != rid) {
				(void) gss_release_oid(&min, &oid);
				(void) gss_release_name(&min, &target_name);
				return (-1);
			}

			tmpptr = &buf2[59+(strlen(key_name)+2)*2];
			(void) dyndns_get_nshort(tmpptr, &rsz);
			in_tok.length = rsz;

			/* bsd38 -> 2*7=14 */
			in_tok.value = &buf2[61+(strlen(key_name)+2)*2];
			inputptr = &in_tok;
		}

	} while (maj != GSS_S_COMPLETE);

	(void) gss_release_oid(&min, &oid);
	(void) gss_release_name(&min, &target_name);

	return (0);
}

/*
 * dyndns_get_sec_context
 * Get security context for secure dynamic DNS update.  This routine opens
 * a TCP socket to the DNS server and calls routines to get a handle to a
 * locally cached user's credential and establish a security context with
 * the DNS server to perform secure dynamic DNS update.  If getting security
 * context fails then a retry may be done after reobtaining new credential and
 * getting a new credential handle.  If obtaining new credential has been
 * done earlier during getting a handle to credential then there is no need to
 * do a retry for security context.
 * Parameters:
 *   hostname: fully qualified hostname
 *   dns_ip  : ip address of hostname in network byte order
 * Returns:
 *   gss_handle: gss credential handle
 *   gss_context: gss security context
 *   -1: error
 *    0: success
 */
static gss_ctx_id_t
dyndns_get_sec_context(const char *hostname, int dns_ip)
{
	int s;
	gss_cred_id_t credHandle;
	gss_ctx_id_t gss_context;
	gss_OID oid;
	OM_uint32 min;
	struct hostent *hentry;
	int kinit_retry, do_acquire_cred;
	char *key_name, dns_hostname[255], user_name[50];

	key_name = (char *)hostname;

	hentry = gethostbyaddr((char *)&dns_ip, 4, AF_INET);
	if (hentry == NULL) {
		syslog(LOG_ERR, "dyndns: Can't get DNS "
		    "hostname from DNS ip.\n");
		return (NULL);
	}
	(void) strcpy(dns_hostname, hentry->h_name);

	if ((s = dyndns_open_init_socket(SOCK_STREAM, dns_ip, 53)) < 0) {
		return (NULL);
	}

	kinit_retry = 0;
	do_acquire_cred = 0;
	acquire_cred:

	if (dyndns_acquire_cred(&credHandle, user_name, &oid, &kinit_retry)) {
		(void) close(s);
		return (NULL);
	}

	if (dyndns_establish_sec_ctx(&gss_context, credHandle, s, key_name,
	    dns_hostname, oid, user_name, &kinit_retry, &do_acquire_cred)) {
		(void) gss_release_cred(&min, &credHandle);
		if (do_acquire_cred) {
			do_acquire_cred = 0;
			goto acquire_cred;
		}
		(void) close(s);
		return (NULL);
	}

	(void) close(s);

	(void) gss_release_cred(&min, &credHandle);
	return (gss_context);
}

/*
 * dyndns_build_add_remove_msg
 * This routine builds the update request message for adding and removing DNS
 * entries which is used for non-secure and secure DNS update.
 * This routine builds an UDP message.
 * Parameters:
 *   buf        : buffer to build message
 *   update_zone: the type of zone to update, use UPDATE_FORW for forward
 *                lookup zone, use UPDATE_REV for reverse lookup zone
 *   hostname   : fully qualified hostname to update DNS with
 *   ip_addr    : IP address of hostname
 *   life_time  : cached time of this entry by others and not within DNS
 *                database
 *   update_type: UPDATE_ADD to add entry, UPDATE_DEL to remove entry
 *   del_type   : DEL_ONE for deleting one entry, DEL_ALL for deleting all
 *                entries of the same resource name.  Only valid for UPDATE_DEL.
 *   addit_cnt  : Indicate how many record is in the additional section of
 *                the DNS message.  A value of zero is always used with
 *                non-secure update message. For secure update message,
 *                the value will be one because the signed TSIG message
 *                is added as the additional record of the DNS update message.
 *   id         : DNS message ID.  If a positive value then this ID value is
 *                used, otherwise the next incremented value is used
 *   level      : This is the domain level which we send the request to, level
 *                zero is the default level, it can go upto 2 in reverse zone
 *                and virtually to any level in forward zone.
 * Returns:
 *   buf      : buffer containing update message
 *   id       : DNS message ID
 *   int      : size of update message
 *   -1       : error
 *
 * This function is changed to handle dynamic DNS update retires to higher
 * authoritative domains.
 */
static int
dyndns_build_add_remove_msg(char *buf, int update_zone, const char *hostname,
	const char *ip_addr, int life_time, int update_type, int del_type,
	int addit_cnt, uint16_t *id, int level)
{
	int a, b, c, d;
	char *bufptr;
	int queryReq, zoneCount, preqCount, updateCount, additionalCount;
	char *zone, *resource, *data, zone_buf[100], resrc_buf[100];
	int zoneType, zoneClass, type, class, ttl;
	char *p;

	queryReq = REQ_UPDATE;
	zoneCount = 1;
	preqCount = 0;
	updateCount = 1;
	additionalCount = addit_cnt;

	(void) memset(buf, 0, NS_PACKETSZ);
	bufptr = buf;

	if (*id == 0)
		*id = smb_get_next_resid();

	if (dyndns_build_header(&bufptr, BUFLEN_UDP(bufptr, buf), *id, queryReq,
	    zoneCount, preqCount, updateCount, additionalCount, 0) == -1) {
		return (-1);
	}

	zoneType = ns_t_soa;
	zoneClass = ns_c_in;

	if (update_zone == UPDATE_FORW) {
		p = (char *)hostname;

		/* Try higher domains according to the level requested */
		do {
			/* domain */
			if ((zone = (char *)strchr(p, '.')) == NULL)
				return (-1);
			zone += 1;
			p = zone;
		} while (--level >= 0);
		resource = (char *)hostname;
		data = (char *)ip_addr;
	} else {
		(void) sscanf(ip_addr, "%d.%d.%d.%d", &a, &b, &c, &d);
		(void) sprintf(zone_buf, "%d.%d.%d.in-addr.arpa", c, b, a);
		zone = p = zone_buf;

		/* Try higher domains according to the level requested */
		while (--level >= 0) {
			/* domain */
			if ((zone = (char *)strchr(p, '.')) == NULL) {
				return (-1);
			}
			zone += 1;
			p = zone;
		}

		(void) sprintf(resrc_buf, "%d.%d.%d.%d.in-addr.arpa",
		    d, c, b, a);
		resource = resrc_buf;	/* ip info */
		data = (char *)hostname;
	}

	if (dyndns_build_quest_zone(&bufptr, BUFLEN_UDP(bufptr, buf), zone,
	    zoneType, zoneClass) == -1) {
		return (-1);
	}

	if (update_zone == UPDATE_FORW)
		type = ns_t_a;
	else
		type = ns_t_ptr;

	if (update_type == UPDATE_ADD) {
		class = ns_c_in;
		ttl = life_time;
	} else {
		if (del_type == DEL_ONE)
			class = ns_c_none;	/* remove one */
		else
			class = ns_c_any;	/* remove all */
		ttl = 0;
	}
	if (dyndns_build_update(&bufptr, BUFLEN_UDP(bufptr, buf),
	    resource, type, class, ttl, data, update_zone,
	    update_type, del_type) == -1) {
		return (-1);
	}

	return (bufptr - buf);
}

/*
 * dyndns_build_unsigned_tsig_msg
 * This routine is used to build the unsigned TSIG message for signing.  The
 * unsigned TSIG message contains the update request message with certain TSIG
 * fields included.  An error time of 300 seconds is used for fudge time.  This
 * is the number used by Microsoft clients.
 * This routine builds a UDP message.
 * Parameters:
 *   buf        : buffer to build message
 *   update_zone: the type of zone to update, use UPDATE_FORW for forward
 *                lookup zone, use UPDATE_REV for reverse lookup zone
 *   hostname   : fully qualified hostname to update DNS with
 *   ip_addr    : IP address of hostname
 *   life_time  : cached time of this entry by others and not within DNS
 *                database
 *   update_type: UPDATE_ADD to add entry, UPDATE_DEL to remove entry
 *   del_type   : DEL_ONE for deleting one entry, DEL_ALL for deleting all
 *                entries of the same resource name.  Only valid for UPDATE_DEL.
 *   key_name   : same key name used in TKEY message
 *   id         : DNS message ID.  If a positive value then this ID value is
 *                used, otherwise the next incremented value is used
 *   level      : This is the domain level which we send the request to, level
 *                zero is the default level, it can go upto 2 in reverse zone
 *                and virtually to any level in forward zone.
 * Returns:
 *   buf      : buffer containing update message
 *   id       : DNS message ID
 *   int      : size of update message
 *   -1       : error
 */
static int
dyndns_build_unsigned_tsig_msg(char *buf, int update_zone, const char *hostname,
	const char *ip_addr, int life_time, int update_type, int del_type,
	char *key_name, uint16_t *id, int level)
{
	char *bufptr;
	int buf_sz;

	if ((buf_sz = dyndns_build_add_remove_msg(buf, update_zone, hostname,
	    ip_addr, life_time, update_type, del_type, 0, id, level)) <= 0) {
		return (-1);
	}

	bufptr = buf + buf_sz;

	if (dyndns_build_tsig(&bufptr, BUFLEN_UDP(bufptr, buf), 0,
	    key_name, 300, NULL, 0, TSIG_UNSIGNED) == -1) {
		return (-1);
	}

	return (bufptr - buf);
}

/*
 * dyndns_build_signed_tsig_msg
 * This routine build the signed TSIG message which contains the update
 * request message encrypted.  An error time of 300 seconds is used for fudge
 * time.  This is the number used by Microsoft clients.
 * This routine builds a UDP message.
 * Parameters:
 *   buf        : buffer to build message
 *   update_zone: the type of zone to update, use UPDATE_FORW for forward
 *                lookup zone, use UPDATE_REV for reverse lookup zone
 *   hostname   : fully qualified hostname to update DNS with
 *   ip_addr    : IP address of hostname
 *   life_time  : cached time of this entry by others and not within DNS
 *                database
 *   update_type: UPDATE_ADD to add entry, UPDATE_DEL to remove entry
 *   del_type   : DEL_ONE for deleting one entry, DEL_ALL for deleting all
 *                entries of the same resource name.  Only valid for UPDATE_DEL.
 *   key_name   : same key name used in TKEY message
 *   id         : DNS message ID.  If a positive value then this ID value is
 *                used, otherwise the next incremented value is used
 *   in_mic     : the update request message encrypted
 *   level      : This is the domain level which we send the request to, level
 *                zero is the default level, it can go upto 2 in reverse zone
 *                and virtually to any level in forward zone.
 *
 * Returns:
 *   buf      : buffer containing update message
 *   id       : DNS message ID
 *   int      : size of update message
 *   -1       : error
 */
static int
dyndns_build_signed_tsig_msg(char *buf, int update_zone, const char *hostname,
	const char *ip_addr, int life_time, int update_type, int del_type,
	char *key_name, uint16_t *id, gss_buffer_desc *in_mic, int level)
{
	char *bufptr;
	int buf_sz;

	if ((buf_sz = dyndns_build_add_remove_msg(buf, update_zone, hostname,
	    ip_addr, life_time, update_type, del_type, 1, id, level)) <= 0) {
		return (-1);
	}

	bufptr = buf + buf_sz;

	if (dyndns_build_tsig(&bufptr, BUFLEN_UDP(bufptr, buf),
	    *id, key_name, 300, in_mic->value,
	    in_mic->length, TSIG_SIGNED) == -1) {
		return (-1);
	}

	return (bufptr - buf);
}

/*
 * dyndns_udp_send_recv
 * This routine sends and receives UDP DNS request and reply messages.  Time
 * out value and retry count is indicated by two environment variables:
 * lookup_dns_retry_cnt
 * lookup_dns_retry_sec
 * If either of these two variables are undefined or their value exceed the
 * value of 10 then a default value of 3 retry and/or a default value of 3
 * secs are used.
 *
 * Pre-condition: Caller must call dyndns_open_init_socket() before calling
 * this function.
 *
 * Parameters:
 *   s        : socket descriptor
 *   buf      : buffer containing data to send
 *   buf_sz   : size of data to send
 * Returns:
 *   -1     : error
 *   rec_buf: reply dat
 *    0     : success
 */
int
dyndns_udp_send_recv(int s, char *buf, int buf_sz, char *rec_buf)
{
	int i, retval, addr_len, max_retries;
	struct timeval tv, timeout;
	fd_set rfds;
	struct sockaddr_in from_addr;
	char *p;

	smb_config_rdlock();
	p = smb_config_getstr(SMB_CI_DYNDNS_RETRY_COUNT);
	if (p == NULL || *p == 0) {
		max_retries = 3;
	} else {
		max_retries = atoi(p);
		if (max_retries < 1 || max_retries > 10)
			max_retries = 3;
	}

	p = smb_config_getstr(SMB_CI_DYNDNS_RETRY_SEC);
	timeout.tv_usec = 0;
	if (p == NULL || *p == 0) {
		timeout.tv_sec = 3;
	} else {
		timeout.tv_sec = atoi(p);
		if (timeout.tv_sec < 1 || timeout.tv_sec > 10)
			timeout.tv_sec = 3;
	}
	smb_config_unlock();

	for (i = 0; i < max_retries + 1; i++) {
		if (send(s, buf, buf_sz, 0) == -1) {
			syslog(LOG_ERR, "dyndns: UDP send error (%s)\n",
			    strerror(errno));
			return (-1);
		}

		FD_ZERO(&rfds);
		FD_SET(s, &rfds);

		tv = timeout;

		retval = select(s+1, &rfds, NULL, NULL, &tv);

		if (retval == -1) {
			return (-1);
		} else if (retval > 0) {
			bzero(rec_buf, NS_PACKETSZ);
			/* required by recvfrom */
			addr_len = sizeof (struct sockaddr_in);
			if (recvfrom(s, rec_buf, NS_PACKETSZ, 0,
			    (struct sockaddr *)&from_addr, &addr_len) == -1) {
				syslog(LOG_ERR, "dyndns: UDP recv err\n");
				return (-1);
			}
			break;
		}
	}

	if (i == (max_retries + 1)) { /* did not receive anything */
		syslog(LOG_ERR, "dyndns: max retries for UDP recv reached\n");
		return (-1);
	}

	return (0);
}

/*
 * dyndns_sec_add_remove_entry
 * Perform secure dynamic DNS update after getting security context.
 * This routine opens a UDP socket to the DNS sever, gets the security context,
 * builds the unsigned TSIG message and signed TSIG message.  The signed TSIG
 * message containing the encrypted update request message is sent to the DNS
 * server.  The response is received and check for error.  If there is no
 * error then credential handle and security context are released and the local
 * NSS cached is purged.
 * Parameters:
 *   update_zone : UPDATE_FORW for forward zone, UPDATE_REV for reverse zone
 *   hostname    : fully qualified hostname
 *   ip_addr     : ip address of hostname in string format
 *   life_time   : cached time of this entry by others and not within DNS
 *                 database
 *   max_retries : maximum retries for sending DNS update request
 *   recv_timeout: receive timeout
 *   update_type : UPDATE_ADD for adding entry, UPDATE_DEL for removing entry
 *   del_type    : DEL_ONE for deleting one entry, DEL_ALL for deleting all
 *                 entries of the same resource name.  Only valid for UPDATE_DEL
 *   dns_str     : DNS IP address in string format
 * Returns:
 *   -1: error
 *    0: success
 *
 * This function is enhanced to handle the case of NOTAUTH error when DNS server
 * is not authoritative for specified zone. In this case we need to resend the
 * same request to the higher authoritative domains.
 * This is true for both secure and unsecure dynamic DNS updates.
 */
static int
dyndns_sec_add_remove_entry(int update_zone, const char *hostname,
    const char *ip_addr, int life_time, int update_type, int del_type,
    char *dns_str)
{
	int s2;
	uint16_t id, rid;
	char buf[NS_PACKETSZ], buf2[NS_PACKETSZ];
	int ret;
	OM_uint32 min, maj;
	gss_buffer_desc in_mic, out_mic;
	gss_ctx_id_t gss_context;
	int dns_ip;
	char *key_name;
	int buf_sz;
	int level = 0;

	assert(dns_str);
	assert(*dns_str);

	dns_ip = inet_addr(dns_str);

sec_retry_higher:

	if ((gss_context = dyndns_get_sec_context(hostname,
	    dns_ip)) == NULL) {
		return (-1);
	}

	key_name = (char *)hostname;

	if ((s2 = dyndns_open_init_socket(SOCK_DGRAM, dns_ip, 53)) < 0) {
		(void) gss_delete_sec_context(&min, &gss_context, NULL);
		return (-1);
	}

	id = 0;
	if ((buf_sz = dyndns_build_unsigned_tsig_msg(buf, update_zone, hostname,
	    ip_addr, life_time, update_type, del_type,
	    key_name, &id, level)) <= 0) {
		(void) close(s2);
		(void) gss_delete_sec_context(&min, &gss_context, NULL);
		return (-1);
	}

	in_mic.length = buf_sz;
	in_mic.value = buf;

	/* sign update message */
	if ((maj = gss_get_mic(&min, gss_context, 0, &in_mic, &out_mic)) !=
	    GSS_S_COMPLETE) {
		display_stat(maj, min);
		(void) close(s2);
		(void) gss_delete_sec_context(&min, &gss_context, NULL);
		return (-1);
	}

	if ((buf_sz = dyndns_build_signed_tsig_msg(buf, update_zone, hostname,
	    ip_addr, life_time, update_type, del_type, key_name, &id,
	    &out_mic, level)) <= 0) {
		(void) close(s2);
		(void) gss_release_buffer(&min, &out_mic);
		(void) gss_delete_sec_context(&min, &gss_context, NULL);
		return (-1);
	}

	(void) gss_release_buffer(&min, &out_mic);

	if (dyndns_udp_send_recv(s2, buf, buf_sz, buf2)) {
		(void) close(s2);
		(void) gss_delete_sec_context(&min, &gss_context, NULL);
		return (-1);
	}

	(void) close(s2);

	(void) gss_delete_sec_context(&min, &gss_context, NULL);

	ret = buf2[3] & 0xf;	/* error field in UDP */

	/*
	 * If it is a NOTAUTH error we should retry with higher domains
	 * until we get a successful reply or the maximum retries is met.
	 */
	if (ret == NOTAUTH && level++ < MAX_AUTH_RETRIES)
		goto sec_retry_higher;

	/* check here for update request is successful */
	if (ret != NOERROR) {
		syslog(LOG_ERR, "dyndns: Error in TSIG reply: %d: ", ret);
		dyndns_msg_err(ret);
		return (-1);
	}

	(void) dyndns_get_nshort(buf2, &rid);
	if (id != rid)
		return (-1);

	return (0);
}

/*
 * dyndns_seach_entry
 * Query DNS server for entry.  This routine can indicate if an entry exist
 * or not during forward or reverse lookup.  Also can indicate if the data
 * of the entry matched.  For example, for forward lookup, the entry is
 * searched using the hostname and the data is the IP address.  For reverse
 * lookup, the entry is searched using the IP address and the data is the
 * hostname.
 * Parameters:
 *   update_zone: UPDATE_FORW for forward zone, UPDATE_REV for reverse zone
 *   hostname   : fully qualified hostname
 *   ip_addr    : ip address of hostname in string format
 *   update_type: UPDATE_ADD for adding entry, UPDATE_DEL for removing entry
 * Returns:
 *   time_out: no use
 *   is_match: is 1 for found matching entry, otherwise 0
 *   1       : an entry exist but not necessarily match
 *   0       : an entry does not exist
 */
/*ARGSUSED*/
static int
dyndns_search_entry(int update_zone, const char *hostname, const char *ip_addr,
    int update_type, struct timeval *time_out, int *is_match)
{
	struct hostent *hentry;
	struct in_addr in;
	in_addr_t ip;
	int i;

	*is_match = 0;
	if (update_zone == UPDATE_FORW) {
		hentry = gethostbyname(hostname);
		if (hentry) {
			ip = inet_addr(ip_addr);
			for (i = 0; hentry->h_addr_list[i]; i++) {
				(void) memcpy(&in.s_addr,
				    hentry->h_addr_list[i], sizeof (in.s_addr));
				if (ip == in.s_addr) {
					*is_match = 1;
					break;
				}
			}
			return (1);
		}
	} else {
		int dns_ip = inet_addr(ip_addr);
		hentry = gethostbyaddr((char *)&dns_ip, 4, AF_INET);
		if (hentry) {
			if (strncasecmp(hentry->h_name, hostname,
			    strlen(hostname)) == 0) {
				*is_match = 1;
			}
			return (1);
		}
	}

	/* entry does not exist */
	return (0);
}

/*
 * dyndns_add_remove_entry
 * Perform non-secure dynamic DNS update.  If fail then tries secure update.
 * This routine opens a UDP socket to the DNS sever, build the update request
 * message, and sends the message to the DNS server.  The response is received
 * and check for error.  If there is no error then the local NSS cached is
 * purged.  DNS may be used to check to see if an entry already exist before
 * adding or to see if an entry does exist before removing it.  Adding
 * duplicate entries or removing non-existing entries does not cause any
 * problems.  DNS is not check when doing a delete all.
 * Parameters:
 *   update_zone: UPDATE_FORW for forward zone, UPDATE_REV for reverse zone
 *   hostname   : fully qualified hostname
 *   ip_addr    : ip address of hostname in string format
 *   life_time  : cached time of this entry by others and not within DNS
 *                database
 *   update_type: UPDATE_ADD to add entry, UPDATE_DEL to remove entry
 *   do_check   : DNS_CHECK to check first in DNS, DNS_NOCHECK for no DNS
 *                checking before update
 *   del_type   : DEL_ONE for deleting one entry, DEL_ALL for deleting all
 *                entries of the same resource name.  Only valid for UPDATE_DEL.
 *   dns_str    : DNS IP address in string format
 * Returns:
 *   -1: error
 *    0: success
 *
 * This function is enhanced to handle the case of NOTAUTH error when DNS server
 * is not authoritative for specified zone. In this case we need to resend the
 * same request to the higher authoritative domains.
 * This is true for both secure and unsecure dynamic DNS updates.
 */
static int
dyndns_add_remove_entry(int update_zone, const char *hostname,
    const char *ip_addr, int life_time, int update_type,
    int do_check, int del_type, char *dns_str)
{
	int s;
	uint16_t id, rid;
	char buf[NS_PACKETSZ], buf2[NS_PACKETSZ];
	int ret, dns_ip;
	int is_exist, is_match;
	struct timeval timeout;
	int buf_sz;
	int level = 0;

	assert(dns_str);
	assert(*dns_str);

	dns_ip = inet_addr(dns_str);

	if (do_check == DNS_CHECK && del_type != DEL_ALL) {
		is_exist = dyndns_search_entry(update_zone, hostname, ip_addr,
		    update_type, &timeout, &is_match);

		if (update_type == UPDATE_ADD && is_exist && is_match) {
			return (0);
		} else if (update_type == UPDATE_DEL && !is_exist) {
			return (0);
		}
	}

retry_higher:
	if ((s = dyndns_open_init_socket(SOCK_DGRAM, dns_ip, 53)) < 0) {
		return (-1);
	}

	id = 0;
	if ((buf_sz = dyndns_build_add_remove_msg(buf, update_zone, hostname,
	    ip_addr, life_time, update_type, del_type, 0, &id, level)) <= 0) {
		(void) close(s);
		return (-1);
	}

	if (dyndns_udp_send_recv(s, buf, buf_sz, buf2)) {
		(void) close(s);
		return (-1);
	}

	(void) close(s);

	ret = buf2[3] & 0xf;	/* error field in UDP */

	/*
	 * If it is a NOTAUTH error we should retry with higher domains
	 * until we get a successful reply
	 */
	if (ret == NOTAUTH && level++ < MAX_AUTH_RETRIES)
		goto retry_higher;

	/* check here for update request is successful */
	if (ret == NOERROR) {
		(void) dyndns_get_nshort(buf2, &rid);
		if (id != rid)
			return (-1);
		return (0);
	}

	if (ret == NOTIMP) {
		syslog(LOG_ERR, "dyndns: DNS does not "
		    "support dynamic update\n");
		return (-1);
	} else if (ret == NOTAUTH) {
		syslog(LOG_ERR, "dyndns: DNS is not authoritative for "
		    "zone name in zone section\n");
		return (-1);
	}

	ret = dyndns_sec_add_remove_entry(update_zone, hostname,
	    ip_addr, life_time, update_type, del_type, dns_str);

	return (ret);
}

/*
 * dyndns_add_entry
 * Main routine to add an entry into DNS.  The attempt will be made on the
 * the servers returned by smb_get_nameserver().  Upon a successful
 * attempt on any one of the server, the function will exit with 0.
 * Otherwise, -1 is retuned to indicate the update attempt on all the
 * nameservers has failed.
 *
 * Parameters:
 *   update_zone: the type of zone to update, use UPDATE_FORW for forward
 *                lookup zone, use UPDATE_REV for reverse lookup zone
 *   hostname   : fully qualified hostname
 *   ip_addr    : ip address of hostname in string format
 *   life_time  : cached time of this entry by others and not within DNS
 *                database
 * Returns:
 *   -1: error
 *    0: success
 */
static int
dyndns_add_entry(int update_zone, const char *hostname, const char *ip_addr,
    int life_time)
{
	char *dns_str;
	struct in_addr ns_list[MAXNS];
	int i, cnt;
	int addr, rc = 0;

	if (hostname == NULL || ip_addr == NULL) {
		return (-1);
	}

	addr = (int)inet_addr(ip_addr);
	if ((addr == -1) || (addr == 0)) {
		return (-1);
	}

	cnt = smb_get_nameservers(ns_list, MAXNS);

	for (i = 0; i < cnt; i++) {
		dns_str = inet_ntoa(ns_list[i]);
		if ((dns_str == NULL) ||
		    (strcmp(dns_str, "0.0.0.0") == 0)) {
			continue;
		}

		if (update_zone == UPDATE_FORW) {
			syslog(LOG_DEBUG, "Dynamic update on forward lookup "
			    "zone for %s (%s)...\n", hostname, ip_addr);
		} else {
			syslog(LOG_DEBUG, "Dynamic update on reverse lookup "
			    "zone for %s (%s)...\n", hostname, ip_addr);
		}
		if (dyndns_add_remove_entry(update_zone, hostname,
		    ip_addr, life_time,
		    UPDATE_ADD, DNS_NOCHECK, DEL_NONE, dns_str) != -1) {
			rc = 1;
			break;
		}
	}

	return (rc ? 0 : -1);
}

/*
 * dyndns_remove_entry
 * Main routine to remove an entry or all entries of the same resource name
 * from DNS.  The update attempt will be made on the primary DNS server.  If
 * there is a failure then another attempt will be made on the secondary DNS
 * server.
 * Parameters:
 *   update_zone: the type of zone to update, use UPDATE_FORW for forward
 *                lookup zone, use UPDATE_REV for reverse lookup zone
 *   hostname   : fully qualified hostname
 *   ip_addr    : ip address of hostname in string format
 *   del_type   : DEL_ONE for deleting one entry, DEL_ALL for deleting all
 *                entries of the same resource name.  Only valid for UPDATE_DEL
 * Returns:
 *   -1: error
 *    0: success
 */
static int
dyndns_remove_entry(int update_zone, const char *hostname, const char *ip_addr,
	int del_type)
{
	char *dns_str;
	struct in_addr ns_list[MAXNS];
	int i, cnt, scnt;
	int addr;

	if ((hostname == NULL || ip_addr == NULL)) {
		return (-1);
	}

	addr = (int)inet_addr(ip_addr);
	if ((addr == -1) || (addr == 0)) {
		return (-1);
	}

	cnt = smb_get_nameservers(ns_list, MAXNS);
	scnt = 0;

	for (i = 0; i < cnt; i++) {
		dns_str = inet_ntoa(ns_list[i]);
		if ((dns_str == NULL) ||
		    (strcmp(dns_str, "0.0.0.0") == 0)) {
			continue;
		}

		if (update_zone == UPDATE_FORW) {
			if (del_type == DEL_ONE) {
				syslog(LOG_DEBUG, "Dynamic update "
				    "on forward lookup "
				    "zone for %s (%s)...\n", hostname, ip_addr);
			} else {
				syslog(LOG_DEBUG, "Removing all "
				    "entries of %s "
				    "in forward lookup zone...\n", hostname);
			}
		} else {
			if (del_type == DEL_ONE) {
				syslog(LOG_DEBUG, "Dynamic update "
				    "on reverse lookup "
				    "zone for %s (%s)...\n", hostname, ip_addr);
			} else {
				syslog(LOG_DEBUG, "Removing all "
				    "entries of %s "
				    "in reverse lookup zone...\n", ip_addr);
			}
		}
		if (dyndns_add_remove_entry(update_zone, hostname, ip_addr, 0,
		    UPDATE_DEL, DNS_NOCHECK, del_type, dns_str) != -1) {
			scnt++;
			break;
		}
	}
	if (scnt)
		return (0);
	return (-1);
}

/*
 * dyndns_update
 * Perform dynamic update on both forward and reverse lookup zone using
 * current hostname and IP addresses.  Before updating DNS, existing host
 * entries with the same hostname in the forward lookup zone are removed
 * and existing pointer entries with the same IP addresses in the reverse
 * lookup zone are removed.  After DNS update, host entries for current
 * hostname will show current IP addresses and pointer entries for current
 * IP addresses will show current hostname.
 * Parameters:
 *   None
 * Returns:
 *   -1: some dynamic DNS updates errors
 *    0: successful
 */
int
dyndns_update(void)
{
	int i, forw_update_ok, error;
	char fqdn[MAXHOSTNAMELEN];
	char *my_ip;
	int nc_cnt;
	struct in_addr addr;
	int rc;

	if (!dyndns_enabled())
		return (-1);

	if (smb_getfqhostname(fqdn, MAXHOSTNAMELEN) != 0)
		return (-1);

	nc_cnt = smb_nic_get_num();

	error = 0;
	forw_update_ok = 0;

	/*
	 * Dummy IP is okay since we are removing all using the hostname.
	 */
	if (dyndns_remove_entry(UPDATE_FORW, fqdn, "1.1.1.1", DEL_ALL) == 0) {
		forw_update_ok = 1;
	} else {
		error++;
	}

	for (i = 0; i < nc_cnt; i++) {
		net_cfg_t cfg;
		if (smb_nic_get_byind(i, &cfg) == NULL)
			break;
		addr.s_addr = cfg.ip;
		if (addr.s_addr == 0)
			continue;
		if (smb_nic_status(cfg.ifname, IFF_STANDBY) ||
		    smb_nic_status(cfg.ifname, IFF_PRIVATE))
			continue;

		my_ip = (char *)strdup(inet_ntoa(addr));
		if (my_ip == NULL) {
			error++;
			continue;
		}

		if (forw_update_ok) {
			rc = dyndns_add_entry(UPDATE_FORW, fqdn, my_ip,
			    DDNS_TTL);

			if (rc == -1)
				error++;
		}

		rc = dyndns_remove_entry(UPDATE_REV, fqdn, my_ip, DEL_ALL);
		if (rc == 0) {
			rc = dyndns_add_entry(UPDATE_REV, fqdn, my_ip,
			    DDNS_TTL);
		}

		if (rc == -1)
			error++;

		(void) free(my_ip);
	}

	return ((error == 0) ? 0 : -1);
}

/*
 * dyndns_clear_rev_zone
 * Clear the rev zone records. Must be called to clear the OLD if list
 * of down records prior to updating the list with new information.
 *
 * Parameters:
 *   None
 * Returns:
 *   -1: some dynamic DNS updates errors
 *    0: successful
 */
int
dyndns_clear_rev_zone(void)
{
	int i, error;
	char fqdn[MAXHOSTNAMELEN];
	char *my_ip;
	int nc_cnt;
	struct in_addr addr;
	int rc;

	if (!dyndns_enabled())
		return (-1);

	if (smb_getfqhostname(fqdn, MAXHOSTNAMELEN) != 0)
		return (-1);

	nc_cnt = smb_nic_get_num();

	error = 0;

	for (i = 0; i < nc_cnt; i++) {
		net_cfg_t cfg;
		if (smb_nic_get_byind(i, &cfg) == NULL)
			break;
		addr.s_addr = cfg.ip;
		if (addr.s_addr == 0)
			continue;
		if (smb_nic_status(cfg.ifname, IFF_STANDBY) ||
		    smb_nic_status(cfg.ifname, IFF_PRIVATE))
			continue;

		my_ip = (char *)strdup(inet_ntoa(addr));
		if (my_ip == NULL) {
			error++;
			continue;
		}

		rc = dyndns_remove_entry(UPDATE_REV, fqdn, my_ip, DEL_ALL);
		if (rc != 0)
			error++;

		(void) free(my_ip);
	}

	return ((error == 0) ? 0 : -1);
}