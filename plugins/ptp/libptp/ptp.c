/* ptp.c
 *
 * Copyright (C) 2001-2004 Mariusz Woloszyn <emsi@ipartners.pl>
 * Copyright (C) 2003-2007 Marcus Meissner <marcus@jet.franken.de>
 * Copyright (C) 2006-2007 Linus Walleij <triad@df.lth.se>
 * Copyright (C) 2007 Tero Saarni <tero.saarni@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#define _BSD_SOURCE
//#include <config.h>
#include "ptp.h"

#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifdef ENABLE_NLS
#  include <libintl.h>
#  undef _
#  define _(String) dgettext (PACKAGE, String)
#  ifdef gettext_noop
#    define N_(String) gettext_noop (String)
#  else
#    define N_(String) (String)
#  endif
#else
#  define textdomain(String) (String)
#  define gettext(String) (String)
#  define dgettext(Domain,Message) (Message)
#  define dcgettext(Domain,Message,Type) (Message)
#  define bindtextdomain(Domain,Directory) (Domain)
#  define _(String) (String)
#  define N_(String) (String)
#endif

#define CHECK_PTP_RC(result)	{uint16_t r=(result); if (r!=PTP_RC_OK) return r;}

#define PTP_CNT_INIT(cnt) {memset(&cnt,0,sizeof(cnt));}

static uint16_t ptp_exit_recv_memory_handler (PTPDataHandler*,unsigned char**,unsigned long*);
static uint16_t ptp_init_recv_memory_handler(PTPDataHandler*);
static uint16_t ptp_init_send_memory_handler(PTPDataHandler*,unsigned char*,unsigned long len);
static uint16_t ptp_exit_send_memory_handler (PTPDataHandler *handler);

static void
ptp_debug (PTPParams *params, const char *format, ...)
{  
        va_list args;

        va_start (args, format);
        if (params->debug_func!=NULL)
                params->debug_func (params->data, format, args);
        else
	{
                vfprintf (stderr, format, args);
		fprintf (stderr,"\n");
		fflush (stderr);
	}
        va_end (args);
}  

static void
ptp_error (PTPParams *params, const char *format, ...)
{  
        va_list args;

        va_start (args, format);
        if (params->error_func!=NULL)
                params->error_func (params->data, format, args);
        else
	{
                vfprintf (stderr, format, args);
		fprintf (stderr,"\n");
		fflush (stderr);
	}
        va_end (args);
}

/* Pack / unpack functions */

#include "ptp-pack.c"

/* major PTP functions */

/* Transaction data phase description */
#define PTP_DP_NODATA		0x0000	/* no data phase */
#define PTP_DP_SENDDATA		0x0001	/* sending data */
#define PTP_DP_GETDATA		0x0002	/* receiving data */
#define PTP_DP_DATA_MASK	0x00ff	/* data phase mask */

/**
 * ptp_transaction:
 * params:	PTPParams*
 * 		PTPContainer* ptp	- general ptp container
 * 		uint16_t flags		- lower 8 bits - data phase description
 * 		unsigned int sendlen	- senddata phase data length
 * 		char** data		- send or receive data buffer pointer
 * 		int* recvlen		- receive data length
 *
 * Performs PTP transaction. ptp is a PTPContainer with appropriate fields
 * filled in (i.e. operation code and parameters). It's up to caller to do
 * so.
 * The flags decide thether the transaction has a data phase and what is its
 * direction (send or receive). 
 * If transaction is sending data the sendlen should contain its length in
 * bytes, otherwise it's ignored.
 * The data should contain an address of a pointer to data going to be sent
 * or is filled with such a pointer address if data are received depending
 * od dataphase direction (send or received) or is beeing ignored (no
 * dataphase).
 * The memory for a pointer should be preserved by the caller, if data are
 * beeing retreived the appropriate amount of memory is beeing allocated
 * (the caller should handle that!).
 *
 * Return values: Some PTP_RC_* code.
 * Upon success PTPContainer* ptp contains PTP Response Phase container with
 * all fields filled in.
 **/
static uint16_t
ptp_transaction_new (PTPParams* params, PTPContainer* ptp, 
		uint16_t flags, unsigned int sendlen,
		PTPDataHandler *handler
) {
	
	ptp_debug(params,"ptp2/ptp_transaction_new : Get transaction (New)");
	
	if ((params==NULL) || (ptp==NULL)) 
		return PTP_ERROR_BADPARAM;

	ptp->Transaction_ID=params->transaction_id++;
	ptp->SessionID=params->session_id;
	// send request 
	CHECK_PTP_RC(params->sendreq_func (params, ptp));
	// is there a dataphase? 
	switch (flags&PTP_DP_DATA_MASK) {
	case PTP_DP_SENDDATA:
		{
			uint16_t ret;
			ret = params->senddata_func(params, ptp,
						    sendlen, handler);
			if (ret == PTP_ERROR_CANCEL) {
				ret = params->cancelreq_func(params, 
							     params->transaction_id-1);
				if (ret == PTP_RC_OK)
					ret = PTP_ERROR_CANCEL;
			}
			if (ret != PTP_RC_OK)
				return ret;
		}
		break;
	case PTP_DP_GETDATA:
		{
			uint16_t ret;
			ret = params->getdata_func(params, ptp, handler);
			if (ret == PTP_ERROR_CANCEL) 
			{
				ptp_debug(params,"ptp2/ptp_transaction_new : Cancel request.");
				ret = params->cancelreq_func(params, 
							     params->transaction_id-1);
				if (ret == PTP_RC_OK)
					ret = PTP_ERROR_CANCEL;
			}
			if (ret != PTP_RC_OK)
				return ret;
		}
		break;
	case PTP_DP_NODATA:
		break;
	default:
		return PTP_ERROR_BADPARAM;
	}
	// get response 
	ptp_debug(params,"ptp2/ptp_transaction_new : Get the response.");
	CHECK_PTP_RC(params->getresp_func(params, ptp));
	ptp_debug(params,"ptp2/ptp_transaction_new : Check sequence number.");
	if (ptp->Transaction_ID != params->transaction_id-1) 
	{
		ptp_error (params,"ptp2/ptp_transaction_new : Sequence number mismatch %d vs expected %d.",ptp->Transaction_ID, params->transaction_id-1);
		return PTP_ERROR_BADPARAM;
	}
	return ptp->Code;
}

/* memory data get/put handler */
typedef struct {
	unsigned char	*data;
	unsigned long	size, curoff;
} PTPMemHandlerPrivate;

static uint16_t
memory_getfunc(PTPParams* params, void* private,
	       unsigned long wantlen, unsigned char *data,
	       unsigned long *gotlen
) {
	PTPMemHandlerPrivate* priv = (PTPMemHandlerPrivate*)private;
	unsigned long tocopy = wantlen;

	if (priv->curoff + tocopy > priv->size)
		tocopy = priv->size - priv->curoff;
	ptp_debug(params,"ptp2/memory_getfunc : Get from memory.");
	memcpy (data, priv->data + priv->curoff, tocopy);
	priv->curoff += tocopy;
	*gotlen = tocopy;
	return PTP_RC_OK;
}

static uint16_t
memory_putfunc(PTPParams* params, void* private,
	       unsigned long sendlen, unsigned char *data,
	       unsigned long *putlen
) {
	
	PTPMemHandlerPrivate* priv = (PTPMemHandlerPrivate*)private;

	if (priv->curoff + sendlen > priv->size) 
	{
		priv->data = realloc (priv->data, priv->curoff+sendlen);
		priv->size = priv->curoff + sendlen;
	}
	ptp_debug(params,"ptp2/memory_putfunc : Write %d to memory.",sendlen);
	memcpy (priv->data + priv->curoff, data, sendlen);
	ptp_debug(params,"ptp2/memory_putfunc : Write to memory done.");
	priv->curoff += sendlen;
	*putlen = sendlen;
	return PTP_RC_OK;
}

/// init private struct for receiving data. 
static uint16_t
ptp_init_recv_memory_handler(PTPDataHandler *handler) {
	PTPMemHandlerPrivate* priv;
	priv = malloc (sizeof(PTPMemHandlerPrivate));
	handler->priv = priv;
	handler->getfunc = memory_getfunc;
	handler->putfunc = memory_putfunc;
	priv->data = NULL;
	priv->size = 0;
	priv->curoff = 0;
	return PTP_RC_OK;
}

///init private struct and put data in for sending data.data is still owned by caller.
static uint16_t
ptp_init_send_memory_handler(PTPDataHandler *handler,
	unsigned char *data, unsigned long len
) {
	PTPMemHandlerPrivate* priv;
	priv = malloc (sizeof(PTPMemHandlerPrivate));
	if (!priv)
		return PTP_RC_GeneralError;
	handler->priv = priv;
	handler->getfunc = memory_getfunc;
	handler->putfunc = memory_putfunc;
	priv->data = data;
	priv->size = len;
	priv->curoff = 0;
	return PTP_RC_OK;
}

// free private struct + data
static uint16_t
ptp_exit_send_memory_handler (PTPDataHandler *handler) {
	PTPMemHandlerPrivate* priv = (PTPMemHandlerPrivate*)handler->priv;
	// data is owned by caller
	free (priv);
	return PTP_RC_OK;
}

// hand over our internal data to caller
static uint16_t
ptp_exit_recv_memory_handler (PTPDataHandler *handler,
	unsigned char **data, unsigned long *size
) {
	PTPMemHandlerPrivate* priv = (PTPMemHandlerPrivate*)handler->priv;
	*data = priv->data;
	*size = priv->size;
	free (priv);
	return PTP_RC_OK;
}

// fd data get/put handler
typedef struct {
	int fd;
} PTPFDHandlerPrivate;

static uint16_t
fd_getfunc(PTPParams* params, void* private,
	       unsigned long wantlen, unsigned char *data,
	       unsigned long *gotlen
) {
	PTPFDHandlerPrivate* priv = (PTPFDHandlerPrivate*)private;
	int		got;

	got = read (priv->fd, data, wantlen);
	if (got != -1)
		*gotlen = got;
	else
		return PTP_RC_GeneralError;
	return PTP_RC_OK;
}

static uint16_t
fd_putfunc(PTPParams* params, void* private,
	       unsigned long sendlen, unsigned char *data,
	       unsigned long *putlen
) {
	int		written;
	PTPFDHandlerPrivate* priv = (PTPFDHandlerPrivate*)private;
	
	ptp_debug(params,"ptp2/fd_putfunc : Write data to file descriptor");
	written = write (priv->fd, data, sendlen);
	if (written != -1)
		*putlen = written;
	else
		return PTP_RC_GeneralError;
	return PTP_RC_OK;
}

static uint16_t
ptp_init_fd_handler(PTPDataHandler *handler, int fd) {
	
	PTPFDHandlerPrivate* priv;
	priv = malloc (sizeof(PTPFDHandlerPrivate));
	handler->priv = priv;
	handler->getfunc = fd_getfunc;
	handler->putfunc = fd_putfunc;
	priv->fd = fd;
	return PTP_RC_OK;
}

static uint16_t
ptp_exit_fd_handler (PTPDataHandler *handler) {
	PTPFDHandlerPrivate* priv = (PTPFDHandlerPrivate*)handler->priv;
	free (priv);
	return PTP_RC_OK;
}

// Old style transaction, based on memory
static uint16_t
ptp_transaction (PTPParams* params, PTPContainer* ptp, 
		uint16_t flags, unsigned int sendlen,
		unsigned char **data, unsigned int *recvlen
) {
	PTPDataHandler	handler;
	uint16_t	ret;

	ptp_debug(params,"ptp2/ptp_transaction : Init memory handler.");
	
	switch (flags & PTP_DP_DATA_MASK) 
	{
		case PTP_DP_SENDDATA:
			ptp_init_send_memory_handler (&handler, *data, sendlen);
			break;
		case PTP_DP_GETDATA:
			ptp_init_recv_memory_handler (&handler);
			break;
		default:break;
	}
	ret = ptp_transaction_new (params, ptp, flags, sendlen, &handler);
	ptp_debug(params,"ptp2/ptp_transaction : Exit memory handler.");
	switch (flags & PTP_DP_DATA_MASK) 
	{
		case PTP_DP_SENDDATA:
			ptp_exit_send_memory_handler (&handler);
			break;
		case PTP_DP_GETDATA: 
		{
			unsigned long len;
			ptp_exit_recv_memory_handler (&handler, data, &len);
			ptp_debug(params,"ptp2/ptp_transaction : Receive lenght = %d.",len);
			if (recvlen)
				*recvlen = len;
			break;
		}
		default:break;
	}
	ptp_debug(params,"ptp2/ptp_transaction : Transaction Done.");
	return ret;
}

/**
 * PTP operation functions
 *
 * all ptp_ functions should take integer parameters
 * in host byte order!
 **/


/**
 * ptp_getdeviceinfo:
 * params:	PTPParams*
 *
 * Gets device info dataset and fills deviceinfo structure.
 *
 * Return values: Some PTP_RC_* code.
 **/
uint16_t
ptp_getdeviceinfo (PTPParams* params, PTPDeviceInfo* deviceinfo)
{
	uint16_t 	ret;
	unsigned long	len;
	PTPContainer	ptp;
	unsigned char*	di=NULL;
	PTPDataHandler	handler;

	ptp_init_recv_memory_handler (&handler);
	PTP_CNT_INIT(ptp);
	ptp.Code=PTP_OC_GetDeviceInfo;
	ptp.Nparam=0;
	len=0;
	ret=ptp_transaction_new(params, &ptp, PTP_DP_GETDATA, 0, &handler);
	ptp_exit_recv_memory_handler (&handler, &di, &len);
	if (ret == PTP_RC_OK) ptp_unpack_DI(params, di, deviceinfo, len);
	free(di);
	return ret;
}


/**
 * ptp_opensession:
 * params:	PTPParams*
 * 		session			- session number 
 *
 * Establishes a new session.
 *
 * Return values: Some PTP_RC_* code.
 **/
uint16_t
ptp_opensession (PTPParams* params, uint32_t session)
{
	uint16_t ret;
	PTPContainer ptp;

	ptp_debug(params,"PTP: Opening session");

	/* SessonID field of the operation dataset should always
	   be set to 0 for OpenSession request! */
	params->session_id=0x00000000;
	/* TransactionID should be set to 0 also! */
	params->transaction_id=0x0000000;
	/* zero out response packet buffer */
	params->response_packet = NULL;
	params->response_packet_size = 0;
	/* no split headers */
	params->split_header_data = 0;

	
	PTP_CNT_INIT(ptp);
	ptp.Code=PTP_OC_OpenSession;
	ptp.Param1=session;
	ptp.Nparam=1;
	ret=ptp_transaction_new(params, &ptp, PTP_DP_NODATA, 0, NULL);
	/* now set the global session id to current session number */
	params->session_id=session;
	return ret;
}

/**
 * ptp_closesession:
 * params:	PTPParams*
 *
 * Closes session.
 *
 * Return values: Some PTP_RC_* code.
 **/
uint16_t
ptp_closesession (PTPParams* params)
{
	PTPContainer ptp;

	ptp_debug(params,"ptp2/ptp_closesession : Closing session");

	/* free any dangling response packet */
	if (params->response_packet_size > 0) {
		free(params->response_packet);
		params->response_packet = NULL;
		params->response_packet_size = 0;
	}
	PTP_CNT_INIT(ptp);
	ptp.Code=PTP_OC_CloseSession;
	ptp.Nparam=0;
	return ptp_transaction_new(params, &ptp, PTP_DP_NODATA, 0, NULL);
}

/**
 * ptp_free_params:
 * params:	PTPParams*
 *
 * Frees all data within the PTPParams struct.
 *
 * Return values: Some PTP_RC_* code.
 **/
void
ptp_free_params (PTPParams *params) 
{
	int i;
	ptp_debug(params,"ptp2/ptp_free_params : Free MTP Properties");
	for (i=0;i<params->nrofprops;i++) 
	{
		MTPProperties	*xpl = &params->props[i];
		ptp_debug(params,"ptp2/ptp_free_params : Property %d",i);
		if ((xpl->datatype == PTP_DTC_STR) && (xpl->propval.str))
		{
			ptp_debug(params,"ptp2/ptp_free_params : Free Property string %s",xpl->propval.str);
			free (xpl->propval.str);
		}
	}
	ptp_debug(params,"ptp2/ptp_free_params : Free properties");
	if (params->props) free (params->props);
	ptp_debug(params,"ptp2/ptp_free_params : Free canon flags");
	if (params->canon_flags) free (params->canon_flags);
	ptp_debug(params,"ptp2/ptp_free_params : Free cameraname");
	if (params->cameraname) free (params->cameraname);
	ptp_debug(params,"ptp2/ptp_free_params : Free wifi profiles");
	if (params->wifi_profiles) free (params->wifi_profiles);
	ptp_debug(params,"ptp2/ptp_free_params : Free handler");
	free (params->handles.Handler);
	ptp_debug(params,"ptp2/ptp_free_params : Free each object info");
	for (i=0;i<params->handles.n;i++)
		ptp_free_objectinfo (&params->objectinfo[i]);
	ptp_debug(params,"ptp2/ptp_free_params : Free global object info");
	free (params->objectinfo);
	ptp_debug(params,"ptp2/ptp_free_params : Free device info");
	ptp_free_DI (&params->deviceinfo);
}

/**
 * ptp_resetdevice:
 * params:	PTPParams*
 *		
 * Uses the built-in function to reset the device
 *
 * Return values: Some PTP_RC_* code.
 *
 */
uint16_t
ptp_resetdevice (PTPParams* params)
{
	PTPContainer ptp;

	PTP_CNT_INIT(ptp);
	ptp.Code=PTP_OC_ResetDevice;
	ptp.Nparam=0;

	return ptp_transaction_new(params, &ptp, PTP_DP_NODATA, 0, NULL);
}

/**
 * ptp_getststorageids:
 * params:	PTPParams*
 *
 * Gets array of StorageIDs and fills the storageids structure.
 *
 * Return values: Some PTP_RC_* code.
 **/
uint16_t
ptp_getstorageids (PTPParams* params, PTPStorageIDs* storageids)
{
	uint16_t ret;
	PTPContainer ptp;
	unsigned int len;
	unsigned char* sids=NULL;

	PTP_CNT_INIT(ptp);
	ptp.Code=PTP_OC_GetStorageIDs;
	ptp.Nparam=0;
	len=0;
	ret=ptp_transaction(params, &ptp, PTP_DP_GETDATA, 0, &sids, &len);
	if (ret == PTP_RC_OK) ptp_unpack_SIDs(params, sids, storageids, len);
	free(sids);
	return ret;
}

/**
 * ptp_getststorageinfo:
 * params:	PTPParams*
 *		storageid		- StorageID
 *
 * Gets StorageInfo dataset of desired storage and fills storageinfo
 * structure.
 *
 * Return values: Some PTP_RC_* code.
 **/
uint16_t
ptp_getstorageinfo (PTPParams* params, uint32_t storageid,
			PTPStorageInfo* storageinfo)
{
	uint16_t ret;
	PTPContainer ptp;
	unsigned char* si=NULL;
	unsigned int len;

	PTP_CNT_INIT(ptp);
	ptp.Code=PTP_OC_GetStorageInfo;
	ptp.Param1=storageid;
	ptp.Nparam=1;
	len=0;
	ret=ptp_transaction(params, &ptp, PTP_DP_GETDATA, 0, &si, &len);
	if (ret == PTP_RC_OK) ptp_unpack_SI(params, si, storageinfo, len);
	free(si);
	return ret;
}

/**
 * ptp_formatstore:
 * params:	PTPParams*
 *              storageid		- StorageID
 *
 * Formats the storage on the device.
 *
 * Return values: Some PTP_RC_* code.
 **/
uint16_t
ptp_formatstore (PTPParams* params, uint32_t storageid)
{
	PTPContainer ptp;

	PTP_CNT_INIT(ptp);
	ptp.Code=PTP_OC_FormatStore;
	ptp.Param1=storageid;
	ptp.Param2=PTP_FST_Undefined;
	ptp.Nparam=2;
	return ptp_transaction(params, &ptp, PTP_DP_NODATA, 0, NULL, NULL);
}

/**
 * ptp_getobjecthandles:
 * params:	PTPParams*
 *		storage			- StorageID
 *		objectformatcode	- ObjectFormatCode (optional)
 *		associationOH		- ObjectHandle of Association for
 *					  wich a list of children is desired
 *					  (optional)
 *		objecthandles		- pointer to structute
 *
 * Fills objecthandles with structure returned by device.
 *
 * Return values: Some PTP_RC_* code.
 **/
uint16_t
ptp_getobjecthandles (PTPParams* params, uint32_t storage,
			uint32_t objectformatcode, uint32_t associationOH,
			PTPObjectHandles* objecthandles)
{
	uint16_t ret;
	PTPContainer ptp;
	unsigned char* oh=NULL;
	unsigned int len;

	ptp_debug(params,"ptp2/ptp_getobjecthandles : Get Object Handles");

	PTP_CNT_INIT(ptp);
	ptp.Code=PTP_OC_GetObjectHandles;
	ptp.Param1=storage;
	ptp.Param2=objectformatcode;
	ptp.Param3=associationOH;
	ptp.Nparam=3;
	len=0;
	ret=ptp_transaction(params, &ptp, PTP_DP_GETDATA, 0, &oh, &len);
	if (ret == PTP_RC_OK) {
		ptp_unpack_OH(params, oh, objecthandles, len);
	} else {
		if (	(storage == 0xffffffff) &&
			(objectformatcode == 0) &&
			(associationOH == 0)
		) {
			/* When we query all object handles on all stores and
			 * get an error -> just handle it as "0 handles".
			 */
			objecthandles->Handler = NULL;
			objecthandles->n = 0;
			ret = PTP_RC_OK;
		}
	}
	free(oh);
	return ret;
}

/**
 * ptp_getnumobjects:
 * params:	PTPParams*
 *		storage			- StorageID
 *		objectformatcode	- ObjectFormatCode (optional)
 *		associationOH		- ObjectHandle of Association for
 *					  wich a list of children is desired
 *					  (optional)
 *		numobs			- pointer to uint32_t that takes number of objects
 *
 * Fills numobs with number of objects on device.
 *
 * Return values: Some PTP_RC_* code.
 **/
uint16_t
ptp_getnumobjects (PTPParams* params, uint32_t storage,
			uint32_t objectformatcode, uint32_t associationOH,
			uint32_t* numobs)
{
	uint16_t ret;
	PTPContainer ptp;
	int len;

	PTP_CNT_INIT(ptp);
	ptp.Code=PTP_OC_GetNumObjects;
	ptp.Param1=storage;
	ptp.Param2=objectformatcode;
	ptp.Param3=associationOH;
	ptp.Nparam=3;
	len=0;
	ret=ptp_transaction(params, &ptp, PTP_DP_NODATA, 0, NULL, NULL);
	if (ret == PTP_RC_OK) 
	{
		if (ptp.Nparam >= 1)
			*numobs = ptp.Param1;
		else
			ret = PTP_RC_GeneralError;
	}
	return ret;
}

/**
 * ptp_getobjectinfo:
 * params:	PTPParams*
 *		handle			- Object handle
 *		objectinfo		- pointer to objectinfo that is returned
 *
 * Get objectinfo structure for handle from device.
 *
 * Return values: Some PTP_RC_* code.
 **/
uint16_t
ptp_getobjectinfo (PTPParams* params, uint32_t handle,
			PTPObjectInfo* objectinfo)
{
	uint16_t ret;
	PTPContainer ptp;
	unsigned char* oi=NULL;
	unsigned int len;

	PTP_CNT_INIT(ptp);
	ptp.Code=PTP_OC_GetObjectInfo;
	ptp.Param1=handle;
	ptp.Nparam=1;
	len=0;
	ret=ptp_transaction(params, &ptp, PTP_DP_GETDATA, 0, &oi, &len);
	ptp_debug(params,"ptp2/ptp_getobjectinfo : Unpack the object information.");
	if (ret == PTP_RC_OK) 
		ptp_unpack_OI(params, oi, objectinfo, len);
	free(oi);
	return ret;
}

/**
 * ptp_getobject:
 * params:	PTPParams*
 *		handle			- Object handle
 *		object			- pointer to data area
 *
 * Get object 'handle' from device and store the data in newly
 * allocated 'object'.
 *
 * Return values: Some PTP_RC_* code.
 **/
uint16_t
ptp_getobject (PTPParams* params, uint32_t handle, unsigned char** object)
{
	PTPContainer ptp;
	unsigned int len;

	PTP_CNT_INIT(ptp);
	ptp.Code=PTP_OC_GetObject;
	ptp.Param1=handle;
	ptp.Nparam=1;
	len=0;
	return ptp_transaction(params, &ptp, PTP_DP_GETDATA, 0, object, &len);
}

/**
 * ptp_getobject_to_handler:
 * params:	PTPParams*
 *		handle			- Object handle
 *		PTPDataHandler*		- pointer datahandler
 *
 * Get object 'handle' from device and store the data in newly
 * allocated 'object'.
 *
 * Return values: Some PTP_RC_* code.
 **/
uint16_t
ptp_getobject_to_handler (PTPParams* params, uint32_t handle, PTPDataHandler *handler)
{
	PTPContainer ptp;

	PTP_CNT_INIT(ptp);
	ptp.Code=PTP_OC_GetObject;
	ptp.Param1=handle;
	ptp.Nparam=1;
	return ptp_transaction_new(params, &ptp, PTP_DP_GETDATA, 0, handler);
}

/**
 * ptp_getobject_tofd:
 * params:	PTPParams*
 *		handle			- Object handle
 *		fd                      - File descriptor to write() to
 *
 * Get object 'handle' from device and write the data to the 
 * given file descriptor.
 *
 * Return values: Some PTP_RC_* code.
 **/
uint16_t
ptp_getobject_tofd (PTPParams* params, uint32_t handle, int fd)
{
	PTPContainer	ptp;
	PTPDataHandler	handler;
	uint16_t	ret;

	ptp_init_fd_handler (&handler, fd);
	PTP_CNT_INIT(ptp);
	ptp.Code=PTP_OC_GetObject;
	ptp.Param1=handle;
	ptp.Nparam=1;
	ret = ptp_transaction_new(params, &ptp, PTP_DP_GETDATA, 0, &handler);
	ptp_exit_fd_handler (&handler);
	return ret;
}

/**
 * ptp_getpartialobject:
 * params:	PTPParams*
 *		handle			- Object handle
 *		offset			- Offset into object
 *		maxbytes		- Maximum of bytes to read
 *		object			- pointer to data area
 *
 * Get object 'handle' from device and store the data in newly
 * allocated 'object'. Start from offset and read at most maxbytes.
 *
 * Return values: Some PTP_RC_* code.
 **/
uint16_t
ptp_getpartialobject (PTPParams* params, uint32_t handle, uint32_t offset,
			uint32_t maxbytes, unsigned char** object)
{
	PTPContainer ptp;
	unsigned int len;

	PTP_CNT_INIT(ptp);
	ptp.Code=PTP_OC_GetPartialObject;
	ptp.Param1=handle;
	ptp.Param2=offset;
	ptp.Param3=maxbytes;
	ptp.Nparam=3;
	len=0;
	return ptp_transaction(params, &ptp, PTP_DP_GETDATA, 0, object, &len);
}

/**
 * ptp_getthumb:
 * params:	PTPParams*
 *		handle			- Object handle
 *		object			- pointer to data area
 *
 * Get thumb for object 'handle' from device and store the data in newly
 * allocated 'object'.
 *
 * Return values: Some PTP_RC_* code.
 **/
uint16_t
ptp_getthumb (PTPParams* params, uint32_t handle, unsigned char** object)
{
	PTPContainer ptp;
	unsigned int len;

	PTP_CNT_INIT(ptp);
	ptp.Code=PTP_OC_GetThumb;
	ptp.Param1=handle;
	ptp.Nparam=1;
	return ptp_transaction(params, &ptp, PTP_DP_GETDATA, 0, object, &len);
}

/**
 * ptp_deleteobject:
 * params:	PTPParams*
 *		handle			- object handle
 *		ofc			- object format code (optional)
 * 
 * Deletes desired objects.
 *
 * Return values: Some PTP_RC_* code.
 **/
uint16_t
ptp_deleteobject (PTPParams* params, uint32_t handle, uint32_t ofc)
{
	PTPContainer ptp;
	uint16_t ret;

	PTP_CNT_INIT(ptp);
	ptp.Code=PTP_OC_DeleteObject;
	ptp.Param1=handle;
	ptp.Param2=ofc;
	ptp.Nparam=2;
	ret = ptp_transaction(params, &ptp, PTP_DP_NODATA, 0, NULL, NULL);
	if (ret != PTP_RC_OK) {
		return ret;
	}
	/* If the object is cached and could be removed, cleanse cache. */
	ptp_remove_object_from_cache(params, handle);
	return PTP_RC_OK;
}

/**
 * ptp_sendobjectinfo:
 * params:	PTPParams*
 *		uint32_t* store		- destination StorageID on Responder
 *		uint32_t* parenthandle 	- Parent ObjectHandle on responder
 * 		uint32_t* handle	- see Return values
 *		PTPObjectInfo* objectinfo- ObjectInfo that is to be sent
 * 
 * Sends ObjectInfo of file that is to be sent via SendFileObject.
 *
 * Return values: Some PTP_RC_* code.
 * Upon success : uint32_t* store	- Responder StorageID in which
 *					  object will be stored
 *		  uint32_t* parenthandle- Responder Parent ObjectHandle
 *					  in which the object will be stored
 *		  uint32_t* handle	- Responder's reserved ObjectHandle
 *					  for the incoming object
 **/
uint16_t
ptp_sendobjectinfo (PTPParams* params, uint32_t* store, 
			uint32_t* parenthandle, uint32_t* handle,
			PTPObjectInfo* objectinfo)
{
	uint16_t ret;
	PTPContainer ptp;
	unsigned char* oidata=NULL;
	uint32_t size;

	PTP_CNT_INIT(ptp);
	ptp.Code=PTP_OC_SendObjectInfo;
	ptp.Param1=*store;
	ptp.Param2=*parenthandle;
	ptp.Nparam=2;
	
	size=ptp_pack_OI(params, objectinfo, &oidata);
	ret = ptp_transaction(params, &ptp, PTP_DP_SENDDATA, size, &oidata, NULL); 
	free(oidata);
	*store=ptp.Param1;
	*parenthandle=ptp.Param2;
	*handle=ptp.Param3; 
	return ret;
}

/**
 * ptp_sendobject:
 * params:	PTPParams*
 *		char*	object		- contains the object that is to be sent
 *		uint32_t size		- object size
 *		
 * Sends object to Responder.
 *
 * Return values: Some PTP_RC_* code.
 *
 */
uint16_t
ptp_sendobject (PTPParams* params, unsigned char* object, uint32_t size)
{
	PTPContainer ptp;

	PTP_CNT_INIT(ptp);
	ptp.Code=PTP_OC_SendObject;
	ptp.Nparam=0;

	return ptp_transaction(params, &ptp, PTP_DP_SENDDATA, size, &object, NULL);
}

/**
 * ptp_sendobject_from_handler:
 * params:	PTPParams*
 *		PTPDataHandler*         - File descriptor to read() object from
 *              uint32_t size           - File/object size
 *
 * Sends object from file descriptor by consecutive reads from this
 * descriptor.
 *
 * Return values: Some PTP_RC_* code.
 **/
uint16_t
ptp_sendobject_from_handler (PTPParams* params, PTPDataHandler *handler, uint32_t size)
{
	PTPContainer	ptp;

	PTP_CNT_INIT(ptp);
	ptp.Code=PTP_OC_SendObject;
	ptp.Nparam=0;
	return ptp_transaction_new(params, &ptp, PTP_DP_SENDDATA, size, handler);
}


/**
 * ptp_sendobject_fromfd:
 * params:	PTPParams*
 *		fd                      - File descriptor to read() object from
 *              uint32_t size           - File/object size
 *
 * Sends object from file descriptor by consecutive reads from this
 * descriptor.
 *
 * Return values: Some PTP_RC_* code.
 **/
uint16_t
ptp_sendobject_fromfd (PTPParams* params, int fd, uint32_t size)
{
	PTPContainer	ptp;
	PTPDataHandler	handler;
	uint16_t	ret;

	ptp_init_fd_handler (&handler, fd);
	PTP_CNT_INIT(ptp);
	ptp.Code=PTP_OC_SendObject;
	ptp.Nparam=0;
	ret = ptp_transaction_new(params, &ptp, PTP_DP_SENDDATA, size, &handler);
	ptp_exit_fd_handler (&handler);
	return ret;
}


/**
 * ptp_initiatecapture:
 * params:	PTPParams*
 *		storageid		- destination StorageID on Responder
 *		ofc			- object format code
 * 
 * Causes device to initiate the capture of one or more new data objects
 * according to its current device properties, storing the data into store
 * indicated by storageid. If storageid is 0x00000000, the object(s) will
 * be stored in a store that is determined by the capturing device.
 * The capturing of new data objects is an asynchronous operation.
 *
 * Return values: Some PTP_RC_* code.
 **/

uint16_t
ptp_initiatecapture (PTPParams* params, uint32_t storageid,
			uint32_t ofc)
{
	PTPContainer ptp;

	PTP_CNT_INIT(ptp);
	ptp.Code=PTP_OC_InitiateCapture;
	ptp.Param1=storageid;
	ptp.Param2=ofc;
	ptp.Nparam=2;
	return ptp_transaction(params, &ptp, PTP_DP_NODATA, 0, NULL, NULL);
}

uint16_t
ptp_getdevicepropdesc (PTPParams* params, uint16_t propcode, 
			PTPDevicePropDesc* devicepropertydesc)
{
	PTPContainer ptp;
	uint16_t ret;
	unsigned int len;
	unsigned char* dpd=NULL;

	PTP_CNT_INIT(ptp);
	ptp.Code=PTP_OC_GetDevicePropDesc;
	ptp.Param1=propcode;
	ptp.Nparam=1;
	len=0;
	ret=ptp_transaction(params, &ptp, PTP_DP_GETDATA, 0, &dpd, &len);
	if (ret == PTP_RC_OK) ptp_unpack_DPD(params, dpd, devicepropertydesc, len);
	free(dpd);
	return ret;
}


uint16_t
ptp_getdevicepropvalue (PTPParams* params, uint16_t propcode,
			PTPPropertyValue* value, uint16_t datatype)
{
	PTPContainer ptp;
	uint16_t ret;
	unsigned int len;
	int offset;
	unsigned char* dpv=NULL;


	PTP_CNT_INIT(ptp);
	ptp.Code=PTP_OC_GetDevicePropValue;
	ptp.Param1=propcode;
	ptp.Nparam=1;
	len=offset=0;
	ret=ptp_transaction(params, &ptp, PTP_DP_GETDATA, 0, &dpv, &len);
	if (ret == PTP_RC_OK) ptp_unpack_DPV(params, dpv, &offset, len, value, datatype);
	free(dpv);
	return ret;
}

uint16_t
ptp_setdevicepropvalue (PTPParams* params, uint16_t propcode,
			PTPPropertyValue *value, uint16_t datatype)
{
	PTPContainer ptp;
	uint16_t ret;
	uint32_t size;
	unsigned char* dpv=NULL;

	PTP_CNT_INIT(ptp);
	ptp.Code=PTP_OC_SetDevicePropValue;
	ptp.Param1=propcode;
	ptp.Nparam=1;
	size=ptp_pack_DPV(params, value, &dpv, datatype);
	ret=ptp_transaction(params, &ptp, PTP_DP_SENDDATA, size, &dpv, NULL);
	free(dpv);
	return ret;
}

/**
 * ptp_ek_sendfileobjectinfo:
 * params:	PTPParams*
 *		uint32_t* store		- destination StorageID on Responder
 *		uint32_t* parenthandle 	- Parent ObjectHandle on responder
 * 		uint32_t* handle	- see Return values
 *		PTPObjectInfo* objectinfo- ObjectInfo that is to be sent
 * 
 * Sends ObjectInfo of file that is to be sent via SendFileObject.
 *
 * Return values: Some PTP_RC_* code.
 * Upon success : uint32_t* store	- Responder StorageID in which
 *					  object will be stored
 *		  uint32_t* parenthandle- Responder Parent ObjectHandle
 *					  in which the object will be stored
 *		  uint32_t* handle	- Responder's reserved ObjectHandle
 *					  for the incoming object
 **/
uint16_t
ptp_ek_sendfileobjectinfo (PTPParams* params, uint32_t* store, 
			uint32_t* parenthandle, uint32_t* handle,
			PTPObjectInfo* objectinfo)
{
	uint16_t ret;
	PTPContainer ptp;
	unsigned char* oidata=NULL;
	uint32_t size;

	PTP_CNT_INIT(ptp);
	ptp.Code=PTP_OC_EK_SendFileObjectInfo;
	ptp.Param1=*store;
	ptp.Param2=*parenthandle;
	ptp.Nparam=2;
	
	size=ptp_pack_OI(params, objectinfo, &oidata);
	ret=ptp_transaction(params, &ptp, PTP_DP_SENDDATA, size, &oidata, NULL); 
	free(oidata);
	*store=ptp.Param1;
	*parenthandle=ptp.Param2;
	*handle=ptp.Param3; 
	return ret;
}

/**
 * ptp_ek_getserial:
 * params:	PTPParams*
 *		char**	serial		- contains the serial number of the camera
 *		uint32_t* size		- contains the string length
 *		
 * Gets the serial number from the device. (ptp serial)
 *
 * Return values: Some PTP_RC_* code.
 *
 */
uint16_t
ptp_ek_getserial (PTPParams* params, unsigned char **data, unsigned int *size)
{
	PTPContainer ptp;

	PTP_CNT_INIT(ptp);
	ptp.Code   = PTP_OC_EK_GetSerial;
	ptp.Nparam = 0;
	return ptp_transaction(params, &ptp, PTP_DP_GETDATA, 0, data, size); 
}

/**
 * ptp_ek_setserial:
 * params:	PTPParams*
 *		char*	serial		- contains the new serial number
 *		uint32_t size		- string length
 *		
 * Sets the serial number of the device. (ptp serial)
 *
 * Return values: Some PTP_RC_* code.
 *
 */
uint16_t
ptp_ek_setserial (PTPParams* params, unsigned char *data, unsigned int size)
{
	PTPContainer ptp;

	PTP_CNT_INIT(ptp);
	ptp.Code   = PTP_OC_EK_SetSerial;
	ptp.Nparam = 0;
	return ptp_transaction(params, &ptp, PTP_DP_SENDDATA, size, &data, NULL); 
}

/* unclear what it does yet */
uint16_t
ptp_ek_9007 (PTPParams* params, unsigned char **data, unsigned int *size)
{
	PTPContainer ptp;

	PTP_CNT_INIT(ptp);
	ptp.Code   = 0x9007;
	ptp.Nparam = 0;
	return ptp_transaction(params, &ptp, PTP_DP_GETDATA, 0, data, size); 
}

/* unclear what it does yet */
uint16_t
ptp_ek_9009 (PTPParams* params, uint32_t *p1, uint32_t *p2)
{
	PTPContainer	ptp;
	uint16_t	ret;

	PTP_CNT_INIT(ptp);
	ptp.Code   = 0x9009;
	ptp.Nparam = 0;
	ret = ptp_transaction(params, &ptp, PTP_DP_NODATA, 0, NULL, NULL); 
	*p1 = ptp.Param1;
	*p2 = ptp.Param2;
	return ret;
}

/* unclear yet, but I guess it returns the info from 9008 */
uint16_t
ptp_ek_900c (PTPParams* params, unsigned char **data, unsigned int *size)
{
	PTPContainer ptp;

	PTP_CNT_INIT(ptp);
	ptp.Code   = 0x900c;
	ptp.Nparam = 0;
	return ptp_transaction(params, &ptp, PTP_DP_GETDATA, 0, data, size); 
	/* returned data is 16bit,16bit,32bit,32bit */
}

/**
 * ptp_ek_settext:
 * params:	PTPParams*
 *		PTPEKTextParams*	- contains the texts to display.
 *		
 * Displays the specified texts on the TFT of the camera.
 *
 * Return values: Some PTP_RC_* code.
 *
 */
uint16_t
ptp_ek_settext (PTPParams* params, PTPEKTextParams *text)
{
	PTPContainer ptp;
	uint16_t ret;
	unsigned int size;
	unsigned char *data;

	PTP_CNT_INIT(ptp);
	ptp.Code   = PTP_OC_EK_SetText;
	ptp.Nparam = 0;
	if (0 == (size = ptp_pack_EK_text(params, text, &data)))
		return PTP_ERROR_BADPARAM;
	ret = ptp_transaction(params, &ptp, PTP_DP_SENDDATA, size, &data, NULL); 
	free(data);
	return ret;
}

/**
 * ptp_ek_sendfileobject:
 * params:	PTPParams*
 *		char*	object		- contains the object that is to be sent
 *		uint32_t size		- object size
 *		
 * Sends object to Responder.
 *
 * Return values: Some PTP_RC_* code.
 *
 */
uint16_t
ptp_ek_sendfileobject (PTPParams* params, unsigned char* object, uint32_t size)
{
	PTPContainer ptp;

	PTP_CNT_INIT(ptp);
	ptp.Code=PTP_OC_EK_SendFileObject;
	ptp.Nparam=0;

	return ptp_transaction(params, &ptp, PTP_DP_SENDDATA, size, &object, NULL);
}

/**
 * ptp_ek_sendfileobject_from_handler:
 * params:	PTPParams*
 *		PTPDataHandler*	handler	- contains the handler of the object that is to be sent
 *		uint32_t size		- object size
 *		
 * Sends object to Responder.
 *
 * Return values: Some PTP_RC_* code.
 *
 */
uint16_t
ptp_ek_sendfileobject_from_handler (PTPParams* params, PTPDataHandler*handler, uint32_t size)
{
	PTPContainer ptp;

	PTP_CNT_INIT(ptp);
	ptp.Code=PTP_OC_EK_SendFileObject;
	ptp.Nparam=0;
	return ptp_transaction_new(params, &ptp, PTP_DP_SENDDATA, size, handler);
}

/*************************************************************************
 *
 * Canon PTP extensions support
 *
 * (C) Nikolai Kopanygin 2003
 *
 *************************************************************************/


/**
 * ptp_canon_getpartialobjectinfo:
 * params:	PTPParams*
 *		uint32_t handle		- ObjectHandle
 *		uint32_t p2 		- Not fully understood parameter
 *					  0 - returns full size
 *					  1 - returns thumbnail size (or EXIF?)
 * 
 * Gets form the responder the size of the specified object.
 *
 * Return values: Some PTP_RC_* code.
 * Upon success : uint32_t* size	- The object size
 *		  uint32_t* rp2		- Still unknown return parameter
 *                                        (perhaps upper 32bit of size)
 *
 *
 **/
uint16_t
ptp_canon_getpartialobjectinfo (PTPParams* params, uint32_t handle, uint32_t p2, 
			uint32_t* size, uint32_t* rp2) 
{
	uint16_t ret;
	PTPContainer ptp;
	
	PTP_CNT_INIT(ptp);
	ptp.Code=PTP_OC_CANON_GetPartialObjectInfo;
	ptp.Param1=handle;
	ptp.Param2=p2;
	ptp.Nparam=2;
	ret=ptp_transaction(params, &ptp, PTP_DP_NODATA, 0, NULL, NULL);
	*size=ptp.Param1;
	*rp2=ptp.Param2;
	return ret;
}

/**
 * ptp_canon_get_mac_address:
 * params:	PTPParams*
 *					  value 0 works.
 * Gets the MAC address of the wireless transmitter.
 *
 * Return values: Some PTP_RC_* code.
 * Upon success : unsigned char* mac	- The MAC address
 *
 **/
uint16_t
ptp_canon_get_mac_address (PTPParams* params, unsigned char **mac)
{
	PTPContainer ptp;
	unsigned int size = 0;

	PTP_CNT_INIT(ptp);
	ptp.Code=PTP_OC_CANON_GetMACAddress;
	ptp.Nparam=0;
	*mac = NULL;
	return ptp_transaction(params, &ptp, PTP_DP_GETDATA, 0, mac, &size);
}

/**
 * ptp_canon_get_directory:
 * params:	PTPParams*

 * Gets the full directory of the camera.
 *
 * Return values: Some PTP_RC_* code.
 * Upon success : PTPObjectHandles        *handles	- filled out with handles
 * 		  PTPObjectInfo           **oinfos	- allocated array of PTP Object Infos
 * 		  uint32_t                **flags	- allocated array of CANON Flags
 *
 **/
uint16_t
ptp_canon_get_directory (PTPParams* params,
	PTPObjectHandles	*handles,
	PTPObjectInfo		**oinfos,	/* size(handles->n) */
	uint32_t		**flags		/* size(handles->n) */
) {
	PTPContainer	ptp;
	unsigned char	*dir = NULL;
	unsigned int	size = 0;
	uint16_t	ret;

	PTP_CNT_INIT(ptp);
	ptp.Code=PTP_OC_CANON_GetDirectory;
	ptp.Nparam=0;
	ret = ptp_transaction(params, &ptp, PTP_DP_GETDATA, 0, &dir, &size);
	if (ret != PTP_RC_OK)
		return ret;
	ret = ptp_unpack_canon_directory(params, dir, ptp.Param1, handles, oinfos, flags);
	free (dir);
	return ret;
}

/**
 * ptp_canon_setobjectarchive:
 *
 * params:	PTPParams*
 *		uint32_t	objectid
 *		uint32_t	flags
 *
 * Return values: Some PTP_RC_* code.
 *
 **/
uint16_t
ptp_canon_setobjectarchive (PTPParams* params, uint32_t oid, uint32_t flags)
{
	PTPContainer ptp;

	PTP_CNT_INIT(ptp);
	ptp.Code=PTP_OC_CANON_SetObjectArchive;
	ptp.Nparam=2;
	ptp.Param1=oid;
	ptp.Param2=flags;
	return ptp_transaction(params, &ptp, PTP_DP_NODATA, 0, NULL, NULL);
}


/**
 * ptp_canon_startshootingmode:
 * params:	PTPParams*
 * 
 * Starts shooting session. It emits a StorageInfoChanged
 * event via the interrupt pipe and pushes the StorageInfoChanged
 * and CANON_CameraModeChange events onto the event stack
 * (see operation PTP_OC_CANON_CheckEvent).
 *
 * Return values: Some PTP_RC_* code.
 *
 **/
uint16_t
ptp_canon_startshootingmode (PTPParams* params)
{
	PTPContainer ptp;
	
	PTP_CNT_INIT(ptp);
	ptp.Code=PTP_OC_CANON_InitiateReleaseControl;
	ptp.Nparam=0;
	return ptp_transaction(params, &ptp, PTP_DP_NODATA, 0, NULL, NULL);
}

/**
 * ptp_canon_gettreeinfo:
 * params:	PTPParams*
 *              uint32_t *out
 * 
 * Switches the camera display to on and lets the user
 * select what to transfer. Sends a 0xc011 event when started 
 * and 0xc013 if direct transfer aborted.
 *
 * Return values: Some PTP_RC_* code.
 *
 **/
uint16_t
ptp_canon_gettreeinfo (PTPParams* params, uint32_t *out)
{
	PTPContainer ptp;
	uint16_t ret;

	PTP_CNT_INIT(ptp);
	ptp.Code   = PTP_OC_CANON_GetTreeInfo;
	ptp.Nparam = 1;
	ptp.Param1 = 0xf;
	ret = ptp_transaction(params, &ptp, PTP_DP_NODATA, 0, NULL, NULL);
	if ((ret == PTP_RC_OK) && (ptp.Nparam>0))
		*out = ptp.Param1;
	return ret;
}

/**
 * ptp_canon_getpairinginfo:
 * params:	PTPParams*
 *              int nr
 * 
 * Get the pairing information.
 *
 * Return values: Some PTP_RC_* code.
 *
 **/
uint16_t
ptp_canon_getpairinginfo (PTPParams* params, uint32_t nr, unsigned char **data, unsigned int *size)
{
	PTPContainer ptp;
	uint16_t ret;
	
	PTP_CNT_INIT(ptp);
	ptp.Code   = PTP_OC_CANON_GetPairingInfo;
	ptp.Nparam = 1;
	ptp.Param1 = nr;
	*data = NULL;
	*size = 0;
	ret = ptp_transaction(params, &ptp, PTP_DP_GETDATA, 0, data, size);
	if (ret != PTP_RC_OK)
		return ret;
	return PTP_RC_OK;
}

/**
 * ptp_canon_get_target_handles:
 * params:	PTPParams*
 *              PTPCanon_directtransfer_entry **out
 *              unsigned int *outsize
 * 
 * Retrieves direct transfer entries specifying the images to transfer
 * from the camera (to be retrieved after 0xc011 event).
 *
 * Return values: Some PTP_RC_* code.
 *
 **/
uint16_t
ptp_canon_gettreesize (PTPParams* params,
	PTPCanon_directtransfer_entry **entries, unsigned int *cnt)
{
	PTPContainer ptp;
	uint16_t ret;
	unsigned char *out = NULL, *cur;
	int i;
	unsigned int size;
	
	PTP_CNT_INIT(ptp);
	ptp.Code   = PTP_OC_CANON_GetTreeSize;
	ptp.Nparam = 0;
	ret = ptp_transaction(params, &ptp, PTP_DP_GETDATA, 0, &out, &size);
	if (ret != PTP_RC_OK)
		return ret;
	*cnt = dtoh32a(out);
	*entries = malloc(sizeof(PTPCanon_directtransfer_entry)*(*cnt));
	cur = out+4;
	for (i=0;i<*cnt;i++) {
		unsigned char len;
		(*entries)[i].oid = dtoh32a(cur);
		(*entries)[i].str = ptp_unpack_string(params, cur, 4, &len);
		cur += 4+(cur[4]*2+1);
	}
	free (out);
	return PTP_RC_OK;
}
/**
 * ptp_canon_endshootingmode:
 * params:	PTPParams*
 * 
 * This operation is observed after pressing the Disconnect 
 * button on the Remote Capture app. It emits a StorageInfoChanged 
 * event via the interrupt pipe and pushes the StorageInfoChanged
 * and CANON_CameraModeChange events onto the event stack
 * (see operation PTP_OC_CANON_CheckEvent).
 *
 * Return values: Some PTP_RC_* code.
 *
 **/
uint16_t
ptp_canon_endshootingmode (PTPParams* params)
{
	PTPContainer ptp;
	
	PTP_CNT_INIT(ptp);
	ptp.Code=PTP_OC_CANON_TerminateReleaseControl;
	ptp.Nparam=0;
	return ptp_transaction(params, &ptp, PTP_DP_NODATA, 0, NULL, NULL);
}

/**
 * ptp_canon_viewfinderon:
 * params:	PTPParams*
 * 
 * Prior to start reading viewfinder images, one  must call this operation.
 * Supposedly, this operation affects the value of the CANON_ViewfinderMode
 * property.
 *
 * Return values: Some PTP_RC_* code.
 *
 **/
uint16_t
ptp_canon_viewfinderon (PTPParams* params)
{
	PTPContainer ptp;
	
	PTP_CNT_INIT(ptp);
	ptp.Code=PTP_OC_CANON_ViewfinderOn;
	ptp.Nparam=0;
	return ptp_transaction(params, &ptp, PTP_DP_NODATA, 0, NULL, NULL);
}

/**
 * ptp_canon_viewfinderoff:
 * params:	PTPParams*
 * 
 * Before changing the shooting mode, or when one doesn't need to read
 * viewfinder images any more, one must call this operation.
 * Supposedly, this operation affects the value of the CANON_ViewfinderMode
 * property.
 *
 * Return values: Some PTP_RC_* code.
 *
 **/
uint16_t
ptp_canon_viewfinderoff (PTPParams* params)
{
	PTPContainer ptp;
	
	PTP_CNT_INIT(ptp);
	ptp.Code=PTP_OC_CANON_ViewfinderOff;
	ptp.Nparam=0;
	return ptp_transaction(params, &ptp, PTP_DP_NODATA, 0, NULL, NULL);
}

/**
 * ptp_canon_aeafawb:
 * params:	PTPParams*
 * 		uint32_t p1 	- Yet unknown parameter,
 * 				  value 7 works
 * 
 * Called AeAfAwb (auto exposure, focus, white balance)
 *
 * Return values: Some PTP_RC_* code.
 *
 **/
uint16_t
ptp_canon_aeafawb (PTPParams* params, uint32_t p1)
{
	PTPContainer ptp;
	
	PTP_CNT_INIT(ptp);
	ptp.Code=PTP_OC_CANON_DoAeAfAwb;
	ptp.Param1=p1;
	ptp.Nparam=1;
	return ptp_transaction(params, &ptp, PTP_DP_NODATA, 0, NULL, NULL);
}


/**
 * ptp_canon_checkevent:
 * params:	PTPParams*
 * 
 * The camera has a FIFO stack, in which it accumulates events.
 * Partially these events are communicated also via the USB interrupt pipe
 * according to the PTP USB specification, partially not.
 * This operation returns from the device a block of data, empty,
 * if the event stack is empty, or filled with an event's data otherwise.
 * The event is removed from the stack in the latter case.
 * The Remote Capture app sends this command to the camera all the time
 * of connection, filling with it the gaps between other operations. 
 *
 * Return values: Some PTP_RC_* code.
 * Upon success : PTPUSBEventContainer* event	- is filled with the event data
 *						  if any
 *                int *isevent			- returns 1 in case of event
 *						  or 0 otherwise
 **/
uint16_t
ptp_canon_checkevent (PTPParams* params, PTPUSBEventContainer* event, int* isevent)
{
	uint16_t ret;
	PTPContainer ptp;
	unsigned char *evdata = NULL;
	unsigned int len;
	
	*isevent=0;
	PTP_CNT_INIT(ptp);
	ptp.Code=PTP_OC_CANON_CheckEvent;
	ptp.Nparam=0;
	len=0;
	ret=ptp_transaction(params, &ptp, PTP_DP_GETDATA, 0, &evdata, &len);
	if (evdata!=NULL) {
		if (ret == PTP_RC_OK) {
        		ptp_unpack_EC(params, evdata, event, len);
    			*isevent=1;
        	}
		free(evdata);
	}
	return ret;
}


/**
 * ptp_canon_focuslock:
 *
 * This operation locks the focus. It is followed by the CANON_GetChanges(?)
 * operation in the log. 
 * It affects the CANON_MacroMode property. 
 *
 * params:	PTPParams*
 *
 * Return values: Some PTP_RC_* code.
 *
 **/
uint16_t
ptp_canon_focuslock (PTPParams* params)
{
	PTPContainer ptp;
	
	PTP_CNT_INIT(ptp);
	ptp.Code=PTP_OC_CANON_FocusLock;
	ptp.Nparam=0;
	return ptp_transaction(params, &ptp, PTP_DP_NODATA, 0, NULL, NULL);
}

/**
 * ptp_canon_focusunlock:
 *
 * This operation unlocks the focus. It is followed by the CANON_GetChanges(?)
 * operation in the log. 
 * It sets the CANON_MacroMode property value to 1 (where it occurs in the log).
 * 
 * params:	PTPParams*
 *
 * Return values: Some PTP_RC_* code.
 *
 **/
uint16_t
ptp_canon_focusunlock (PTPParams* params)
{
	PTPContainer ptp;
	
	PTP_CNT_INIT(ptp);
	ptp.Code=PTP_OC_CANON_FocusUnlock;
	ptp.Nparam=0;
	return ptp_transaction(params, &ptp, PTP_DP_NODATA, 0, NULL, NULL);
}

/**
 * ptp_canon_keepdeviceon:
 *
 * This operation sends a "ping" style message to the camera.
 * 
 * params:	PTPParams*
 *
 * Return values: Some PTP_RC_* code.
 *
 **/
uint16_t
ptp_canon_keepdeviceon (PTPParams* params)
{
	PTPContainer ptp;
	
	PTP_CNT_INIT(ptp);
	ptp.Code=PTP_OC_CANON_KeepDeviceOn;
	ptp.Nparam=0;
	return ptp_transaction(params, &ptp, PTP_DP_NODATA, 0, NULL, NULL);
}

/**
 * ptp_canon_initiatecaptureinmemory:
 * 
 * This operation starts the image capture according to the current camera
 * settings. When the capture has happened, the camera emits a CaptureComplete
 * event via the interrupt pipe and pushes the CANON_RequestObjectTransfer,
 * CANON_DeviceInfoChanged and CaptureComplete events onto the event stack
 * (see operation CANON_CheckEvent). From the CANON_RequestObjectTransfer
 * event's parameter one can learn the just captured image's ObjectHandle.
 * The image is stored in the camera's own RAM.
 * On the next capture the image will be overwritten!
 *
 * params:	PTPParams*
 *
 * Return values: Some PTP_RC_* code.
 *
 **/
uint16_t
ptp_canon_initiatecaptureinmemory (PTPParams* params)
{
	PTPContainer ptp;
	
	PTP_CNT_INIT(ptp);
	ptp.Code=PTP_OC_CANON_InitiateCaptureInMemory;
	ptp.Nparam=0;
	return ptp_transaction(params, &ptp, PTP_DP_NODATA, 0, NULL, NULL);
}

/**
 * ptp_canon_eos_capture:
 * 
 * This starts a EOS400D style capture. You have to use the
 * 0x9116 command to poll for its completion.
 * The image is saved on the CF Card currently.
 *
 * params:	PTPParams*
 *
 * Return values: Some PTP_RC_* code.
 *
 **/
uint16_t
ptp_canon_eos_capture (PTPParams* params)
{
	PTPContainer ptp;
	
	PTP_CNT_INIT(ptp);
	ptp.Code=PTP_OC_CANON_EOS_RemoteRelease;
	ptp.Nparam=0;
	return ptp_transaction(params, &ptp, PTP_DP_NODATA, 0, NULL, NULL);
}

/**
 * ptp_canon_eos_getevent:
 * 
 * This retrieves configuration status/updates/changes
 * on EOS cameras. It reads a datablock which has a list of variable
 * sized structures.
 *
 * params:	PTPParams*
 *
 * Return values: Some PTP_RC_* code.
 *
 **/
uint16_t
ptp_canon_eos_getevent (PTPParams* params, PTPCanon_changes_entry **entries, int *nrofentries)
{
	PTPContainer ptp;
	uint16_t	ret;
	unsigned int 	size = 0;
	unsigned char	*data = NULL;

	*nrofentries = 0;
	*entries = NULL;
	PTP_CNT_INIT(ptp);
	ptp.Code = PTP_OC_CANON_EOS_GetEvent;
	ptp.Nparam = 0;
	ret = ptp_transaction(params, &ptp, PTP_DP_GETDATA, 0, &data, &size);
	if (ret != PTP_RC_OK) return ret;
        *nrofentries = ptp_unpack_CANON_changes(params,data,size,entries);
	return PTP_RC_OK;
}

uint16_t
ptp_canon_eos_getdevicepropdesc (PTPParams* params, uint16_t propcode,
	PTPDevicePropDesc *dpd)
{
	int i;

	for (i=0;i<params->nrofcanon_props;i++)
		if (params->canon_props[i].proptype == propcode)
			break;
	if (params->nrofcanon_props == i)
		return PTP_RC_Undefined;
	memcpy (dpd, &params->canon_props[i].dpd, sizeof (*dpd));
	if (dpd->FormFlag == PTP_DPFF_Enumeration) {
		/* need to duplicate the Enumeration alloc */
		dpd->FORM.Enum.SupportedValue = malloc (sizeof (PTPPropertyValue)*dpd->FORM.Enum.NumberOfValues);
		memcpy (dpd->FORM.Enum.SupportedValue,
			params->canon_props[i].dpd.FORM.Enum.SupportedValue,
			sizeof (PTPPropertyValue)*dpd->FORM.Enum.NumberOfValues
		);
		/* FIXME: duplicate strings if type is STR. */
	}
	return PTP_RC_OK;
}


uint16_t
ptp_canon_eos_getstorageids (PTPParams* params, PTPStorageIDs* storageids)
{
	PTPContainer	ptp;
	unsigned int	len = 0;
	uint16_t	ret;
	unsigned char*	sids=NULL;
	
	PTP_CNT_INIT(ptp);
	ptp.Code 	= PTP_OC_CANON_EOS_GetStorageIDs;
	ptp.Nparam	= 0;
	ret = ptp_transaction(params, &ptp, PTP_DP_GETDATA, 0, &sids, &len);
	if (ret == PTP_RC_OK) ptp_unpack_SIDs(params, sids, storageids, len);
	free(sids);
	return ret;
}

uint16_t
ptp_canon_eos_getstorageinfo (PTPParams* params, uint32_t p1)
{
	PTPContainer ptp;
	unsigned char	*data = NULL;
	unsigned int	size = 0;
	uint16_t	ret;
	
	PTP_CNT_INIT(ptp);
	ptp.Code 	= PTP_OC_CANON_EOS_GetStorageInfo;
	ptp.Nparam	= 1;
	ptp.Param1	= p1;
	ret = ptp_transaction(params, &ptp, PTP_DP_GETDATA, 0, &data, &size);
	/* FIXME: do stuff with data */
	return ret;
}

/**
 * ptp_canon_eos_getpartialobject:
 * 
 * This retrieves a part of an PTP object which you specify as object id.
 * The id originates from 0x9116 call.
 * After finishing it, we seem to need to call ptp_canon_eos_enddirecttransfer.
 *
 * params:	PTPParams*
 * 		oid		Object ID
 * 		offset		The offset where to start the data transfer 
 *		xsize		Size in bytes of the transfer to do
 *		data		Pointer that receives the malloc()ed memory of the transfer.
 *
 * Return values: Some PTP_RC_* code.
 *
 */
uint16_t
ptp_canon_eos_getpartialobject (PTPParams* params, uint32_t oid, uint32_t offset, uint32_t xsize, unsigned char**data)
{
	PTPContainer	ptp;
	unsigned int	size = 0;

	*data = NULL;
	PTP_CNT_INIT(ptp);
	ptp.Code 	= PTP_OC_CANON_EOS_GetPartialObject;
	ptp.Nparam	= 3;
	ptp.Param1	= oid;
	ptp.Param2	= offset;
	ptp.Param3	= xsize;
	return ptp_transaction(params, &ptp, PTP_DP_GETDATA, 0, data, &size);
}

/**
 * ptp_canon_eos_transfercomplete:
 * 
 * This ends a direct object transfer from an EOS camera.
 *
 * params:	PTPParams*
 * 		oid		Object ID
 *
 * Return values: Some PTP_RC_* code.
 *
 */
uint16_t
ptp_canon_eos_transfercomplete (PTPParams* params, uint32_t oid)
{
	PTPContainer	ptp;

	PTP_CNT_INIT(ptp);
	ptp.Code 	= PTP_OC_CANON_EOS_TransferComplete;
	ptp.Nparam	= 1;
	ptp.Param1	= oid;
	return ptp_transaction(params, &ptp, PTP_DP_NODATA, 0, NULL, NULL);
}

uint16_t
ptp_canon_eos_setdevicepropvalueex (PTPParams* params, unsigned char* data, unsigned int size)
{
	PTPContainer	ptp;

	PTP_CNT_INIT(ptp);
	ptp.Code 	= PTP_OC_CANON_EOS_SetDevicePropValueEx;
	ptp.Nparam	= 0;
	return ptp_transaction(params, &ptp, PTP_DP_SENDDATA, size, &data, NULL);
}

uint16_t
ptp_canon_eos_setdevicepropvalue (PTPParams* params,
	uint16_t propcode, PTPPropertyValue *value, uint16_t datatype
) {
	PTPContainer	ptp;
	uint16_t	ret;
	int 		i;
	unsigned char	*data;
	unsigned int	size;

	PTP_CNT_INIT(ptp);
	ptp.Code 	= PTP_OC_CANON_EOS_SetDevicePropValueEx;
	ptp.Nparam	= 0;
	for (i=0;i<params->nrofcanon_props;i++)
		if (params->canon_props[i].proptype == propcode)
			break;
	if (params->nrofcanon_props == i)
		return PTP_RC_Undefined;
	if (datatype != PTP_DTC_STR) {
		data = calloc(sizeof(uint32_t),3);
		size = sizeof(uint32_t)*3;
	} else {
		 /* FIXME! */
		return PTP_RC_Undefined;
	}
	htod32a(&data[0], size);
	htod32a(&data[4], propcode);
	switch (datatype) {
	case PTP_DTC_UINT8:
		/*fprintf (stderr, "%x -> %d\n", propcode, value->u8);*/
		htod8a(&data[8], value->u8);
		params->canon_props[i].dpd.CurrentValue.u8 = value->u8;
		break;
	case PTP_DTC_UINT16:
		/*fprintf (stderr, "%x -> %d\n", propcode, value->u16);*/
		htod16a(&data[8], value->u16);
		params->canon_props[i].dpd.CurrentValue.u16 = value->u16;
		break;
	case PTP_DTC_UINT32:
		/*fprintf (stderr, "%x -> %d\n", propcode, value->u32);*/
		htod32a(&data[8], value->u32);
		params->canon_props[i].dpd.CurrentValue.u32 = value->u32;
		break;
	}
	ret = ptp_transaction(params, &ptp, PTP_DP_SENDDATA, size, &data, NULL);
	free (data);
	return ret;
}

uint16_t
ptp_canon_eos_pchddcapacity (PTPParams* params, uint32_t p1, uint32_t p2, uint32_t p3)
{
	PTPContainer	ptp;

	PTP_CNT_INIT(ptp);
	ptp.Code 	= PTP_OC_CANON_EOS_PCHDDCapacity;
	ptp.Nparam	= 3;
	ptp.Param1	= p1;
	ptp.Param2	= p2;
	ptp.Param3	= p3;
	return ptp_transaction(params, &ptp, PTP_DP_NODATA, 0, NULL, NULL);
}


uint16_t
ptp_canon_eos_setremotemode (PTPParams* params, uint32_t p1)
{
	PTPContainer	ptp;

	PTP_CNT_INIT(ptp);
	ptp.Code 	= PTP_OC_CANON_EOS_SetRemoteMode;
	ptp.Nparam	= 1;
	ptp.Param1	= p1;
	return ptp_transaction(params, &ptp, PTP_DP_NODATA, 0, NULL, NULL);
}


uint16_t
ptp_canon_eos_seteventmode (PTPParams* params, uint32_t p1)
{
	PTPContainer	ptp;

	PTP_CNT_INIT(ptp);
	ptp.Code 	= PTP_OC_CANON_EOS_SetEventMode;
	ptp.Nparam	= 1;
	ptp.Param1	= p1;
	return ptp_transaction(params, &ptp, PTP_DP_NODATA, 0, NULL, NULL);
}


uint16_t
ptp_canon_9012 (PTPParams* params)
{
	PTPContainer ptp;
	
	PTP_CNT_INIT(ptp);
	ptp.Code=0x9012;
	ptp.Nparam=0;
	return ptp_transaction(params, &ptp, PTP_DP_NODATA, 0, NULL, NULL);
}

/**
 * ptp_canon_getpartialobject:
 *
 * This operation is used to read from the device a data 
 * block of an object from a specified offset.
 *
 * params:	PTPParams*
 *      uint32_t handle - the handle of the requested object
 *      uint32_t offset - the offset in bytes from the beginning of the object
 *      uint32_t size - the requested size of data block to read
 *      uint32_t pos - 1 for the first block, 2 - for a block in the middle,
 *                  3 - for the last block
 *
 * Return values: Some PTP_RC_* code.
 *      char **block - the pointer to the block of data read
 *      uint32_t* readnum - the number of bytes read
 *
 **/
uint16_t
ptp_canon_getpartialobject (PTPParams* params, uint32_t handle, 
				uint32_t offset, uint32_t size,
				uint32_t pos, unsigned char** block, 
				uint32_t* readnum)
{
	uint16_t ret;
	PTPContainer ptp;
	unsigned char *data=NULL;
	unsigned int len;
	
	PTP_CNT_INIT(ptp);
	ptp.Code=PTP_OC_CANON_GetPartialObjectEx;
	ptp.Param1=handle;
	ptp.Param2=offset;
	ptp.Param3=size;
	ptp.Param4=pos;
	ptp.Nparam=4;
	len=0;
	ret=ptp_transaction(params, &ptp, PTP_DP_GETDATA, 0, &data, &len);
	if (ret==PTP_RC_OK) {
		*block=data;
		*readnum=ptp.Param1;
	}
	return ret;
}

/**
 * ptp_canon_getviewfinderimage:
 *
 * This operation can be used to read the image which is currently
 * in the camera's viewfinder. The image size is 320x240, format is JPEG.
 * Of course, prior to calling this operation, one must turn the viewfinder
 * on with the CANON_ViewfinderOn command.
 * Invoking this operation many times, one can get live video from the camera!
 * 
 * params:	PTPParams*
 * 
 * Return values: Some PTP_RC_* code.
 *      char **image - the pointer to the read image
 *      unit32_t *size - the size of the image in bytes
 *
 **/
uint16_t
ptp_canon_getviewfinderimage (PTPParams* params, unsigned char** image, uint32_t* size)
{
	uint16_t ret;
	PTPContainer ptp;
	unsigned int len;
	
	PTP_CNT_INIT(ptp);
	ptp.Code=PTP_OC_CANON_GetViewfinderImage;
	ptp.Nparam=0;
	ret=ptp_transaction(params, &ptp, PTP_DP_GETDATA, 0, image, &len);
	if (ret==PTP_RC_OK) *size=ptp.Param1;
	return ret;
}

/**
 * ptp_canon_getchanges:
 *
 * This is an interesting operation, about the effect of which I am not sure.
 * This command is called every time when a device property has been changed 
 * with the SetDevicePropValue operation, and after some other operations.
 * This operation reads the array of Device Properties which have been changed
 * by the previous operation.
 * Probably, this operation is even required to make those changes work.
 *
 * params:	PTPParams*
 * 
 * Return values: Some PTP_RC_* code.
 *      uint16_t** props - the pointer to the array of changed properties
 *      uint32_t* propnum - the number of elements in the *props array
 *
 **/
uint16_t
ptp_canon_getchanges (PTPParams* params, uint16_t** props, uint32_t* propnum)
{
	uint16_t ret;
	PTPContainer ptp;
	unsigned char* data=NULL;
	unsigned int len;
	
	PTP_CNT_INIT(ptp);
	ptp.Code=PTP_OC_CANON_GetChanges;
	ptp.Nparam=0;
	len=0;
	ret=ptp_transaction(params, &ptp, PTP_DP_GETDATA, 0, &data, &len);
	if (ret == PTP_RC_OK)
        	*propnum=ptp_unpack_uint16_t_array(params,data,0,props);
	free(data);
	return ret;
}

/**
 * ptp_canon_getobjectinfo:
 *
 * This command reads a specified object's record in a device's filesystem,
 * or the records of all objects belonging to a specified folder (association).
 *  
 * params:	PTPParams*
 *      uint32_t store - StorageID,
 *      uint32_t p2 - Yet unknown (0 value works OK)
 *      uint32_t parent - Parent Object Handle
 *                      # If Parent Object Handle is 0xffffffff, 
 *                      # the Parent Object is the top level folder.
 *      uint32_t handle - Object Handle
 *                      # If Object Handle is 0, the records of all objects 
 *                      # belonging to the Parent Object are read.
 *                      # If Object Handle is not 0, only the record of this 
 *                      # Object is read.
 *
 * Return values: Some PTP_RC_* code.
 *      PTPCANONFolderEntry** entries - the pointer to the folder entry array
 *      uint32_t* entnum - the number of elements of the array
 *
 **/
uint16_t
ptp_canon_getobjectinfo (PTPParams* params, uint32_t store, uint32_t p2, 
			    uint32_t parent, uint32_t handle, 
			    PTPCANONFolderEntry** entries, uint32_t* entnum)
{
	uint16_t ret;
	PTPContainer ptp;
	unsigned char *data = NULL;
	unsigned int len;
	
	PTP_CNT_INIT(ptp);
	ptp.Code=PTP_OC_CANON_GetObjectInfoEx;
	ptp.Param1=store;
	ptp.Param2=p2;
	ptp.Param3=parent;
	ptp.Param4=handle;
	ptp.Nparam=4;
	len=0;
	ret=ptp_transaction(params, &ptp, PTP_DP_GETDATA, 0, &data, &len);
	if (ret == PTP_RC_OK) {
		int i;
		*entnum=ptp.Param1;
		*entries=calloc(*entnum, sizeof(PTPCANONFolderEntry));
		if (*entries!=NULL) {
			for(i=0; i<(*entnum); i++)
				ptp_unpack_Canon_FE(params,
					data+i*PTP_CANON_FolderEntryLen,
					&((*entries)[i]) );
		} else {
			ret=PTP_ERROR_IO; /* Cannot allocate memory */
		}
	}
	free(data);
	return ret;
}

/**
 * ptp_canon_get_objecthandle_by_name:
 *
 * This command looks up the specified object on the camera.
 *
 * Format is "A:\\PATH".
 *
 * The 'A' is the VolumeLabel from GetStorageInfo,
 * my IXUS has "A" for the card and "V" for internal memory.
 *  
 * params:	PTPParams*
 *      char* name - path name
 *
 * Return values: Some PTP_RC_* code.
 *      uint32_t *oid - PTP object id.
 *
 **/
uint16_t
ptp_canon_get_objecthandle_by_name (PTPParams* params, char* name, uint32_t* objectid)
{
	uint16_t ret;
	PTPContainer ptp;
	unsigned char *data = NULL;
	uint8_t len;

	PTP_CNT_INIT (ptp);
	ptp.Code=PTP_OC_CANON_GetObjectHandleByName;
	ptp.Nparam=0;
	len=0;
	data = malloc (2*(strlen(name)+1)+2);
	memset (data, 0, 2*(strlen(name)+1)+2);
	ptp_pack_string (params, name, data, 0, &len);
	ret=ptp_transaction (params, &ptp, PTP_DP_SENDDATA, (len+1)*2+1, &data, NULL);
	free (data);
	*objectid = ptp.Param1;
	return ret;
}

/**
 * ptp_canon_get_customize_data:
 *
 * This command downloads the specified theme slot, including jpegs
 * and wav files.
 *  
 * params:	PTPParams*
 *      uint32_t themenr - nr of theme
 *
 * Return values: Some PTP_RC_* code.
 *      unsigned char **data - pointer to data pointer
 *      unsigned int  *size - size of data returned
 *
 **/
uint16_t
ptp_canon_get_customize_data (PTPParams* params, uint32_t themenr,
		unsigned char **data, unsigned int *size)
{
	PTPContainer ptp;

	*data = NULL;
	*size = 0;
	PTP_CNT_INIT(ptp);
	ptp.Code	= PTP_OC_CANON_GetCustomizeData;
	ptp.Param1	= themenr;
	ptp.Nparam	= 1;
	return ptp_transaction(params, &ptp, PTP_DP_GETDATA, 0, data, size); 
}


uint16_t
ptp_nikon_curve_download (PTPParams* params, unsigned char **data, unsigned int *size) {
	PTPContainer ptp;
	*data = NULL;
	*size = 0;
	PTP_CNT_INIT(ptp);
	ptp.Code	= PTP_OC_NIKON_CurveDownload;
	ptp.Nparam	= 0;
	return ptp_transaction(params, &ptp, PTP_DP_GETDATA, 0, data, size); 
}

uint16_t
ptp_nikon_getfileinfoinblock ( PTPParams* params,
	uint32_t p1, uint32_t p2, uint32_t p3,
	unsigned char **data, unsigned int *size
) {
	PTPContainer ptp;
	*data = NULL;
	*size = 0;
	PTP_CNT_INIT(ptp);
	ptp.Code	= PTP_OC_NIKON_GetFileInfoInBlock;
	ptp.Nparam	= 3;
	ptp.Param1	= p1;
	ptp.Param2	= p2;
	ptp.Param3	= p3;
	return ptp_transaction(params, &ptp, PTP_DP_GETDATA, 0, data, size); 
}

/**
 * ptp_nikon_setcontrolmode:
 *
 * This command can switch the camera to full PC control mode.
 *  
 * params:	PTPParams*
 *      uint32_t mode - mode
 *
 * Return values: Some PTP_RC_* code.
 *
 **/
uint16_t
ptp_nikon_setcontrolmode (PTPParams* params, uint32_t mode)
{
        PTPContainer ptp;
        
        PTP_CNT_INIT(ptp);
        ptp.Code=PTP_OC_NIKON_SetControlMode;
        ptp.Param1=mode;
        ptp.Nparam=1;
        return ptp_transaction(params, &ptp, PTP_DP_NODATA, 0, NULL, NULL);
}

/**
 * ptp_nikon_capture:
 *
 * This command captures a picture on the Nikon.
 *  
 * params:	PTPParams*
 *      uint32_t x - unknown parameter. seen to be -1.
 *
 * Return values: Some PTP_RC_* code.
 *
 **/
uint16_t
ptp_nikon_capture (PTPParams* params, uint32_t x)
{
        PTPContainer ptp;
        
        PTP_CNT_INIT(ptp);
        ptp.Code=PTP_OC_NIKON_Capture;
        ptp.Param1=x;
        ptp.Nparam=1;
        return ptp_transaction(params, &ptp, PTP_DP_NODATA, 0, NULL, NULL);
}

/**
 * ptp_nikon_check_event:
 *
 * This command checks the event queue on the Nikon.
 *  
 * params:	PTPParams*
 *      PTPUSBEventContainer **event - list of usb events.
 *	int *evtcnt - number of usb events in event structure.
 *
 * Return values: Some PTP_RC_* code.
 *
 **/
uint16_t
ptp_nikon_check_event (PTPParams* params, PTPUSBEventContainer** event, int* evtcnt)
{
        PTPContainer ptp;
	uint16_t ret;
	unsigned char *data = NULL;
	unsigned int size = 0;

	PTP_CNT_INIT(ptp);
	ptp.Code=PTP_OC_NIKON_CheckEvent;
	ptp.Nparam=0;
	*evtcnt = 0;
	ret = ptp_transaction (params, &ptp, PTP_DP_GETDATA, 0, &data, &size);
	if (ret == PTP_RC_OK) {
		ptp_unpack_Nikon_EC (params, data, size, event, evtcnt);
		free (data);
	}
	return ret;
}

/**
 * ptp_nikon_device_ready:
 *
 * This command checks if the device is ready. Used after
 * a capture.
 *  
 * params:	PTPParams*
 *
 * Return values: Some PTP_RC_* code.
 *
 **/
uint16_t
ptp_nikon_device_ready (PTPParams* params)
{
        PTPContainer ptp;
        
        PTP_CNT_INIT(ptp);
        ptp.Code=PTP_OC_NIKON_DeviceReady;
        ptp.Nparam=0;
        return ptp_transaction(params, &ptp, PTP_DP_NODATA, 0, NULL, NULL);
}

/**
 * ptp_nikon_getptpipinfo:
 *
 * This command gets the ptpip info data.
 *  
 * params:	PTPParams*
 *	unsigned char *data	- data
 *	unsigned int size	- size of returned data
 *
 * Return values: Some PTP_RC_* code.
 *
 **/
uint16_t
ptp_nikon_getptpipinfo (PTPParams* params, unsigned char **data, unsigned int *size)
{
        PTPContainer ptp;
        
        PTP_CNT_INIT(ptp);
        ptp.Code=PTP_OC_NIKON_GetDevicePTPIPInfo;
        ptp.Nparam=0;
        return ptp_transaction(params, &ptp, PTP_DP_GETDATA, 0, data, size);
}

/**
 * ptp_nikon_getwifiprofilelist:
 *
 * This command gets the wifi profile list.
 *  
 * params:	PTPParams*
 *
 * Return values: Some PTP_RC_* code.
 *
 **/
uint16_t
ptp_nikon_getwifiprofilelist (PTPParams* params)
{
        PTPContainer ptp;
	unsigned char* data;
	unsigned int size;
	unsigned int pos;
	unsigned int profn;
	unsigned int n;
	char* buffer;
	uint8_t len;
	
        PTP_CNT_INIT(ptp);
        ptp.Code=PTP_OC_NIKON_GetProfileAllData;
        ptp.Nparam=0;
	size = 0;
	data = NULL;
	CHECK_PTP_RC(ptp_transaction(params, &ptp, PTP_DP_GETDATA, 0, &data, &size));

	if (size < 2) return PTP_RC_Undefined; /* FIXME: Add more precise error code */

	params->wifi_profiles_version = data[0];
	params->wifi_profiles_number = data[1];
	if (params->wifi_profiles)
		free(params->wifi_profiles);
	
	params->wifi_profiles = malloc(params->wifi_profiles_number*sizeof(PTPNIKONWifiProfile));
	memset(params->wifi_profiles, 0, params->wifi_profiles_number*sizeof(PTPNIKONWifiProfile));

	pos = 2;
	profn = 0;
	while (profn < params->wifi_profiles_number && pos < size) {
		if (pos+6 >= size) return PTP_RC_Undefined;
		params->wifi_profiles[profn].id = data[pos++];
		params->wifi_profiles[profn].valid = data[pos++];

		n = dtoh32a(&data[pos]);
		pos += 4;
		if (pos+n+4 >= size) return PTP_RC_Undefined;
		strncpy(params->wifi_profiles[profn].profile_name, (char*)&data[pos], n);
		params->wifi_profiles[profn].profile_name[16] = '\0';
		pos += n;

		params->wifi_profiles[profn].display_order = data[pos++];
		params->wifi_profiles[profn].device_type = data[pos++];
		params->wifi_profiles[profn].icon_type = data[pos++];

		buffer = ptp_unpack_string(params, data, pos, &len);
		strncpy(params->wifi_profiles[profn].creation_date, buffer, sizeof(params->wifi_profiles[profn].creation_date));
		free (buffer);
		pos += (len*2+1);
		if (pos+1 >= size) return PTP_RC_Undefined;
		/* FIXME: check if it is really last usage date */
		buffer = ptp_unpack_string(params, data, pos, &len);
		strncpy(params->wifi_profiles[profn].lastusage_date, buffer, sizeof(params->wifi_profiles[profn].lastusage_date));
		free (buffer);
		pos += (len*2+1);
		if (pos+5 >= size) return PTP_RC_Undefined;
		
		n = dtoh32a(&data[pos]);
		pos += 4;
		if (pos+n >= size) return PTP_RC_Undefined;
		strncpy(params->wifi_profiles[profn].essid, (char*)&data[pos], n);
		params->wifi_profiles[profn].essid[32] = '\0';
		pos += n;
		pos += 1;
		profn++;
	}

#if 0
	PTPNIKONWifiProfile test;
	memset(&test, 0, sizeof(PTPNIKONWifiProfile));
	strcpy(test.profile_name, "MyTest");
	test.icon_type = 1;
	strcpy(test.essid, "nikon");
	test.ip_address = 10 + 11 << 16 + 11 << 24;
	test.subnet_mask = 24;
	test.access_mode = 1;
	test.wifi_channel = 1;
	test.key_nr = 1;

	ptp_nikon_writewifiprofile(params, &test);
#endif

	return PTP_RC_OK;
}

/**
 * ptp_nikon_deletewifiprofile:
 *
 * This command deletes a wifi profile.
 *  
 * params:	PTPParams*
 *	unsigned int profilenr	- profile number
 *
 * Return values: Some PTP_RC_* code.
 *
 **/
uint16_t
ptp_nikon_deletewifiprofile (PTPParams* params, uint32_t profilenr)
{
	PTPContainer ptp;

	PTP_CNT_INIT(ptp);
	ptp.Code=PTP_OC_NIKON_DeleteProfile;
	ptp.Nparam=1;
	ptp.Param1=profilenr;
	return ptp_transaction(params, &ptp, PTP_DP_NODATA, 0, NULL, NULL);
}

/**
 * ptp_nikon_writewifiprofile:
 *
 * This command gets the ptpip info data.
 *  
 * params:	PTPParams*
 *	unsigned int profilenr	- profile number
 *	unsigned char *data	- data
 *	unsigned int size	- size of returned data
 *
 * Return values: Some PTP_RC_* code.
 *
 **/
uint16_t
ptp_nikon_writewifiprofile (PTPParams* params, PTPNIKONWifiProfile* profile)
{
	unsigned char guid[16];
	
	PTPContainer ptp;
	unsigned char buffer[1024];
	unsigned char* data = buffer;
	int size = 0;
	int i;
	uint8_t len;
	int profilenr = -1;
	
	//ptp_nikon_getptpipguid(guid);

	if (!params->wifi_profiles)
		CHECK_PTP_RC(ptp_nikon_getwifiprofilelist(params));
	
	for (i = 0; i < params->wifi_profiles_number; i++) {
		if (!params->wifi_profiles[i].valid) {
			profilenr = params->wifi_profiles[i].id;
			break;
		}
	}
	
	if (profilenr == -1) {
		/* No free profile! */
		return PTP_RC_StoreFull;
	}
	
	memset(buffer, 0, 1024);
	
	buffer[0x00] = 0x64; /* Version */
	
	/* Profile name */
	htod32a(&buffer[0x01], 17);
	/* 16 as third parameter, so there will always be a null-byte in the end */
	strncpy((char*)&buffer[0x05], profile->profile_name, 16);
	
	buffer[0x16] = 0x00; /* Display order */
	buffer[0x17] = profile->device_type;
	buffer[0x18] = profile->icon_type;
	
	/* FIXME: Creation date: put a real date here */
	ptp_pack_string(params, "19990909T090909", data, 0x19, &len);
	
	/* IP parameters */
	*((unsigned int*)&buffer[0x3A]) = profile->ip_address; /* Do not reverse bytes */
	buffer[0x3E] = profile->subnet_mask;
	*((unsigned int*)&buffer[0x3F]) = profile->gateway_address; /* Do not reverse bytes */
	buffer[0x43] = profile->address_mode;
	
	/* Wifi parameters */
	buffer[0x44] = profile->access_mode;
	buffer[0x45] = profile->wifi_channel;
	
	htod32a(&buffer[0x46], 33); /* essid */
	 /* 32 as third parameter, so there will always be a null-byte in the end */
	strncpy((char*)&buffer[0x4A], profile->essid, 32);
	
	buffer[0x6B] = profile->authentification;
	buffer[0x6C] = profile->encryption;
	htod32a(&buffer[0x6D], 64);
	for (i = 0; i < 64; i++) {
		buffer[0x71+i] = profile->key[i];
	}
	buffer[0xB1] = profile->key_nr;
	memcpy(&buffer[0xB2], guid, 16);
	
	switch(profile->encryption) {
	case 1: /* WEP 64bit */
		htod16a(&buffer[0xC2], 5); /* (64-24)/8 = 5 */
		break;
	case 2: /* WEP 128bit */
		htod16a(&buffer[0xC2], 13); /* (128-24)/8 = 13 */
		break;
	default:
		htod16a(&buffer[0xC2], 0);
	}
	size = 0xC4;
	       
	PTP_CNT_INIT(ptp);
	ptp.Code=PTP_OC_NIKON_SendProfileData;
	ptp.Nparam=1;
	ptp.Param1=profilenr;
	return ptp_transaction(params, &ptp, PTP_DP_SENDDATA, size, &data, NULL);
}

/**
 * ptp_mtp_getobjectpropssupported:
 *
 * This command gets the object properties possible from the device.
 *  
 * params:	PTPParams*
 *	uint ofc		- object format code
 *	unsigned int *propnum	- number of elements in returned array
 *	uint16_t *props		- array of supported properties
 *
 * Return values: Some PTP_RC_* code.
 *
 **/
uint16_t
ptp_mtp_getobjectpropssupported (PTPParams* params, uint16_t ofc,
		 uint32_t *propnum, uint16_t **props
) {
        PTPContainer ptp;
	uint16_t ret;
	unsigned char *data = NULL;
	unsigned int size = 0;
        
        PTP_CNT_INIT(ptp);
        ptp.Code=PTP_OC_MTP_GetObjectPropsSupported;
        ptp.Nparam = 1;
        ptp.Param1 = ofc;
        ret = ptp_transaction(params, &ptp, PTP_DP_GETDATA, 0, &data, &size);
	if (ret == PTP_RC_OK)
        	*propnum=ptp_unpack_uint16_t_array(params,data,0,props);
	free(data);
	return ret;
}

/**
 * ptp_mtp_getobjectpropdesc:
 *
 * This command gets the object property description.
 *  
 * params:	PTPParams*
 *	uint16_t opc	- object property code
 *	uint16_t ofc	- object format code
 *
 * Return values: Some PTP_RC_* code.
 *
 **/
uint16_t
ptp_mtp_getobjectpropdesc (
	PTPParams* params, uint16_t opc, uint16_t ofc, PTPObjectPropDesc *opd
) {
        PTPContainer ptp;
	uint16_t ret;
	unsigned char *data = NULL;
	unsigned int size = 0;
        
        PTP_CNT_INIT(ptp);
        ptp.Code=PTP_OC_MTP_GetObjectPropDesc;
        ptp.Nparam = 2;
        ptp.Param1 = opc;
        ptp.Param2 = ofc;
        ret = ptp_transaction(params, &ptp, PTP_DP_GETDATA, 0, &data, &size);
	if (ret == PTP_RC_OK)
		ptp_unpack_OPD (params, data, opd, size);
	free(data);
	return ret;
}

/**
 * ptp_mtp_getobjectpropvalue:
 *
 * This command gets the object properties of an object handle.
 *  
 * params:	PTPParams*
 *	uint32_t objectid	- object format code
 *	uint16_t opc		- object prop code
 *
 * Return values: Some PTP_RC_* code.
 *
 **/
uint16_t
ptp_mtp_getobjectpropvalue (
	PTPParams* params, uint32_t oid, uint16_t opc,
	PTPPropertyValue *value, uint16_t datatype
) {
        PTPContainer ptp;
	uint16_t ret;
	unsigned char *data = NULL;
	unsigned int size = 0;
	int offset = 0;
        
        PTP_CNT_INIT(ptp);
        ptp.Code=PTP_OC_MTP_GetObjectPropValue;
        ptp.Nparam = 2;
        ptp.Param1 = oid;
        ptp.Param2 = opc;
        ret = ptp_transaction(params, &ptp, PTP_DP_GETDATA, 0, &data, &size);
	if (ret == PTP_RC_OK)
		ptp_unpack_DPV(params, data, &offset, size, value, datatype);
	free(data);
	return ret;
}

/**
 * ptp_mtp_setobjectpropvalue:
 *
 * This command gets the object properties of an object handle.
 *  
 * params:	PTPParams*
 *	uint32_t objectid	- object format code
 *	uint16_t opc		- object prop code
 *
 * Return values: Some PTP_RC_* code.
 *
 **/
uint16_t
ptp_mtp_setobjectpropvalue (
	PTPParams* params, uint32_t oid, uint16_t opc,
	PTPPropertyValue *value, uint16_t datatype
) {
        PTPContainer ptp;
	uint16_t ret;
	unsigned char *data = NULL;
	unsigned int size ;
        
        PTP_CNT_INIT(ptp);
        ptp.Code=PTP_OC_MTP_SetObjectPropValue;
        ptp.Nparam = 2;
        ptp.Param1 = oid;
        ptp.Param2 = opc;
	size = ptp_pack_DPV(params, value, &data, datatype);
        ret = ptp_transaction(params, &ptp, PTP_DP_SENDDATA, size, &data, NULL);
	free(data);
	return ret;
}

uint16_t
ptp_mtp_getobjectreferences (PTPParams* params, uint32_t handle, uint32_t** ohArray, uint32_t* arraylen)
{
	PTPContainer ptp;
	uint16_t ret;
	unsigned char* dpv=NULL;
	unsigned int dpvlen = 0;

	PTP_CNT_INIT(ptp);
	ptp.Code=PTP_OC_MTP_GetObjectReferences;
	ptp.Param1=handle;
	ptp.Nparam=1;
	ret=ptp_transaction(params, &ptp, PTP_DP_GETDATA, 0, &dpv, &dpvlen);
	if (ret == PTP_RC_OK) {
		/* Sandisk Sansa skips the DATA phase, but returns OK as response.
		 * this will gives us a NULL here. Handle it. -Marcus */
		if ((dpv == NULL) || (dpvlen == 0)) {
			*arraylen = 0;
			*ohArray = NULL;
		} else {
			*arraylen = ptp_unpack_uint32_t_array(params, dpv, 0, ohArray);
		}
	}
	free(dpv);
	return ret;
}

uint16_t
ptp_mtp_setobjectreferences (PTPParams* params, uint32_t handle, uint32_t* ohArray, uint32_t arraylen)
{
	PTPContainer ptp;
	uint16_t ret;
	uint32_t size;
	unsigned char* dpv=NULL;

	PTP_CNT_INIT(ptp);
	ptp.Code   = PTP_OC_MTP_SetObjectReferences;
	ptp.Param1 = handle;
	ptp.Nparam = 1;
	size = ptp_pack_uint32_t_array(params, ohArray, arraylen, &dpv);
	ret = ptp_transaction(params, &ptp, PTP_DP_SENDDATA, size, (unsigned char **)&dpv, NULL);
	free(dpv);
	return ret;
}

uint16_t
ptp_mtp_getobjectproplist (PTPParams* params, uint32_t handle, MTPProperties **props, int *nrofprops)
{
	uint16_t ret;
	PTPContainer ptp;
	unsigned char* opldata = NULL;
	unsigned int oplsize;

	PTP_CNT_INIT(ptp);
	ptp.Code = PTP_OC_MTP_GetObjPropList;
	ptp.Param1 = handle;
	ptp.Param2 = 0x00000000U;  /* 0x00000000U should be "all formats" */
	ptp.Param3 = 0xFFFFFFFFU;  /* 0xFFFFFFFFU should be "all properties" */
	ptp.Param4 = 0x00000000U;
	ptp.Param5 = 0x00000000U;
	ptp.Nparam = 5;
	ret = ptp_transaction(params, &ptp, PTP_DP_GETDATA, 0, &opldata, &oplsize);  
	if (ret == PTP_RC_OK) *nrofprops = ptp_unpack_OPL(params, opldata, props, oplsize);
	if (opldata != NULL)
		free(opldata);
	return ret;
}

uint16_t
ptp_mtp_sendobjectproplist (PTPParams* params, uint32_t* store, uint32_t* parenthandle, uint32_t* handle,
			    uint16_t objecttype, uint64_t objectsize, MTPProperties *props, int nrofprops)
{
	uint16_t ret;
	PTPContainer ptp;
	unsigned char* opldata=NULL;
	uint32_t oplsize;

	PTP_CNT_INIT(ptp);
	ptp.Code = PTP_OC_MTP_SendObjectPropList;
	ptp.Param1 = *store;
	ptp.Param2 = *parenthandle;
	ptp.Param3 = (uint32_t) objecttype;
	ptp.Param4 = (uint32_t) (objectsize >> 32);
	ptp.Param5 = (uint32_t) (objectsize & 0xffffffffU);
	ptp.Nparam = 5;

	/* Set object handle to 0 for a new object */
	oplsize = ptp_pack_OPL(params,props,nrofprops,&opldata);
	ret = ptp_transaction(params, &ptp, PTP_DP_SENDDATA, oplsize, &opldata, NULL); 
	free(opldata);
	*store = ptp.Param1;
	*parenthandle = ptp.Param2;
	*handle = ptp.Param3; 

	return ret;
}

uint16_t
ptp_mtp_setobjectproplist (PTPParams* params, MTPProperties *props, int nrofprops)
{
	uint16_t ret;
	PTPContainer ptp;
	unsigned char* opldata=NULL;
	uint32_t oplsize;
  
	PTP_CNT_INIT(ptp);
	ptp.Code = PTP_OC_MTP_SetObjPropList;
	ptp.Nparam = 0;
  
	oplsize = ptp_pack_OPL(params,props,nrofprops,&opldata);
	ret = ptp_transaction(params, &ptp, PTP_DP_SENDDATA, oplsize, &opldata, NULL); 
	free(opldata);

	return ret;
}

/* Non PTP protocol functions */
/* devinfo testing functions */

int
ptp_operation_issupported(PTPParams* params, uint16_t operation)
{
	int i=0;

	for (;i<params->deviceinfo.OperationsSupported_len;i++) {
		if (params->deviceinfo.OperationsSupported[i]==operation)
			return 1;
	}
	return 0;
}


int
ptp_event_issupported(PTPParams* params, uint16_t event)
{
	int i=0;

	for (;i<params->deviceinfo.EventsSupported_len;i++) {
		if (params->deviceinfo.EventsSupported[i]==event)
			return 1;
	}
	return 0;
}


int
ptp_property_issupported(PTPParams* params, uint16_t property)
{
	int i;

	for (i=0;i<params->deviceinfo.DevicePropertiesSupported_len;i++)
		if (params->deviceinfo.DevicePropertiesSupported[i]==property)
			return 1;
	return 0;
}

/* ptp structures freeing functions */
void
ptp_free_devicepropvalue(uint16_t dt, PTPPropertyValue* dpd) {
	switch (dt) {
	case PTP_DTC_INT8:	case PTP_DTC_UINT8:
	case PTP_DTC_UINT16:	case PTP_DTC_INT16:
	case PTP_DTC_UINT32:	case PTP_DTC_INT32:
	case PTP_DTC_UINT64:	case PTP_DTC_INT64:
	case PTP_DTC_UINT128:	case PTP_DTC_INT128:
		/* Nothing to free */
		break;
	case PTP_DTC_AINT8:	case PTP_DTC_AUINT8:
	case PTP_DTC_AUINT16:	case PTP_DTC_AINT16:
	case PTP_DTC_AUINT32:	case PTP_DTC_AINT32:
	case PTP_DTC_AUINT64:	case PTP_DTC_AINT64:
	case PTP_DTC_AUINT128:	case PTP_DTC_AINT128:
		if (dpd->a.v)
			free(dpd->a.v);
		break;
	case PTP_DTC_STR:
		if (dpd->str)
			free(dpd->str);
		break;
	}
}

void
ptp_free_devicepropdesc(PTPDevicePropDesc* dpd)
{
	uint16_t i;

	ptp_free_devicepropvalue (dpd->DataType, &dpd->FactoryDefaultValue);
	ptp_free_devicepropvalue (dpd->DataType, &dpd->CurrentValue);
	switch (dpd->FormFlag) {
	case PTP_DPFF_Range:
		ptp_free_devicepropvalue (dpd->DataType, &dpd->FORM.Range.MinimumValue);
		ptp_free_devicepropvalue (dpd->DataType, &dpd->FORM.Range.MaximumValue);
		ptp_free_devicepropvalue (dpd->DataType, &dpd->FORM.Range.StepSize);
		break;
	case PTP_DPFF_Enumeration:
		if (dpd->FORM.Enum.SupportedValue) {
			for (i=0;i<dpd->FORM.Enum.NumberOfValues;i++)
				ptp_free_devicepropvalue (dpd->DataType, dpd->FORM.Enum.SupportedValue+i);
			free (dpd->FORM.Enum.SupportedValue);
		}
	}
}

void
ptp_free_objectpropdesc(PTPObjectPropDesc* opd)
{
	uint16_t i;

	ptp_free_devicepropvalue (opd->DataType, &opd->FactoryDefaultValue);
	switch (opd->FormFlag) {
	case PTP_OPFF_None:
		break;
	case PTP_OPFF_Range:
		ptp_free_devicepropvalue (opd->DataType, &opd->FORM.Range.MinimumValue);
		ptp_free_devicepropvalue (opd->DataType, &opd->FORM.Range.MaximumValue);
		ptp_free_devicepropvalue (opd->DataType, &opd->FORM.Range.StepSize);
		break;
	case PTP_OPFF_Enumeration:
		if (opd->FORM.Enum.SupportedValue) {
			for (i=0;i<opd->FORM.Enum.NumberOfValues;i++)
				ptp_free_devicepropvalue (opd->DataType, opd->FORM.Enum.SupportedValue+i);
			free (opd->FORM.Enum.SupportedValue);
		}
		break;
	case PTP_OPFF_DateTime:
	case PTP_OPFF_FixedLengthArray:
	case PTP_OPFF_RegularExpression:
	case PTP_OPFF_ByteArray:
	case PTP_OPFF_LongString:
		/* Ignore these presently, we cannot unpack them, so there is nothing to be freed. */
		break;
	default:
		fprintf (stderr, "Unknown OPFF type %d\n", opd->FormFlag);
		break;
	}
}

void
ptp_free_objectinfo (PTPObjectInfo *oi)
{
	if (!oi) return;
        free (oi->Filename); oi->Filename = NULL;
        free (oi->Keywords); oi->Keywords = NULL;
}

void 
ptp_perror(PTPParams* params, uint16_t error) {

	int i;
	/* PTP error descriptions */
	static struct {
		short n;
		const char *txt;
	} ptp_errors[] = {
	{PTP_RC_Undefined, 		N_("PTP: Undefined Error")},
	{PTP_RC_OK, 			N_("PTP: OK!")},
	{PTP_RC_GeneralError, 		N_("PTP: General Error")},
	{PTP_RC_SessionNotOpen, 	N_("PTP: Session Not Open")},
	{PTP_RC_InvalidTransactionID, 	N_("PTP: Invalid Transaction ID")},
	{PTP_RC_OperationNotSupported, 	N_("PTP: Operation Not Supported")},
	{PTP_RC_ParameterNotSupported, 	N_("PTP: Parameter Not Supported")},
	{PTP_RC_IncompleteTransfer, 	N_("PTP: Incomplete Transfer")},
	{PTP_RC_InvalidStorageId, 	N_("PTP: Invalid Storage ID")},
	{PTP_RC_InvalidObjectHandle, 	N_("PTP: Invalid Object Handle")},
	{PTP_RC_DevicePropNotSupported, N_("PTP: Device Prop Not Supported")},
	{PTP_RC_InvalidObjectFormatCode, N_("PTP: Invalid Object Format Code")},
	{PTP_RC_StoreFull, 		N_("PTP: Store Full")},
	{PTP_RC_ObjectWriteProtected, 	N_("PTP: Object Write Protected")},
	{PTP_RC_StoreReadOnly, 		N_("PTP: Store Read Only")},
	{PTP_RC_AccessDenied,		N_("PTP: Access Denied")},
	{PTP_RC_NoThumbnailPresent, 	N_("PTP: No Thumbnail Present")},
	{PTP_RC_SelfTestFailed, 	N_("PTP: Self Test Failed")},
	{PTP_RC_PartialDeletion, 	N_("PTP: Partial Deletion")},
	{PTP_RC_StoreNotAvailable, 	N_("PTP: Store Not Available")},
	{PTP_RC_SpecificationByFormatUnsupported,
				N_("PTP: Specification By Format Unsupported")},
	{PTP_RC_NoValidObjectInfo, 	N_("PTP: No Valid Object Info")},
	{PTP_RC_InvalidCodeFormat, 	N_("PTP: Invalid Code Format")},
	{PTP_RC_UnknownVendorCode, 	N_("PTP: Unknown Vendor Code")},
	{PTP_RC_CaptureAlreadyTerminated,
					N_("PTP: Capture Already Terminated")},
	{PTP_RC_DeviceBusy, 		N_("PTP: Device Busy")},
	{PTP_RC_InvalidParentObject, 	N_("PTP: Invalid Parent Object")},
	{PTP_RC_InvalidDevicePropFormat, N_("PTP: Invalid Device Prop Format")},
	{PTP_RC_InvalidDevicePropValue, N_("PTP: Invalid Device Prop Value")},
	{PTP_RC_InvalidParameter, 	N_("PTP: Invalid Parameter")},
	{PTP_RC_SessionAlreadyOpened, 	N_("PTP: Session Already Opened")},
	{PTP_RC_TransactionCanceled, 	N_("PTP: Transaction Canceled")},
	{PTP_RC_SpecificationOfDestinationUnsupported,
			N_("PTP: Specification Of Destination Unsupported")},
	{PTP_RC_EK_FilenameRequired,	N_("PTP: EK Filename Required")},
	{PTP_RC_EK_FilenameConflicts,	N_("PTP: EK Filename Conflicts")},
	{PTP_RC_EK_FilenameInvalid,	N_("PTP: EK Filename Invalid")},

	{PTP_ERROR_IO,		  N_("PTP: I/O error")},
	{PTP_ERROR_BADPARAM,	  N_("PTP: Error: bad parameter")},
	{PTP_ERROR_DATA_EXPECTED, N_("PTP: Protocol error, data expected")},
	{PTP_ERROR_RESP_EXPECTED, N_("PTP: Protocol error, response expected")},
	{0, NULL}
};

	for (i=0; ptp_errors[i].txt!=NULL; i++)
		if (ptp_errors[i].n == error)
			ptp_error(params, ptp_errors[i].txt);
}

const char*
ptp_get_property_description(PTPParams* params, uint16_t dpc)
{
	int i;
	/* Device Property descriptions */
	struct {
		uint16_t dpc;
		const char *txt;
	} ptp_device_properties[] = {
		{PTP_DPC_Undefined,		N_("Undefined PTP Property")},
		{PTP_DPC_BatteryLevel,		N_("Battery Level")},
		{PTP_DPC_FunctionalMode,	N_("Functional Mode")},
		{PTP_DPC_ImageSize,		N_("Image Size")},
		{PTP_DPC_CompressionSetting,	N_("Compression Setting")},
		{PTP_DPC_WhiteBalance,		N_("White Balance")},
		{PTP_DPC_RGBGain,		N_("RGB Gain")},
		{PTP_DPC_FNumber,		N_("F-Number")},
		{PTP_DPC_FocalLength,		N_("Focal Length")},
		{PTP_DPC_FocusDistance,		N_("Focus Distance")},
		{PTP_DPC_FocusMode,		N_("Focus Mode")},
		{PTP_DPC_ExposureMeteringMode,	N_("Exposure Metering Mode")},
		{PTP_DPC_FlashMode,		N_("Flash Mode")},
		{PTP_DPC_ExposureTime,		N_("Exposure Time")},
		{PTP_DPC_ExposureProgramMode,	N_("Exposure Program Mode")},
		{PTP_DPC_ExposureIndex,
					N_("Exposure Index (film speed ISO)")},
		{PTP_DPC_ExposureBiasCompensation,
					N_("Exposure Bias Compensation")},
		{PTP_DPC_DateTime,		N_("Date & Time")},
		{PTP_DPC_CaptureDelay,		N_("Pre-Capture Delay")},
		{PTP_DPC_StillCaptureMode,	N_("Still Capture Mode")},
		{PTP_DPC_Contrast,		N_("Contrast")},
		{PTP_DPC_Sharpness,		N_("Sharpness")},
		{PTP_DPC_DigitalZoom,		N_("Digital Zoom")},
		{PTP_DPC_EffectMode,		N_("Effect Mode")},
		{PTP_DPC_BurstNumber,		N_("Burst Number")},
		{PTP_DPC_BurstInterval,		N_("Burst Interval")},
		{PTP_DPC_TimelapseNumber,	N_("Timelapse Number")},
		{PTP_DPC_TimelapseInterval,	N_("Timelapse Interval")},
		{PTP_DPC_FocusMeteringMode,	N_("Focus Metering Mode")},
		{PTP_DPC_UploadURL,		N_("Upload URL")},
		{PTP_DPC_Artist,		N_("Artist")},
		{PTP_DPC_CopyrightInfo,		N_("Copyright Info")},
		{0,NULL}
	};
	struct {
		uint16_t dpc;
		const char *txt;
	} ptp_device_properties_EK[] = {
		{PTP_DPC_EK_ColorTemperature,	N_("Color Temperature")},
		{PTP_DPC_EK_DateTimeStampFormat,
					N_("Date Time Stamp Format")},
		{PTP_DPC_EK_BeepMode,		N_("Beep Mode")},
		{PTP_DPC_EK_VideoOut,		N_("Video Out")},
		{PTP_DPC_EK_PowerSaving,	N_("Power Saving")},
		{PTP_DPC_EK_UI_Language,	N_("UI Language")},
		{0,NULL}
	};

	struct {
		uint16_t dpc;
		const char *txt;
	} ptp_device_properties_Canon[] = {
		{PTP_DPC_CANON_BeepMode,	N_("Beep Mode")},
		{PTP_DPC_CANON_BatteryKind,	N_("Battery Type")},
		{PTP_DPC_CANON_BatteryStatus,	N_("Battery Mode")},
		{PTP_DPC_CANON_UILockType,	N_("UILockType")},
		{PTP_DPC_CANON_CameraMode,	N_("Camera Mode")},
		{PTP_DPC_CANON_ImageQuality,	N_("Image Quality")},
		{PTP_DPC_CANON_FullViewFileFormat,	N_("Full View File Format")},
		{PTP_DPC_CANON_ImageSize,	N_("Image Size")},
		{PTP_DPC_CANON_SelfTime,	N_("Self Time")},
		{PTP_DPC_CANON_FlashMode,	N_("Flash Mode")},
		{PTP_DPC_CANON_Beep,		N_("Beep")},
		{PTP_DPC_CANON_ShootingMode,	N_("Shooting Mode")},
		{PTP_DPC_CANON_ImageMode,	N_("Image Mode")},
		{PTP_DPC_CANON_DriveMode,	N_("Drive Mode")},
		{PTP_DPC_CANON_EZoom,		N_("Zoom")},
		{PTP_DPC_CANON_MeteringMode,	N_("Metering Mode")},
		{PTP_DPC_CANON_AFDistance,	N_("AF Distance")},
		{PTP_DPC_CANON_FocusingPoint,	N_("Focusing Point")},
		{PTP_DPC_CANON_WhiteBalance,	N_("White Balance")},
		{PTP_DPC_CANON_SlowShutterSetting,	N_("Slow Shutter Setting")},
		{PTP_DPC_CANON_AFMode,		N_("AF Mode")},
		{PTP_DPC_CANON_ImageStabilization,		N_("Image Stabilization")},
		{PTP_DPC_CANON_Contrast,	N_("Contrast")},
		{PTP_DPC_CANON_ColorGain,	N_("Color Gain")},
		{PTP_DPC_CANON_Sharpness,	N_("Sharpness")},
		{PTP_DPC_CANON_Sensitivity,	N_("Sensitivity")},
		{PTP_DPC_CANON_ParameterSet,	N_("Parameter Set")},
		{PTP_DPC_CANON_ISOSpeed,	N_("ISO Speed")},
		{PTP_DPC_CANON_Aperture,	N_("Aperture")},
		{PTP_DPC_CANON_ShutterSpeed,	N_("Shutter Speed")},
		{PTP_DPC_CANON_ExpCompensation,	N_("Exposure Compensation")},
		{PTP_DPC_CANON_FlashCompensation,	N_("Flash Compensation")},
		{PTP_DPC_CANON_AEBExposureCompensation,	N_("AEB Exposure Compensation")},
		{PTP_DPC_CANON_AvOpen,		N_("Av Open")},
		{PTP_DPC_CANON_AvMax,		N_("Av Max")},
		{PTP_DPC_CANON_FocalLength,	N_("Focal Length")},
		{PTP_DPC_CANON_FocalLengthTele,	N_("Focal Length Tele")},
		{PTP_DPC_CANON_FocalLengthWide,	N_("Focal Length Wide")},
		{PTP_DPC_CANON_FocalLengthDenominator,	N_("Focal Length Denominator")},
		{PTP_DPC_CANON_CaptureTransferMode,	N_("Capture Transfer Mode")},
		{PTP_DPC_CANON_Zoom,		N_("Zoom")},
		{PTP_DPC_CANON_NamePrefix,	N_("Name Prefix")},
		{PTP_DPC_CANON_SizeQualityMode,	N_("Size Quality Mode")},
		{PTP_DPC_CANON_SupportedThumbSize,	N_("Supported Thumb Size")},
		{PTP_DPC_CANON_SizeOfOutputDataFromCamera,	N_("Size of Output Data from Camera")},
		{PTP_DPC_CANON_SizeOfInputDataToCamera,		N_("Size of Input Data to Camera")},
		{PTP_DPC_CANON_RemoteAPIVersion,N_("Remote API Version")},
		{PTP_DPC_CANON_FirmwareVersion,	N_("Firmware Version")},
		{PTP_DPC_CANON_CameraModel,	N_("Camera Model")},
		{PTP_DPC_CANON_CameraOwner,	N_("Camera Owner")},
		{PTP_DPC_CANON_UnixTime,	N_("UNIX Time")},
		{PTP_DPC_CANON_CameraBodyID,	N_("Camera Body ID")},
		{PTP_DPC_CANON_CameraOutput,	N_("Camera Output")},
		{PTP_DPC_CANON_DispAv,		N_("Disp Av")},
		{PTP_DPC_CANON_AvOpenApex,	N_("Av Open Apex")},
		{PTP_DPC_CANON_DZoomMagnification,	N_("Digital Zoom Magnification")},
		{PTP_DPC_CANON_MlSpotPos,	N_("Ml Spot Position")},
		{PTP_DPC_CANON_DispAvMax,	N_("Disp Av Max")},
		{PTP_DPC_CANON_AvMaxApex,	N_("Av Max Apex")},
		{PTP_DPC_CANON_EZoomStartPosition,	N_("EZoom Start Position")},
		{PTP_DPC_CANON_FocalLengthOfTele,	N_("Focal Length Tele")},
		{PTP_DPC_CANON_EZoomSizeOfTele,	N_("EZoom Size of Tele")},
		{PTP_DPC_CANON_PhotoEffect,	N_("Photo Effect")},
		{PTP_DPC_CANON_AssistLight,	N_("Assist Light")},
		{PTP_DPC_CANON_FlashQuantityCount,	N_("Flash Quantity Count")},
		{PTP_DPC_CANON_RotationAngle,	N_("Rotation Angle")},
		{PTP_DPC_CANON_RotationScene,	N_("Rotation Scene")},
		{PTP_DPC_CANON_EventEmulateMode,N_("Event Emulate Mode")},
		{PTP_DPC_CANON_DPOFVersion,	N_("DPOF Version")},
		{PTP_DPC_CANON_TypeOfSupportedSlideShow,	N_("Type of Slideshow")},
		{PTP_DPC_CANON_AverageFilesizes,N_("Average Filesizes")},
		{PTP_DPC_CANON_ModelID,		N_("Model ID")},
		{0,NULL}
	};

	struct {
		uint16_t dpc;
		const char *txt;
	} ptp_device_properties_Nikon[] = {
		{PTP_DPC_NIKON_WhiteBalanceAutoBias,		/* 0xD017 */
		 N_("Auto White Balance Bias")},
		{PTP_DPC_NIKON_WhiteBalanceTungstenBias,	/* 0xD018 */
		 N_("Tungsten White Balance Bias")},
		{PTP_DPC_NIKON_WhiteBalanceFluorescentBias,	/* 0xD019 */
		 N_("Fluorescent White Balance Bias")},
		{PTP_DPC_NIKON_WhiteBalanceDaylightBias,	/* 0xD01a */
		 N_("Daylight White Balance Bias")},
		{PTP_DPC_NIKON_WhiteBalanceFlashBias,		/* 0xD01b */
		 N_("Flash White Balance Bias")},
		{PTP_DPC_NIKON_WhiteBalanceCloudyBias,		/* 0xD01c */
		 N_("Cloudy White Balance Bias")},
		{PTP_DPC_NIKON_WhiteBalanceShadeBias,		/* 0xD01d */
		 N_("Shady White Balance Bias")},
		{PTP_DPC_NIKON_WhiteBalanceColorTemperature,	/* 0xD01e */
		 N_("White Balance Colour Temperature")},
		{PTP_DPC_NIKON_ImageSharpening,			/* 0xD02a */
		 N_("Sharpening")},
		{PTP_DPC_NIKON_ToneCompensation,		/* 0xD02b */
		 N_("Tone Compensation")},
		{PTP_DPC_NIKON_ColorModel,			/* 0xD02c */
		 N_("Color Model")},
		{PTP_DPC_NIKON_HueAdjustment,			/* 0xD02d */
		 N_("Hue Adjustment")},
		{PTP_DPC_NIKON_NonCPULensDataFocalLength,	/* 0xD02e */
		 N_("Lens Focal Length (Non CPU)")},
		{PTP_DPC_NIKON_NonCPULensDataMaximumAperture,	/* 0xD02f */
		 N_("Lens Max. Aperture (Non CPU)")},
		{PTP_DPC_NIKON_CSMMenuBankSelect,		/* 0xD040 */
		 "PTP_DPC_NIKON_CSMMenuBankSelect"},
		{PTP_DPC_NIKON_MenuBankNameA,			/* 0xD041 */
		 "PTP_DPC_NIKON_MenuBankNameA"},
		{PTP_DPC_NIKON_MenuBankNameB,			/* 0xD042 */
		 "PTP_DPC_NIKON_MenuBankNameB"},
		{PTP_DPC_NIKON_MenuBankNameC,			/* 0xD043 */
		 "PTP_DPC_NIKON_MenuBankNameC"},
		{PTP_DPC_NIKON_MenuBankNameD,			/* 0xD044 */
		 "PTP_DPC_NIKON_MenuBankNameD"},
		{PTP_DPC_NIKON_A1AFCModePriority,		/* 0xD048 */
		 "PTP_DPC_NIKON_A1AFCModePriority"},
		{PTP_DPC_NIKON_A2AFSModePriority,		/* 0xD049 */
		 "PTP_DPC_NIKON_A2AFSModePriority"},
		{PTP_DPC_NIKON_A3GroupDynamicAF,		/* 0xD04a */
		 "PTP_DPC_NIKON_A3GroupDynamicAF"},
		{PTP_DPC_NIKON_A4AFActivation,			/* 0xD04b */
		 "PTP_DPC_NIKON_A4AFActivation"},
		{PTP_DPC_NIKON_A5FocusAreaIllumManualFocus,	/* 0xD04c */
		 "PTP_DPC_NIKON_A5FocusAreaIllumManualFocus"},
		{PTP_DPC_NIKON_FocusAreaIllumContinuous,	/* 0xD04d */
		 "PTP_DPC_NIKON_FocusAreaIllumContinuous"},
		{PTP_DPC_NIKON_FocusAreaIllumWhenSelected,	/* 0xD04e */
		 "PTP_DPC_NIKON_FocusAreaIllumWhenSelected"},
		{PTP_DPC_NIKON_FocusAreaWrap,			/* 0xD04f */
		 N_("Focus Area Wrap")},
		{PTP_DPC_NIKON_A7VerticalAFON,			/* 0xD050 */
		 N_("Vertical AF On")},
		{PTP_DPC_NIKON_ISOAuto,				/* 0xD054 */
		 N_("Auto ISO")},
		{PTP_DPC_NIKON_B2ISOStep,			/* 0xD055 */
		 N_("ISO Step")},
		{PTP_DPC_NIKON_EVStep,				/* 0xD056 */
		 N_("Exposure Step")},
		{PTP_DPC_NIKON_B4ExposureCompEv,		/* 0xD057 */
		 N_("Exposure Compensation (EV)")},
		{PTP_DPC_NIKON_ExposureCompensation,		/* 0xD058 */
		 N_("Exposure Compensation")},
		{PTP_DPC_NIKON_CenterWeightArea,		/* 0xD059 */
		 N_("Centre Weight Area")},
		{PTP_DPC_NIKON_AELockMode,			/* 0xD05e */
		 N_("Exposure Lock")},
		{PTP_DPC_NIKON_AELAFLMode,			/* 0xD05f */
		 N_("Focus Lock")},
		{PTP_DPC_NIKON_MeterOff,			/* 0xD062 */
		 N_("Auto Meter Off Time")},
		{PTP_DPC_NIKON_SelfTimer,			/* 0xD063 */
		 N_("Self Timer Delay")},
		{PTP_DPC_NIKON_MonitorOff,			/* 0xD064 */
		 N_("LCD Off Time")},
		{PTP_DPC_NIKON_D1ShootingSpeed,			/* 0xD068 */
		 N_("Shooting Speed")},
		{PTP_DPC_NIKON_D2MaximumShots,			/* 0xD069 */
		 N_("Maximum Shots")},
		{PTP_DPC_NIKON_D3ExpDelayMode,			/* 0xD06a */
		 "PTP_DPC_NIKON_D3ExpDelayMode"},
		{PTP_DPC_NIKON_LongExposureNoiseReduction,	/* 0xD06b */
		 N_("Long Exposure Noise Reduction")},
		{PTP_DPC_NIKON_FileNumberSequence,		/* 0xD06c */
		 N_("File Number Sequencing")},
		{PTP_DPC_NIKON_D6ControlPanelFinderRearControl,	/* 0xD06d */
		 "PTP_DPC_NIKON_D6ControlPanelFinderRearControl"},
		{PTP_DPC_NIKON_ControlPanelFinderViewfinder,	/* 0xD06e */
		 "PTP_DPC_NIKON_ControlPanelFinderViewfinder"},
		{PTP_DPC_NIKON_D7Illumination,			/* 0xD06f */
		 "PTP_DPC_NIKON_D7Illumination"},
		{PTP_DPC_NIKON_E1FlashSyncSpeed,		/* 0xD074 */
		 N_("Flash Sync. Speed")},
		{PTP_DPC_NIKON_FlashShutterSpeed,		/* 0xD075 */
		 N_("Flash Shutter Speed")},
		{PTP_DPC_NIKON_E3AAFlashMode,			/* 0xD076 */
		 N_("Flash Mode")},
		{PTP_DPC_NIKON_E4ModelingFlash,			/* 0xD077 */
		 N_("Modeling Flash")},
		{PTP_DPC_NIKON_BracketSet,			/* 0xD078 */
		 N_("Bracket Set")},
		{PTP_DPC_NIKON_E6ManualModeBracketing,		/* 0xD079 */
		 N_("Manual Mode Bracketing")},
		{PTP_DPC_NIKON_BracketOrder,			/* 0xD07a */
		 N_("Bracket Order")},
		{PTP_DPC_NIKON_E8AutoBracketSelection,		/* 0xD07b */
		 N_("Auto Bracket Selection")},
		{PTP_DPC_NIKON_F1CenterButtonShootingMode,	/* 0xD080 */
		 N_("Center Button Shooting Mode")},
		{PTP_DPC_NIKON_CenterButtonPlaybackMode,	/* 0xD081 */
		 N_("Center Button Playback Mode")},
		{PTP_DPC_NIKON_F2Multiselector,			/* 0xD082 */
		 N_("Multiselector")},
		{PTP_DPC_NIKON_F3PhotoInfoPlayback,		/* 0xD083 */
		 N_("Photo Info. Playback")},
		{PTP_DPC_NIKON_F4AssignFuncButton,		/* 0xD084 */
		 N_("Assign Func. Button")},
		{PTP_DPC_NIKON_F5CustomizeCommDials,		/* 0xD085 */
		 N_("Customise Command Dials")},
		{PTP_DPC_NIKON_ReverseCommandDial,		/* 0xD086 */
		 N_("Reverse Command Dial")},
		{PTP_DPC_NIKON_ApertureSetting,			/* 0xD087 */
		 N_("Aperture Setting")},
		{PTP_DPC_NIKON_MenusAndPlayback,		/* 0xD088 */
		 N_("Menus and Playback")},
		{PTP_DPC_NIKON_F6ButtonsAndDials,		/* 0xD089 */
		 N_("Buttons and Dials")},
		{PTP_DPC_NIKON_NoCFCard,			/* 0xD08a */
		 N_("No CF Card Release")},
		{PTP_DPC_NIKON_ImageRotation,			/* 0xD092 */
		 N_("Image Rotation")},
		{PTP_DPC_NIKON_Bracketing,			/* 0xD0c0 */
		 N_("Exposure Bracketing")},
		{PTP_DPC_NIKON_ExposureBracketingIntervalDist,	/* 0xD0c1 */
		 N_("Exposure Bracketing Distance")},
		{PTP_DPC_NIKON_BracketingProgram,		/* 0xD0c2 */
		 N_("Exposure Bracketing Number")},
		{PTP_DPC_NIKON_AutofocusLCDTopMode2,		/* 0xD107 */
		 N_("AF LCD Top Mode 2")},
		{PTP_DPC_NIKON_AutofocusArea,			/* 0xD108 */
		 N_("Active AF Sensor")},
		{PTP_DPC_NIKON_LightMeter,			/* 0xD10a */
		 N_("Exposure Meter")},
		{PTP_DPC_NIKON_ExposureApertureLock,		/* 0xD111 */
		 N_("Exposure Aperture Lock")},
		{PTP_DPC_NIKON_MaximumShots,			/* 0xD103 */
		 N_("Maximum Shots")},
		{PTP_DPC_NIKON_OptimizeImage,			/* 0xD140 */
		 N_("Optimize Image")},
		{PTP_DPC_NIKON_Saturation,			/* 0xD142 */
		 N_("Saturation")},
		{PTP_DPC_NIKON_CSMMenu,				/* 0xD180 */
		 N_("CSM Menu")},
		{PTP_DPC_NIKON_BeepOff,
		 N_("AF Beep Mode")},
		{PTP_DPC_NIKON_AutofocusMode,
		 N_("Autofocus Mode")},
		{PTP_DPC_NIKON_AFAssist,
		 N_("AF Assist Lamp")},
		{PTP_DPC_NIKON_PADVPMode,
		 N_("Auto ISO P/A/DVP Setting")},
		{PTP_DPC_NIKON_ImageReview,
		 N_("Image Review")},
		{PTP_DPC_NIKON_GridDisplay,
		 N_("Viewfinder Grid Display")},
		{PTP_DPC_NIKON_AFAreaIllumination,
		 N_("AF Area Illumination")},
		{PTP_DPC_NIKON_FlashMode,
		 N_("Flash Mode")},
		{PTP_DPC_NIKON_FlashModeManualPower,
		 N_("Flash Mode Manual Power")},
		{PTP_DPC_NIKON_FlashSign,
		 N_("Flash Sign")},
		{PTP_DPC_NIKON_FlashExposureCompensation,
		 N_("Flash Exposure Compensation")},
		{PTP_DPC_NIKON_RemoteTimeout,
		 N_("Remote Timeout")},
		{PTP_DPC_NIKON_ImageCommentString,
		 N_("Image Comment String")},
		{PTP_DPC_NIKON_FlashOpen,
		 N_("Flash Open")},
		{PTP_DPC_NIKON_FlashCharged,
		 N_("Flash Charged")},
		{PTP_DPC_NIKON_LensID,
		 N_("Lens ID")},
		{PTP_DPC_NIKON_FocalLengthMin,
		 N_("Min. Focal Length")},
		{PTP_DPC_NIKON_FocalLengthMax,
		 N_("Max. Focal Length")},
		{PTP_DPC_NIKON_MaxApAtMinFocalLength,
		 N_("Max. Aperture at Min. Focal Length")},
		{PTP_DPC_NIKON_MaxApAtMaxFocalLength,
		 N_("Max. Aperture at Max. Focal Length")},
		{PTP_DPC_NIKON_LowLight,
		 N_("Low Light")},
		{PTP_DPC_NIKON_ACPower, N_("AC Power")},
		{PTP_DPC_NIKON_BracketingSet, N_("NIKON Auto Bracketing Set")},
		{PTP_DPC_NIKON_WhiteBalanceBracketStep, N_("NIKON White Balance Bracket Step")},
		{PTP_DPC_NIKON_AFLLock, N_("NIKON AF-L Locked")},
		{0,NULL}
	};
        struct {
		uint16_t dpc;
		const char *txt;
        } ptp_device_properties_MTP[] = {
		{PTP_DPC_MTP_SecureTime,        N_("Secure Time")},
		{PTP_DPC_MTP_DeviceCertificate, N_("Device Certificate")},
		{PTP_DPC_MTP_SynchronizationPartner,
		 N_("Synchronization Partner")},
		{PTP_DPC_MTP_DeviceFriendlyName,
		 N_("Friendly Device Name")},
		{PTP_DPC_MTP_VolumeLevel,       N_("Volume Level")},
		{PTP_DPC_MTP_DeviceIcon,        N_("Device Icon")},
		{PTP_DPC_MTP_PlaybackRate,      N_("Playback Rate")},
		{PTP_DPC_MTP_PlaybackObject,    N_("Playback Object")},
		{PTP_DPC_MTP_PlaybackContainerIndex,
		 N_("Playback Container Index")},
		{PTP_DPC_MTP_PlaybackPosition,  N_("Playback Position")},
		{PTP_DPC_MTP_RevocationInfo,    N_("Revocation Info")},
		{PTP_DPC_MTP_PlaysForSureID,    N_("PlaysForSure ID")},
		{0,NULL}
        };

	for (i=0; ptp_device_properties[i].txt!=NULL; i++)
		if (ptp_device_properties[i].dpc==dpc)
			return (ptp_device_properties[i].txt);

	if (params->deviceinfo.VendorExtensionID==PTP_VENDOR_MICROSOFT)
		for (i=0; ptp_device_properties_MTP[i].txt!=NULL; i++)
			if (ptp_device_properties_MTP[i].dpc==dpc)
				return (ptp_device_properties_MTP[i].txt);

	if (params->deviceinfo.VendorExtensionID==PTP_VENDOR_EASTMAN_KODAK)
		for (i=0; ptp_device_properties_EK[i].txt!=NULL; i++)
			if (ptp_device_properties_EK[i].dpc==dpc)
				return (ptp_device_properties_EK[i].txt);

	if (params->deviceinfo.VendorExtensionID==PTP_VENDOR_CANON)
		for (i=0; ptp_device_properties_Canon[i].txt!=NULL; i++)
			if (ptp_device_properties_Canon[i].dpc==dpc)
				return (ptp_device_properties_Canon[i].txt);

	if (params->deviceinfo.VendorExtensionID==PTP_VENDOR_NIKON)
		for (i=0; ptp_device_properties_Nikon[i].txt!=NULL; i++)
			if (ptp_device_properties_Nikon[i].dpc==dpc)
				return (ptp_device_properties_Nikon[i].txt);

	return NULL;
}

static int64_t
_value_to_num(PTPPropertyValue *data, uint16_t dt) {
	if (dt == PTP_DTC_STR) {
		if (!data->str)
			return 0;
		return atol(data->str);
	}
	if (dt & PTP_DTC_ARRAY_MASK) {
		return 0;
	} else {
		switch (dt) {
		case PTP_DTC_UNDEF: 
			return 0;
		case PTP_DTC_INT8:
			return data->i8;
		case PTP_DTC_UINT8:
			return data->u8;
		case PTP_DTC_INT16:
			return data->i16;
		case PTP_DTC_UINT16:
			return data->u16;
		case PTP_DTC_INT32:
			return data->i32;
		case PTP_DTC_UINT32:
			return data->u32;
	/*
		PTP_DTC_INT64           
		PTP_DTC_UINT64         
		PTP_DTC_INT128        
		PTP_DTC_UINT128      
	*/
		default:
			return 0;
		}
	}

	return 0;
}

#define PTP_VAL_BOOL(dpc) {dpc, 0, N_("Off")}, {dpc, 1, N_("On")}
#define PTP_VAL_RBOOL(dpc) {dpc, 0, N_("On")}, {dpc, 1, N_("Off")}
#define PTP_VAL_YN(dpc) {dpc, 0, N_("No")}, {dpc, 1, N_("Yes")}

int
ptp_render_property_value(PTPParams* params, uint16_t dpc,
			  PTPDevicePropDesc *dpd, int length, char *out)
{
	int i;

	struct {
		uint16_t dpc;
		double coef;
		double bias;
		const char *format;
	} ptp_value_trans[] = {
		{PTP_DPC_ExposureIndex, 1.0, 0.0, "ISO %.0f"},
		{0, 0.0, 0.0, NULL}
	};

	struct {
		uint16_t dpc;
		double coef;
		double bias;
		const char *format;
	} ptp_value_trans_Nikon[] = {
		{PTP_DPC_BatteryLevel, 1.0, 0.0, "%.0f%%"},
		{PTP_DPC_FNumber, 0.01, 0.0, "f/%.2g"},
		{PTP_DPC_FocalLength, 0.01, 0.0, "%.0f mm"},
		{PTP_DPC_ExposureTime, 0.00001, 0.0, "%.2g sec"},
		{PTP_DPC_ExposureBiasCompensation, 0.001, 0.0, N_("%.1f stops")},
		{PTP_DPC_NIKON_LightMeter, 0.08333, 0.0, N_("%.1f stops")},
		{PTP_DPC_NIKON_FlashExposureCompensation, 0.16666, 0.0, N_("%.1f stops")},
		{PTP_DPC_NIKON_CenterWeightArea, 2.0, 6.0, N_("%.0f mm")},
		{PTP_DPC_NIKON_FocalLengthMin, 0.01, 0.0, "%.0f mm"},
		{PTP_DPC_NIKON_FocalLengthMax, 0.01, 0.0, "%.0f mm"},
		{PTP_DPC_NIKON_MaxApAtMinFocalLength, 0.01, 0.0, "f/%.2g"},
		{PTP_DPC_NIKON_MaxApAtMaxFocalLength, 0.01, 0.0, "f/%.2g"},
		{0, 0.0, 0.0, NULL}
	};

	struct {
		uint16_t dpc;
		int64_t key;
		char *value;
	} ptp_value_list_Nikon[] = {
		{PTP_DPC_CompressionSetting, 0, N_("JPEG Basic")},
		{PTP_DPC_CompressionSetting, 1, N_("JPEG Norm")},
		{PTP_DPC_CompressionSetting, 2, N_("JPEG Fine")},
		{PTP_DPC_CompressionSetting, 4, N_("RAW")},
		{PTP_DPC_CompressionSetting, 5, N_("RAW + JPEG Basic")},
		{PTP_DPC_WhiteBalance, 2, N_("Auto")},
		{PTP_DPC_WhiteBalance, 6, N_("Incandescent")},
		{PTP_DPC_WhiteBalance, 5, N_("Fluorescent")},
		{PTP_DPC_WhiteBalance, 4, N_("Daylight")},
		{PTP_DPC_WhiteBalance, 7, N_("Flash")},
		{PTP_DPC_WhiteBalance, 32784, N_("Cloudy")},
		{PTP_DPC_WhiteBalance, 32785, N_("Shade")},
		{PTP_DPC_WhiteBalance, 32786, N_("Color Temperature")},
		{PTP_DPC_WhiteBalance, 32787, N_("Preset")},
		{PTP_DPC_FlashMode, 32784, N_("Default")},
		{PTP_DPC_FlashMode, 4, N_("Red-eye Reduction")},
		{PTP_DPC_FlashMode, 32787, N_("Red-eye Reduction + Slow Sync")},
		{PTP_DPC_FlashMode, 32785, N_("Slow Sync")},
		{PTP_DPC_FlashMode, 32785, N_("Rear Curtain Sync + Slow Sync")},
		{PTP_DPC_FocusMeteringMode, 2, N_("Dynamic Area")},
		{PTP_DPC_FocusMeteringMode, 32784, N_("Single Area")},
		{PTP_DPC_FocusMeteringMode, 32785, N_("Closest Subject")},
		{PTP_DPC_FocusMeteringMode, 32786, N_("Group Dynamic")},
		{PTP_DPC_FocusMode, 1, N_("Manual Focus")},
		{PTP_DPC_FocusMode, 32784, "AF-S"},
		{PTP_DPC_FocusMode, 32785, "AF-C"},
		PTP_VAL_BOOL(PTP_DPC_NIKON_ISOAuto),
		PTP_VAL_BOOL(PTP_DPC_NIKON_ExposureCompensation),
		PTP_VAL_BOOL(PTP_DPC_NIKON_AELockMode),
		{PTP_DPC_NIKON_AELAFLMode, 0, N_("AE/AF Lock")},
		{PTP_DPC_NIKON_AELAFLMode, 1, N_("AF Lock only")},
		{PTP_DPC_NIKON_AELAFLMode, 2, N_("AE Lock only")},
		{PTP_DPC_NIKON_AELAFLMode, 3, N_("AF Lock Hold")},
		{PTP_DPC_NIKON_AELAFLMode, 4, N_("AF On")},
		{PTP_DPC_NIKON_AELAFLMode, 5, N_("Flash Lock")},
		{PTP_DPC_ExposureMeteringMode, 2, N_("Center Weighted")},
		{PTP_DPC_ExposureMeteringMode, 3, N_("Matrix")},
		{PTP_DPC_ExposureMeteringMode, 4, N_("Spot")},
		{PTP_DPC_ExposureProgramMode, 1, "M"},
		{PTP_DPC_ExposureProgramMode, 3, "A"},
		{PTP_DPC_ExposureProgramMode, 4, "S"},
		{PTP_DPC_ExposureProgramMode, 2, "P"},
		{PTP_DPC_ExposureProgramMode, 32784, N_("Auto")},
		{PTP_DPC_ExposureProgramMode, 32785, N_("Portrait")},
		{PTP_DPC_ExposureProgramMode, 32786, N_("Landscape")},
		{PTP_DPC_ExposureProgramMode, 32787, N_("Macro")},
		{PTP_DPC_ExposureProgramMode, 32788, N_("Sports")},
		{PTP_DPC_ExposureProgramMode, 32790, N_("Night Landscape")},
		{PTP_DPC_ExposureProgramMode, 32789, N_("Night Portrait")},
		{PTP_DPC_StillCaptureMode, 1, N_("Single Shot")},
		{PTP_DPC_StillCaptureMode, 2, N_("Power Wind")},
		{PTP_DPC_StillCaptureMode, 32784, N_("Continuous Low Speed")},
		{PTP_DPC_StillCaptureMode, 32785, N_("Timer")},
		{PTP_DPC_StillCaptureMode, 32787, N_("Remote")},
		{PTP_DPC_StillCaptureMode, 32787, N_("Mirror Up")},
		{PTP_DPC_StillCaptureMode, 32788, N_("Timer + Remote")},
		PTP_VAL_BOOL(PTP_DPC_NIKON_AutofocusMode),
		PTP_VAL_RBOOL(PTP_DPC_NIKON_AFAssist),
		PTP_VAL_RBOOL(PTP_DPC_NIKON_ImageReview),
		PTP_VAL_BOOL(PTP_DPC_NIKON_GridDisplay),
		{PTP_DPC_NIKON_AFAreaIllumination, 0, N_("Auto")},
		{PTP_DPC_NIKON_AFAreaIllumination, 1, N_("Off")},
		{PTP_DPC_NIKON_AFAreaIllumination, 2, N_("On")},
		{PTP_DPC_NIKON_ColorModel, 0, "sRGB"},
		{PTP_DPC_NIKON_ColorModel, 1, "AdobeRGB"},
		{PTP_DPC_NIKON_ColorModel, 2, "sRGB"},
		{PTP_DPC_NIKON_FlashMode, 0, "iTTL"},
		{PTP_DPC_NIKON_FlashMode, 1, N_("Manual")},
		{PTP_DPC_NIKON_FlashMode, 2, N_("Commander")},
		{PTP_DPC_NIKON_FlashModeManualPower, 0, N_("Full")},
		{PTP_DPC_NIKON_FlashModeManualPower, 1, "1/2"},
		{PTP_DPC_NIKON_FlashModeManualPower, 2, "1/4"},
		{PTP_DPC_NIKON_FlashModeManualPower, 3, "1/8"},
		{PTP_DPC_NIKON_FlashModeManualPower, 4, "1/16"},
		PTP_VAL_RBOOL(PTP_DPC_NIKON_FlashSign),
		{PTP_DPC_NIKON_RemoteTimeout, 0, N_("1 min")},
		{PTP_DPC_NIKON_RemoteTimeout, 1, N_("5 mins")},
		{PTP_DPC_NIKON_RemoteTimeout, 2, N_("10 mins")},
		{PTP_DPC_NIKON_RemoteTimeout, 3, N_("15 mins")},
		PTP_VAL_YN(PTP_DPC_NIKON_FlashOpen),
		PTP_VAL_YN(PTP_DPC_NIKON_FlashCharged),
		PTP_VAL_BOOL(PTP_DPC_NIKON_LongExposureNoiseReduction),
		PTP_VAL_BOOL(PTP_DPC_NIKON_FileNumberSequence),
		PTP_VAL_BOOL(PTP_DPC_NIKON_ReverseCommandDial),
		PTP_VAL_RBOOL(PTP_DPC_NIKON_NoCFCard),
		PTP_VAL_RBOOL(PTP_DPC_NIKON_ImageRotation),
		PTP_VAL_BOOL(PTP_DPC_NIKON_Bracketing),
		{PTP_DPC_NIKON_AutofocusArea, 0, N_("Centre")},
		{PTP_DPC_NIKON_AutofocusArea, 1, N_("Top")},
		{PTP_DPC_NIKON_AutofocusArea, 2, N_("Bottom")},
		{PTP_DPC_NIKON_AutofocusArea, 3, N_("Left")},
		{PTP_DPC_NIKON_AutofocusArea, 4, N_("Right")},
		{PTP_DPC_NIKON_OptimizeImage, 0, N_("Normal")},
		{PTP_DPC_NIKON_OptimizeImage, 1, N_("Vivid")},
		{PTP_DPC_NIKON_OptimizeImage, 2, N_("Sharper")},
		{PTP_DPC_NIKON_OptimizeImage, 3, N_("Softer")},
		{PTP_DPC_NIKON_OptimizeImage, 4, N_("Direct Print")},
		{PTP_DPC_NIKON_OptimizeImage, 5, N_("Portrait")},
		{PTP_DPC_NIKON_OptimizeImage, 6, N_("Landscape")},
		{PTP_DPC_NIKON_OptimizeImage, 7, N_("Custom")},

		{PTP_DPC_NIKON_ImageSharpening, 0, N_("Auto")},
		{PTP_DPC_NIKON_ImageSharpening, 1, N_("Normal")},
		{PTP_DPC_NIKON_ImageSharpening, 2, N_("Low")},
		{PTP_DPC_NIKON_ImageSharpening, 3, N_("Medium Low")},
		{PTP_DPC_NIKON_ImageSharpening, 4, N_("Medium high")},
		{PTP_DPC_NIKON_ImageSharpening, 5, N_("High")},
		{PTP_DPC_NIKON_ImageSharpening, 6, N_("None")},

		{PTP_DPC_NIKON_ToneCompensation, 0, N_("Auto")},
		{PTP_DPC_NIKON_ToneCompensation, 1, N_("Normal")},
		{PTP_DPC_NIKON_ToneCompensation, 2, N_("Low contrast")},
		{PTP_DPC_NIKON_ToneCompensation, 3, N_("Medium Low")},
		{PTP_DPC_NIKON_ToneCompensation, 4, N_("Medium High")},
		{PTP_DPC_NIKON_ToneCompensation, 5, N_("High control")},
		{PTP_DPC_NIKON_ToneCompensation, 6, N_("Custom")},

		{PTP_DPC_NIKON_Saturation, 0, N_("Normal")},
		{PTP_DPC_NIKON_Saturation, 1, N_("Moderate")},
		{PTP_DPC_NIKON_Saturation, 2, N_("Enhanced")},

		{PTP_DPC_NIKON_LensID, 0, N_("Unknown")},
		{PTP_DPC_NIKON_LensID, 38, "Sigma 70-300mm 1:4-5.6 D APO Macro"},
		{PTP_DPC_NIKON_LensID, 83, "AF Nikkor 80-200mm 1:2.8 D ED"},
		{PTP_DPC_NIKON_LensID, 118, "AF Nikkor 50mm 1:1.8 D"},
		{PTP_DPC_NIKON_LensID, 127, "AF-S Nikkor 18-70mm 1:3.5-4.5G ED DX"},
		PTP_VAL_YN(PTP_DPC_NIKON_LowLight),
		PTP_VAL_YN(PTP_DPC_NIKON_CSMMenu),
		PTP_VAL_RBOOL(PTP_DPC_NIKON_BeepOff),
		{0, 0, NULL}
	};
	if (params->deviceinfo.VendorExtensionID==PTP_VENDOR_NIKON) {
		int64_t kval;

		for (i=0; ptp_value_trans[i].dpc!=0; i++)
			if (ptp_value_trans[i].dpc==dpc) {
				double value = _value_to_num(&(dpd->CurrentValue), dpd->DataType);

				return snprintf(out, length, 
					_(ptp_value_trans[i].format),
					value * ptp_value_trans[i].coef +
					ptp_value_trans[i].bias);
			}

		for (i=0; ptp_value_trans_Nikon[i].dpc!=0; i++)
			if (ptp_value_trans_Nikon[i].dpc==dpc) {
				double value = _value_to_num(&(dpd->CurrentValue), dpd->DataType);

				return snprintf(out, length, 
					_(ptp_value_trans_Nikon[i].format),
					value * ptp_value_trans_Nikon[i].coef +
					ptp_value_trans_Nikon[i].bias);
			}

		kval = _value_to_num(&(dpd->CurrentValue), dpd->DataType);

		for (i=0; ptp_value_list_Nikon[i].dpc!=0; i++)
			if (ptp_value_list_Nikon[i].dpc==dpc &&
			    ptp_value_list_Nikon[i].key==kval)
				return snprintf(out, length, "%s",
					_(ptp_value_list_Nikon[i].value));
	}
	if (params->deviceinfo.VendorExtensionID==PTP_VENDOR_MICROSOFT) {
		switch (dpc) {
		case PTP_DPC_MTP_SynchronizationPartner:
		case PTP_DPC_MTP_DeviceFriendlyName:
			return snprintf(out, length, "%s", dpd->CurrentValue.str);
		case PTP_DPC_MTP_SecureTime:
		case PTP_DPC_MTP_DeviceCertificate: {
			/* FIXME: Convert to use unicode demux functions */
			for (i=0;(i<dpd->CurrentValue.a.count) && (i<length);i++)
				out[i] = dpd->CurrentValue.a.v[i].u16;
			if (	dpd->CurrentValue.a.count &&
				(dpd->CurrentValue.a.count < length)) {
				out[dpd->CurrentValue.a.count-1] = 0;
				return dpd->CurrentValue.a.count-1;
			} else {
				out[length-1] = 0;
				return length;
			}
			break;
		}
		default:
			break;
		}
	}

	return 0;
}

struct {
	uint16_t ofc;
	const char *format;
} ptp_ofc_trans[] = {
	{PTP_OFC_Undefined,"Undefined Type"},
	{PTP_OFC_Defined,"Defined Type"},
	{PTP_OFC_Association,"Association/Directory"},
	{PTP_OFC_Script,"Script"},
	{PTP_OFC_Executable,"Executable"},
	{PTP_OFC_Text,"Text"},
	{PTP_OFC_HTML,"HTML"},
	{PTP_OFC_DPOF,"DPOF"},
	{PTP_OFC_AIFF,"AIFF"},
	{PTP_OFC_WAV,"MS Wave"},
	{PTP_OFC_MP3,"MP3"},
	{PTP_OFC_AVI,"MS AVI"},
	{PTP_OFC_MPEG,"MPEG"},
	{PTP_OFC_ASF,"ASF"},
	{PTP_OFC_QT,"Apple Quicktime"},
	{PTP_OFC_EXIF_JPEG,"JPEG"},
	{PTP_OFC_TIFF_EP,"TIFF EP"},
	{PTP_OFC_FlashPix,"FlashPix"},
	{PTP_OFC_BMP,"BMP"},
	{PTP_OFC_CIFF,"CIFF"},
	{PTP_OFC_GIF,"GIF"},
	{PTP_OFC_JFIF,"JFIF"},
	{PTP_OFC_PCD,"PCD"},
	{PTP_OFC_PICT,"PICT"},
	{PTP_OFC_PNG,"PNG"},
	{PTP_OFC_TIFF,"TIFF"},
	{PTP_OFC_TIFF_IT,"TIFF_IT"},
	{PTP_OFC_JP2,"JP2"},
	{PTP_OFC_JPX,"JPX"},
};

struct {
	uint16_t ofc;
	const char *format;
} ptp_ofc_mtp_trans[] = {
	{PTP_OFC_MTP_MediaCard,N_("Media Card")},
	{PTP_OFC_MTP_MediaCardGroup,N_("Media Card Group")},
	{PTP_OFC_MTP_Encounter,N_("Encounter")},
	{PTP_OFC_MTP_EncounterBox,N_("Encounter Box")},
	{PTP_OFC_MTP_M4A,N_("M4A")},
	{PTP_OFC_MTP_Firmware,N_("Firmware")},
	{PTP_OFC_MTP_WindowsImageFormat,N_("Windows Image Format")},
	{PTP_OFC_MTP_UndefinedAudio,N_("Undefined Audio")},
	{PTP_OFC_MTP_WMA,"WMA"},
	{PTP_OFC_MTP_OGG,"OGG"},
	{PTP_OFC_MTP_AAC,"AAC"},
	{PTP_OFC_MTP_AudibleCodec,N_("Audible.com Codec")},
	{PTP_OFC_MTP_FLAC,"FLAC"},
	{PTP_OFC_MTP_UndefinedVideo,N_("Undefined Video")},
	{PTP_OFC_MTP_WMV,"WMV"},
	{PTP_OFC_MTP_MP4,"MP4"},
	{PTP_OFC_MTP_MP2,"MP2"},
	{PTP_OFC_MTP_3GP,"3GP"},
	{PTP_OFC_MTP_UndefinedCollection,N_("Undefined Collection")},
	{PTP_OFC_MTP_AbstractMultimediaAlbum,N_("Abstract Multimedia Album")},
	{PTP_OFC_MTP_AbstractImageAlbum,N_("Abstract Image Album")},
	{PTP_OFC_MTP_AbstractAudioAlbum,N_("Abstract Audio Album")},
	{PTP_OFC_MTP_AbstractVideoAlbum,N_("Abstract Video Album")},
	{PTP_OFC_MTP_AbstractAudioVideoPlaylist,N_("Abstract Audio Video Playlist")},
	{PTP_OFC_MTP_AbstractContactGroup,N_("Abstract Contact Group")},
	{PTP_OFC_MTP_AbstractMessageFolder,N_("Abstract Message Folder")},
	{PTP_OFC_MTP_AbstractChapteredProduction,N_("Abstract Chaptered Production")},
	{PTP_OFC_MTP_AbstractAudioPlaylist,N_("Abstract Audio Playlist")},
	{PTP_OFC_MTP_AbstractVideoPlaylist,N_("Abstract Video Playlist")},
	{PTP_OFC_MTP_AbstractMediacast,N_("Abstract Mediacast")},
	{PTP_OFC_MTP_WPLPlaylist,N_("WPL Playlist")},
	{PTP_OFC_MTP_M3UPlaylist,N_("M3U Playlist")},
	{PTP_OFC_MTP_MPLPlaylist,N_("MPL Playlist")},
	{PTP_OFC_MTP_ASXPlaylist,N_("ASX Playlist")},
	{PTP_OFC_MTP_PLSPlaylist,N_("PLS Playlist")},
	{PTP_OFC_MTP_UndefinedDocument,N_("Undefined Document")},
	{PTP_OFC_MTP_AbstractDocument,N_("Abstract Document")},
	{PTP_OFC_MTP_XMLDocument,N_("XMLDocument")},
	{PTP_OFC_MTP_MSWordDocument,N_("Microsoft Word Document")},
	{PTP_OFC_MTP_MHTCompiledHTMLDocument,N_("MHT Compiled HTML Document")},
	{PTP_OFC_MTP_MSExcelSpreadsheetXLS,N_("Microsoft Excel Spreadsheet (.xls)")},
	{PTP_OFC_MTP_MSPowerpointPresentationPPT,N_("Microsoft Powerpoint (.ppt)")},
	{PTP_OFC_MTP_UndefinedMessage,N_("Undefined Message")},
	{PTP_OFC_MTP_AbstractMessage,N_("Abstract Message")},
	{PTP_OFC_MTP_UndefinedContact,N_("Undefined Contact")},
	{PTP_OFC_MTP_AbstractContact,N_("Abstract Contact")},
	{PTP_OFC_MTP_vCard2,N_("vCard2")},
	{PTP_OFC_MTP_vCard3,N_("vCard3")},
	{PTP_OFC_MTP_UndefinedCalendarItem,N_("Undefined Calendar Item")},
	{PTP_OFC_MTP_AbstractCalendarItem,N_("Abstract Calendar Item")},
	{PTP_OFC_MTP_vCalendar1,N_("vCalendar1")},
	{PTP_OFC_MTP_vCalendar2,N_("vCalendar2")},
	{PTP_OFC_MTP_UndefinedWindowsExecutable,N_("Undefined Windows Executable")},
	{PTP_OFC_MTP_MediaCast,N_("Media Cast")},
	{PTP_OFC_MTP_Section,N_("Section")},
};

int
ptp_render_ofc(PTPParams* params, uint16_t ofc, int spaceleft, char *txt)
{
	int i;
	
	if (!(ofc & 0x8000)) {
		for (i=0;i<sizeof(ptp_ofc_trans)/sizeof(ptp_ofc_trans[0]);i++)
			if (ofc == ptp_ofc_trans[i].ofc)
				return snprintf(txt, spaceleft,_(ptp_ofc_trans[i].format));
	} else {
		switch (params->deviceinfo.VendorExtensionID) {
		case PTP_VENDOR_EASTMAN_KODAK:
			switch (ofc) {
			case PTP_OFC_EK_M3U:
				return snprintf (txt, spaceleft,"M3U");
			default:
				break;
			}
			break;
		case PTP_VENDOR_CANON:
			switch (ofc) {
			case PTP_OFC_CANON_CRW:
				return snprintf (txt, spaceleft,"CRW");
			default:
				break;
			}
			break;
		case PTP_VENDOR_MICROSOFT:
			for (i=0;i<sizeof(ptp_ofc_mtp_trans)/sizeof(ptp_ofc_mtp_trans[0]);i++)
				if (ofc == ptp_ofc_mtp_trans[i].ofc)
					return snprintf(txt, spaceleft,_(ptp_ofc_mtp_trans[i].format));
			break;
		default:break;
		}
	}
	return snprintf (txt, spaceleft,_("Unknown(%04x)"), ofc);
}

struct {
	uint16_t opcode;
	const char *name;
} ptp_opcode_trans[] = {
	{PTP_OC_Undefined,N_("Undefined")},
	{PTP_OC_GetDeviceInfo,N_("get device info")},
	{PTP_OC_OpenSession,N_("Open session")},
	{PTP_OC_CloseSession,N_("Close session")},
	{PTP_OC_GetStorageIDs,N_("Get storage IDs")},
	{PTP_OC_GetStorageInfo,N_("Get storage info")},
	{PTP_OC_GetNumObjects,N_("Get number of objects")},
	{PTP_OC_GetObjectHandles,N_("Get object handles")},
	{PTP_OC_GetObjectInfo,N_("Get object info")},
	{PTP_OC_GetObject,N_("Get object")},
	{PTP_OC_GetThumb,N_("Get thumbnail")},
	{PTP_OC_DeleteObject,N_("Delete object")},
	{PTP_OC_SendObjectInfo,N_("Send object info")},
	{PTP_OC_SendObject,N_("Send object")},
	{PTP_OC_InitiateCapture,N_("Initiate capture")},
	{PTP_OC_FormatStore,N_("Format storage")},
	{PTP_OC_ResetDevice,N_("Reset device")},
	{PTP_OC_SelfTest,N_("Self test device")},
	{PTP_OC_SetObjectProtection,N_("Set object protection")},
	{PTP_OC_PowerDown,N_("Power down device")},
	{PTP_OC_GetDevicePropDesc,N_("Get device property description")},
	{PTP_OC_GetDevicePropValue,N_("Get device property value")},
	{PTP_OC_SetDevicePropValue,N_("Set device property value")},
	{PTP_OC_ResetDevicePropValue,N_("Reset device property value")},
	{PTP_OC_TerminateOpenCapture,N_("Terminate open capture")},
	{PTP_OC_MoveObject,N_("Move object")},
	{PTP_OC_CopyObject,N_("Copy object")},
	{PTP_OC_GetPartialObject,N_("Get partial object")},
	{PTP_OC_InitiateOpenCapture,N_("Initiate open capture")}
};

struct {
	uint16_t opcode;
	const char *name;
} ptp_opcode_mtp_trans[] = {
	{PTP_OC_MTP_GetObjectPropsSupported,N_("Get object properties supported")},
	{PTP_OC_MTP_GetObjectPropDesc,N_("Get object property description")},
	{PTP_OC_MTP_GetObjectPropValue,N_("Get object property value")},
	{PTP_OC_MTP_SetObjectPropValue,N_("Set object property value")},
	{PTP_OC_MTP_GetObjPropList,N_("Get object property list")},
	{PTP_OC_MTP_SetObjPropList,N_("Set object property list")},
	{PTP_OC_MTP_GetInterdependendPropdesc,N_("Get interdependent property description")},
	{PTP_OC_MTP_SendObjectPropList,N_("Send object property list")},
	{PTP_OC_MTP_GetObjectReferences,N_("Get object references")},
	{PTP_OC_MTP_SetObjectReferences,N_("Set object references")},
	{PTP_OC_MTP_UpdateDeviceFirmware,N_("Update device firmware")},
	{PTP_OC_MTP_Skip,N_("Skip to next position in playlist")},

	/* WMDRMPD Extensions */
	{PTP_OC_MTP_WMDRMPD_GetSecureTimeChallenge,N_("Get secure time challenge")},
	{PTP_OC_MTP_WMDRMPD_GetSecureTimeResponse,N_("Get secure time response")},
	{PTP_OC_MTP_WMDRMPD_SetLicenseResponse,N_("Set license response")},
	{PTP_OC_MTP_WMDRMPD_GetSyncList,N_("Get sync list")},
	{PTP_OC_MTP_WMDRMPD_SendMeterChallengeQuery,N_("Send meter challenge query")},
	{PTP_OC_MTP_WMDRMPD_GetMeterChallenge,N_("Get meter challenge")},
	{PTP_OC_MTP_WMDRMPD_SetMeterResponse,N_("Get meter response")},
	{PTP_OC_MTP_WMDRMPD_CleanDataStore,N_("Clean data store")},
	{PTP_OC_MTP_WMDRMPD_GetLicenseState,N_("Get license state")},
	{PTP_OC_MTP_WMDRMPD_SendWMDRMPDCommand,N_("Send WMDRM-PD Command")},
	{PTP_OC_MTP_WMDRMPD_SendWMDRMPDRequest,N_("Send WMDRM-PD Request")},

	/* WMPPD Extensions */
	{PTP_OC_MTP_WMPPD_ReportAddedDeletedItems,N_("Report Added/Deleted Items")},
	{PTP_OC_MTP_WMPPD_ReportAcquiredItems,N_("Report Acquired Items")},
	{PTP_OC_MTP_WMPPD_PlaylistObjectPref,N_("Get transferable playlist types")},

	/* WMDRMPD Extensions... these have no identifiers associated with them */
	{PTP_OC_MTP_WMDRMPD_SendWMDRMPDAppRequest,N_("Send WMDRM-PD Application Request")},
	{PTP_OC_MTP_WMDRMPD_GetWMDRMPDAppResponse,N_("Get WMDRM-PD Application Response")},
	{PTP_OC_MTP_WMDRMPD_EnableTrustedFilesOperations,N_("Enable trusted file operations")},
	{PTP_OC_MTP_WMDRMPD_DisableTrustedFilesOperations,N_("Disable trusted file operations")},
	{PTP_OC_MTP_WMDRMPD_EndTrustedAppSession,N_("End trusted application session")},

	/* AAVT Extensions */
	{PTP_OC_MTP_AAVT_OpenMediaSession,N_("Open Media Session")},
	{PTP_OC_MTP_AAVT_CloseMediaSession,N_("Close Media Session")},
	{PTP_OC_MTP_AAVT_GetNextDataBlock,N_("Get Next Data Block")},
	{PTP_OC_MTP_AAVT_SetCurrentTimePosition,N_("Set Current Time Position")},

	/* WMDRMND Extensions */
	{PTP_OC_MTP_WMDRMND_SendRegistrationRequest,N_("Send Registration Request")},
	{PTP_OC_MTP_WMDRMND_GetRegistrationResponse,N_("Get Registration Response")},
	{PTP_OC_MTP_WMDRMND_GetProximityChallenge,N_("Get Proximity Challenge")},
	{PTP_OC_MTP_WMDRMND_SendProximityResponse,N_("Send Proximity Response")},
	{PTP_OC_MTP_WMDRMND_SendWMDRMNDLicenseRequest,N_("Send WMDRM-ND License Request")},
	{PTP_OC_MTP_WMDRMND_GetWMDRMNDLicenseResponse,N_("Get WMDRM-ND License Response")},

	/* WiFi Provisioning MTP Extension Codes (microsoft.com/WPDWCN: 1.0) */
	{PTP_OC_MTP_WPDWCN_ProcessWFCObject,N_("Process WFC Object")}
};

int
ptp_render_opcode(PTPParams* params, uint16_t opcode, int spaceleft, char *txt)
{
	int i;

	if (!(opcode & 0x8000)) {
		for (i=0;i<sizeof(ptp_opcode_trans)/sizeof(ptp_opcode_trans[0]);i++)
			if (opcode == ptp_opcode_trans[i].opcode)
				return snprintf(txt, spaceleft,_(ptp_opcode_trans[i].name));
	} else {
		switch (params->deviceinfo.VendorExtensionID) {
		case PTP_VENDOR_MICROSOFT:
			for (i=0;i<sizeof(ptp_opcode_mtp_trans)/sizeof(ptp_opcode_mtp_trans[0]);i++)
				if (opcode == ptp_opcode_mtp_trans[i].opcode)
					return snprintf(txt, spaceleft,_(ptp_opcode_mtp_trans[i].name));
			break;
		default:break;
		}
	}
	return snprintf (txt, spaceleft,_("Unknown (%04x)"), opcode);
}


struct {
	uint16_t id;
	const char *name;
} ptp_opc_trans[] = {
	{PTP_OPC_StorageID,"StorageID"},
	{PTP_OPC_ObjectFormat,"ObjectFormat"},
	{PTP_OPC_ProtectionStatus,"ProtectionStatus"},
	{PTP_OPC_ObjectSize,"ObjectSize"},
	{PTP_OPC_AssociationType,"AssociationType"},
	{PTP_OPC_AssociationDesc,"AssociationDesc"},
	{PTP_OPC_ObjectFileName,"ObjectFileName"},
	{PTP_OPC_DateCreated,"DateCreated"},
	{PTP_OPC_DateModified,"DateModified"},
	{PTP_OPC_Keywords,"Keywords"},
	{PTP_OPC_ParentObject,"ParentObject"},
	{PTP_OPC_AllowedFolderContents,"AllowedFolderContents"},
	{PTP_OPC_Hidden,"Hidden"},
	{PTP_OPC_SystemObject,"SystemObject"},
	{PTP_OPC_PersistantUniqueObjectIdentifier,"PersistantUniqueObjectIdentifier"},
	{PTP_OPC_SyncID,"SyncID"},
	{PTP_OPC_PropertyBag,"PropertyBag"},
	{PTP_OPC_Name,"Name"},
	{PTP_OPC_CreatedBy,"CreatedBy"},
	{PTP_OPC_Artist,"Artist"},
	{PTP_OPC_DateAuthored,"DateAuthored"},
	{PTP_OPC_Description,"Description"},
	{PTP_OPC_URLReference,"URLReference"},
	{PTP_OPC_LanguageLocale,"LanguageLocale"},
	{PTP_OPC_CopyrightInformation,"CopyrightInformation"},
	{PTP_OPC_Source,"Source"},
	{PTP_OPC_OriginLocation,"OriginLocation"},
	{PTP_OPC_DateAdded,"DateAdded"},
	{PTP_OPC_NonConsumable,"NonConsumable"},
	{PTP_OPC_CorruptOrUnplayable,"CorruptOrUnplayable"},
	{PTP_OPC_ProducerSerialNumber,"ProducerSerialNumber"},
	{PTP_OPC_RepresentativeSampleFormat,"RepresentativeSampleFormat"},
	{PTP_OPC_RepresentativeSampleSize,"RepresentativeSampleSize"},
	{PTP_OPC_RepresentativeSampleHeight,"RepresentativeSampleHeight"},
	{PTP_OPC_RepresentativeSampleWidth,"RepresentativeSampleWidth"},
	{PTP_OPC_RepresentativeSampleDuration,"RepresentativeSampleDuration"},
	{PTP_OPC_RepresentativeSampleData,"RepresentativeSampleData"},
	{PTP_OPC_Width,"Width"},
	{PTP_OPC_Height,"Height"},
	{PTP_OPC_Duration,"Duration"},
	{PTP_OPC_Rating,"Rating"},
	{PTP_OPC_Track,"Track"},
	{PTP_OPC_Genre,"Genre"},
	{PTP_OPC_Credits,"Credits"},
	{PTP_OPC_Lyrics,"Lyrics"},
	{PTP_OPC_SubscriptionContentID,"SubscriptionContentID"},
	{PTP_OPC_ProducedBy,"ProducedBy"},
	{PTP_OPC_UseCount,"UseCount"},
	{PTP_OPC_SkipCount,"SkipCount"},
	{PTP_OPC_LastAccessed,"LastAccessed"},
	{PTP_OPC_ParentalRating,"ParentalRating"},
	{PTP_OPC_MetaGenre,"MetaGenre"},
	{PTP_OPC_Composer,"Composer"},
	{PTP_OPC_EffectiveRating,"EffectiveRating"},
	{PTP_OPC_Subtitle,"Subtitle"},
	{PTP_OPC_OriginalReleaseDate,"OriginalReleaseDate"},
	{PTP_OPC_AlbumName,"AlbumName"},
	{PTP_OPC_AlbumArtist,"AlbumArtist"},
	{PTP_OPC_Mood,"Mood"},
	{PTP_OPC_DRMStatus,"DRMStatus"},
	{PTP_OPC_SubDescription,"SubDescription"},
	{PTP_OPC_IsCropped,"IsCropped"},
	{PTP_OPC_IsColorCorrected,"IsColorCorrected"},
	{PTP_OPC_ImageBitDepth,"ImageBitDepth"},
	{PTP_OPC_Fnumber,"Fnumber"},
	{PTP_OPC_ExposureTime,"ExposureTime"},
	{PTP_OPC_ExposureIndex,"ExposureIndex"},
	{PTP_OPC_DisplayName,"DisplayName"},
	{PTP_OPC_BodyText,"BodyText"},
	{PTP_OPC_Subject,"Subject"},
	{PTP_OPC_Priority,"Priority"},
	{PTP_OPC_GivenName,"GivenName"},
	{PTP_OPC_MiddleNames,"MiddleNames"},
	{PTP_OPC_FamilyName,"FamilyName"},

	{PTP_OPC_Prefix,"Prefix"},
	{PTP_OPC_Suffix,"Suffix"},
	{PTP_OPC_PhoneticGivenName,"PhoneticGivenName"},
	{PTP_OPC_PhoneticFamilyName,"PhoneticFamilyName"},
	{PTP_OPC_EmailPrimary,"EmailPrimary"},
	{PTP_OPC_EmailPersonal1,"EmailPersonal1"},
	{PTP_OPC_EmailPersonal2,"EmailPersonal2"},
	{PTP_OPC_EmailBusiness1,"EmailBusiness1"},
	{PTP_OPC_EmailBusiness2,"EmailBusiness2"},
	{PTP_OPC_EmailOthers,"EmailOthers"},
	{PTP_OPC_PhoneNumberPrimary,"PhoneNumberPrimary"},
	{PTP_OPC_PhoneNumberPersonal,"PhoneNumberPersonal"},
	{PTP_OPC_PhoneNumberPersonal2,"PhoneNumberPersonal2"},
	{PTP_OPC_PhoneNumberBusiness,"PhoneNumberBusiness"},
	{PTP_OPC_PhoneNumberBusiness2,"PhoneNumberBusiness2"},
	{PTP_OPC_PhoneNumberMobile,"PhoneNumberMobile"},
	{PTP_OPC_PhoneNumberMobile2,"PhoneNumberMobile2"},
	{PTP_OPC_FaxNumberPrimary,"FaxNumberPrimary"},
	{PTP_OPC_FaxNumberPersonal,"FaxNumberPersonal"},
	{PTP_OPC_FaxNumberBusiness,"FaxNumberBusiness"},
	{PTP_OPC_PagerNumber,"PagerNumber"},
	{PTP_OPC_PhoneNumberOthers,"PhoneNumberOthers"},
	{PTP_OPC_PrimaryWebAddress,"PrimaryWebAddress"},
	{PTP_OPC_PersonalWebAddress,"PersonalWebAddress"},
	{PTP_OPC_BusinessWebAddress,"BusinessWebAddress"},
	{PTP_OPC_InstantMessengerAddress,"InstantMessengerAddress"},
	{PTP_OPC_InstantMessengerAddress2,"InstantMessengerAddress2"},
	{PTP_OPC_InstantMessengerAddress3,"InstantMessengerAddress3"},
	{PTP_OPC_PostalAddressPersonalFull,"PostalAddressPersonalFull"},
	{PTP_OPC_PostalAddressPersonalFullLine1,"PostalAddressPersonalFullLine1"},
	{PTP_OPC_PostalAddressPersonalFullLine2,"PostalAddressPersonalFullLine2"},
	{PTP_OPC_PostalAddressPersonalFullCity,"PostalAddressPersonalFullCity"},
	{PTP_OPC_PostalAddressPersonalFullRegion,"PostalAddressPersonalFullRegion"},
	{PTP_OPC_PostalAddressPersonalFullPostalCode,"PostalAddressPersonalFullPostalCode"},
	{PTP_OPC_PostalAddressPersonalFullCountry,"PostalAddressPersonalFullCountry"},
	{PTP_OPC_PostalAddressBusinessFull,"PostalAddressBusinessFull"},
	{PTP_OPC_PostalAddressBusinessLine1,"PostalAddressBusinessLine1"},
	{PTP_OPC_PostalAddressBusinessLine2,"PostalAddressBusinessLine2"},
	{PTP_OPC_PostalAddressBusinessCity,"PostalAddressBusinessCity"},
	{PTP_OPC_PostalAddressBusinessRegion,"PostalAddressBusinessRegion"},
	{PTP_OPC_PostalAddressBusinessPostalCode,"PostalAddressBusinessPostalCode"},
	{PTP_OPC_PostalAddressBusinessCountry,"PostalAddressBusinessCountry"},
	{PTP_OPC_PostalAddressOtherFull,"PostalAddressOtherFull"},
	{PTP_OPC_PostalAddressOtherLine1,"PostalAddressOtherLine1"},
	{PTP_OPC_PostalAddressOtherLine2,"PostalAddressOtherLine2"},
	{PTP_OPC_PostalAddressOtherCity,"PostalAddressOtherCity"},
	{PTP_OPC_PostalAddressOtherRegion,"PostalAddressOtherRegion"},
	{PTP_OPC_PostalAddressOtherPostalCode,"PostalAddressOtherPostalCode"},
	{PTP_OPC_PostalAddressOtherCountry,"PostalAddressOtherCountry"},
	{PTP_OPC_OrganizationName,"OrganizationName"},
	{PTP_OPC_PhoneticOrganizationName,"PhoneticOrganizationName"},
	{PTP_OPC_Role,"Role"},
	{PTP_OPC_Birthdate,"Birthdate"},
	{PTP_OPC_MessageTo,"MessageTo"},
	{PTP_OPC_MessageCC,"MessageCC"},
	{PTP_OPC_MessageBCC,"MessageBCC"},
	{PTP_OPC_MessageRead,"MessageRead"},
	{PTP_OPC_MessageReceivedTime,"MessageReceivedTime"},
	{PTP_OPC_MessageSender,"MessageSender"},
	{PTP_OPC_ActivityBeginTime,"ActivityBeginTime"},
	{PTP_OPC_ActivityEndTime,"ActivityEndTime"},
	{PTP_OPC_ActivityLocation,"ActivityLocation"},
	{PTP_OPC_ActivityRequiredAttendees,"ActivityRequiredAttendees"},
	{PTP_OPC_ActivityOptionalAttendees,"ActivityOptionalAttendees"},
	{PTP_OPC_ActivityResources,"ActivityResources"},
	{PTP_OPC_ActivityAccepted,"ActivityAccepted"},
	{PTP_OPC_Owner,"Owner"},
	{PTP_OPC_Editor,"Editor"},
	{PTP_OPC_Webmaster,"Webmaster"},
	{PTP_OPC_URLSource,"URLSource"},
	{PTP_OPC_URLDestination,"URLDestination"},
	{PTP_OPC_TimeBookmark,"TimeBookmark"},
	{PTP_OPC_ObjectBookmark,"ObjectBookmark"},
	{PTP_OPC_ByteBookmark,"ByteBookmark"},
	{PTP_OPC_LastBuildDate,"LastBuildDate"},
	{PTP_OPC_TimetoLive,"TimetoLive"},
	{PTP_OPC_MediaGUID,"MediaGUID"},
	{PTP_OPC_TotalBitRate,"TotalBitRate"},
	{PTP_OPC_BitRateType,"BitRateType"},
	{PTP_OPC_SampleRate,"SampleRate"},
	{PTP_OPC_NumberOfChannels,"NumberOfChannels"},
	{PTP_OPC_AudioBitDepth,"AudioBitDepth"},
	{PTP_OPC_ScanDepth,"ScanDepth"},
	{PTP_OPC_AudioWAVECodec,"AudioWAVECodec"},
	{PTP_OPC_AudioBitRate,"AudioBitRate"},
	{PTP_OPC_VideoFourCCCodec,"VideoFourCCCodec"},
	{PTP_OPC_VideoBitRate,"VideoBitRate"},
	{PTP_OPC_FramesPerThousandSeconds,"FramesPerThousandSeconds"},
	{PTP_OPC_KeyFrameDistance,"KeyFrameDistance"},
	{PTP_OPC_BufferSize,"BufferSize"},
	{PTP_OPC_EncodingQuality,"EncodingQuality"},
	{PTP_OPC_EncodingProfile,"EncodingProfile"},
	{PTP_OPC_BuyFlag,"BuyFlag"},
};

int
ptp_render_mtp_propname(uint16_t propid, int spaceleft, char *txt) {
	int i;
	for (i=0;i<sizeof(ptp_opc_trans)/sizeof(ptp_opc_trans[0]);i++)
		if (propid == ptp_opc_trans[i].id)
			return snprintf(txt, spaceleft,ptp_opc_trans[i].name);
	return snprintf (txt, spaceleft,"unknown(%04x)", propid);
}

/*
 * Allocate and default-initialize a few object properties.
 */
MTPProperties *
ptp_get_new_object_prop_entry(MTPProperties **props, int *nrofprops) {
	MTPProperties *newprops;
	MTPProperties *prop;

	if (*props == NULL) {
		newprops = malloc(sizeof(MTPProperties)*(*nrofprops+1));
	} else {
		newprops = realloc(*props,sizeof(MTPProperties)*(*nrofprops+1));
	}
	if (newprops == NULL)
		return NULL;
	prop = &newprops[*nrofprops];
	prop->property = PTP_OPC_StorageID; /* Should be "unknown" */
	prop->datatype = PTP_DTC_UNDEF;
	prop->ObjectHandle = 0x00000000U;
	prop->propval.str = NULL;
	
	(*props) = newprops;
	(*nrofprops)++;
	return prop;
}

void 
ptp_destroy_object_prop(MTPProperties *prop)
{
  if (!prop)
    return;
  
  if (prop->datatype == PTP_DTC_STR && prop->propval.str != NULL)
    free(prop->propval.str);
  else if ((prop->datatype == PTP_DTC_AINT8 || prop->datatype == PTP_DTC_AINT16 ||
            prop->datatype == PTP_DTC_AINT32 || prop->datatype == PTP_DTC_AINT64 || prop->datatype == PTP_DTC_AINT128 ||
            prop->datatype == PTP_DTC_AUINT8 || prop->datatype == PTP_DTC_AUINT16 ||
            prop->datatype == PTP_DTC_AUINT32 || prop->datatype == PTP_DTC_AUINT64 || prop->datatype ==  PTP_DTC_AUINT128)
            && prop->propval.a.v != NULL)
    free(prop->propval.a.v);
}

void 
ptp_destroy_object_prop_list(MTPProperties *props, int nrofprops)
{
  int i;
  MTPProperties *prop = props;

  for (i=0;i<nrofprops;i++,prop++)
    ptp_destroy_object_prop(prop);
  free(props);
}

/*
 * Find a certain object property in the cache, i.e. a certain metadata
 * item for a certain object handle.
 */
MTPProperties *
ptp_find_object_prop_in_cache(PTPParams *params, uint32_t const handle, uint32_t const attribute_id)
{
	int i;
	MTPProperties *prop = params->props;
	
	if (!prop)
		return NULL;
	
	for (i=0;i<params->nrofprops;i++) {
		if (handle == prop->ObjectHandle && attribute_id == prop->property)
			return prop;
		prop ++;
	}
	return NULL;
}

void
ptp_remove_object_from_cache(PTPParams *params, uint32_t handle)
{
	int i;

	/* remove object from object info cache */
	for (i = 0; i < params->handles.n; i++) {
		if (params->handles.Handler[i] == handle) {
			ptp_free_objectinfo(&params->objectinfo[i]);
			memmove(params->handles.Handler+i, params->handles.Handler+i+1,
				(params->handles.n-i-1)*sizeof(uint32_t));
			memmove(params->objectinfo+i, params->objectinfo+i+1,
				(params->handles.n-i-1)*sizeof(PTPObjectInfo));
			params->handles.n--;
			/* We use less memory than before so this shouldn't fail */
			params->handles.Handler = realloc(params->handles.Handler, sizeof(uint32_t)*params->handles.n);
			params->objectinfo = realloc(params->objectinfo, sizeof(PTPObjectInfo)*params->handles.n);
		}
	}
	
	/* delete cached object properties if metadata cache exists */
	if (params->props != NULL) {
		int nrofoldprops = 0;
		int firstoldprop = 0;
		
		for (i=0; i<params->nrofprops; i++) {
			MTPProperties *prop = &params->props[i];
			if (prop->ObjectHandle == handle)
				{
					nrofoldprops++;
					if (nrofoldprops == 1) {
						firstoldprop = i;
					}
				}
		}
		for (i=firstoldprop;i<(firstoldprop+nrofoldprops);i++) {
			ptp_destroy_object_prop(&params->props[i]);
		}
		memmove(&params->props[firstoldprop], 
			&params->props[firstoldprop+nrofoldprops], 
			(params->nrofprops-firstoldprop-nrofoldprops)*sizeof(MTPProperties));
		/* We use less memory than before so this shouldn't fail */
		params->props = realloc(params->props, 
					(params->nrofprops - nrofoldprops)*sizeof(MTPProperties));
		params->nrofprops -= nrofoldprops;
	}
}

uint16_t
ptp_add_object_to_cache(PTPParams *params, uint32_t handle)
{
	uint32_t n;
	uint32_t *xhandler;
	PTPObjectInfo *xoi;
	
	/* We have a new handle */
	params->handles.n++;
	n = params->handles.n;
	
	/* Insert the new handle */
	xhandler = (uint32_t*) realloc(params->handles.Handler,
				       sizeof(uint32_t)*n);
	if (!xhandler) {
		/* Well, out of memory is no I/O error really... */
		return PTP_ERROR_IO;
	}
	params->handles.Handler = xhandler;
	params->handles.Handler[n-1] = handle;
	
	/* Insert a new object info struct and populate it */
	xoi = (PTPObjectInfo*)realloc(params->objectinfo,
				      sizeof(PTPObjectInfo)*n);
	if (!xoi) {
		/* Well, out of memory is no I/O error really... */
		return PTP_ERROR_IO;
	}
	params->objectinfo = xoi;
	memset(&params->objectinfo[n-1], 0, sizeof(PTPObjectInfo));
	ptp_getobjectinfo(params, handle, &params->objectinfo[n-1]);

	/* Update proplist if we use cached props */
	if (params->props != NULL) {
		MTPProperties *props = NULL;
		MTPProperties *xprops;
		int no_new_props = 0;
		uint16_t ret;
		
		ret = ptp_mtp_getobjectproplist(params, handle, &props, &no_new_props);
		if (ret != PTP_RC_OK) {
			return ret;
		}
		xprops = realloc(params->props, (params->nrofprops+no_new_props)*sizeof(MTPProperties));
		if (!xprops) {
			free(props);
			/* Well, out of memory is no I/O error really... */
			return PTP_ERROR_IO;
		}
		params->props = xprops;
		memcpy(&params->props[params->nrofprops],&props[0],no_new_props*sizeof(MTPProperties));
		/* do not free the sub strings, we copied them above! Only free the array. */
		free(props);
		params->nrofprops += no_new_props;
	}
	return PTP_RC_OK;
}
