/**
 * vim: set ts=4 :
 * =============================================================================
 * SourceMod Sample Extension
 * Copyright (C) 2004-2008 AlliedModders LLC.  All rights reserved.
 * =============================================================================
 *
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3.0, as published by the
 * Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, AlliedModders LLC gives you permission to link the
 * code of this program (as well as its derivative works) to "Half-Life 2," the
 * "Source Engine," the "SourcePawn JIT," and any Game MODs that run on software
 * by the Valve Corporation.  You must obey the GNU General Public License in
 * all respects for all other code used.  Additionally, AlliedModders LLC grants
 * this exception to all derivative works.  AlliedModders LLC defines further
 * exceptions, found in LICENSE.txt (as of this writing, version JULY-31-2007),
 * or <http://www.sourcemod.net/license.php>.
 *
 * Version: $Id$
 */

#include "extension.h"
#include "context.h"
#include "readerwriterqueue.h"
#include <uv.h>
#include <vector>

/**
 * @file extension.cpp
 * @brief Implement extension code here.
 */

#define MAX_IP_BUFFER_LENGTH 64

moodycamel::ReaderWriterQueue<CSocketConnect *> g_ConnectQueue;
moodycamel::ReaderWriterQueue<CSocketError *> g_ErrorQueue;
moodycamel::ReaderWriterQueue<CSocketData *> g_DataQueue;

// A context is never deleted on the UV thread: libuv hands it over here once it is
// done with it, and the game thread performs the actual delete. The queues above hold
// raw context pointers, so deleting on the UV thread would free objects the game
// thread is about to dereference while draining them.
moodycamel::ReaderWriterQueue<CAsyncSocketContext *> g_DeleteQueue;

// Game thread only. Contexts handed over by the UV thread on an earlier frame. They
// are deleted one frame later, after a full drain of the queues above: the UV thread
// always enqueues a callback item before it hands the context over, so by then every
// item that can reference them has been consumed.
std::vector<CAsyncSocketContext *> g_ContextsToDelete;

uv_loop_t *g_UV_Loop;
uv_thread_t g_UV_LoopThread;

uv_async_t g_UV_AsyncAdded;
moodycamel::ReaderWriterQueue<CAsyncAddJob> g_AsyncAddQueue;

bool g_Running;
AsyncSocket g_AsyncSocket;		/**< Global singleton for extension's main interface */

SMEXT_LINK(&g_AsyncSocket);

CAsyncSocketContext *AsyncSocket::GetSocketInstanceByHandle(Handle_t handle)
{
	HandleSecurity sec;
	sec.pOwner = NULL;
	sec.pIdentity = myself->GetIdentity();

	CAsyncSocketContext *pSocketContext;

	if(handlesys->ReadHandle(handle, socketHandleType, &sec, (void **)&pSocketContext) != HandleError_None)
		return NULL;

	return pSocketContext;
}

void AsyncSocket::OnHandleDestroy(HandleType_t type, void *object)
{
	if(object != NULL)
	{
		CAsyncSocketContext *pSocketContext = (CAsyncSocketContext *)object;
		pSocketContext->m_Deleted = true;

		if(g_Running)
		{
			CAsyncAddJob Job;
			Job.CallbackFn = UV_DeleteAsyncContext;
			Job.pData = pSocketContext;
			g_AsyncAddQueue.enqueue(Job);

			uv_async_send(&g_UV_AsyncAdded);
		}
		else
		{
			delete pSocketContext;
		}
	}
}

void UV_CloseOrphanedClient(uv_async_t *pHandle)
{
	uv_handle_t *handle = (uv_handle_t *)pHandle->data;
	uv_close(handle, UV_FreeHandle);
	uv_close((uv_handle_t *)pHandle, pHandle->close_cb);
}

void OnGameFrame(bool simulating)
{
	CSocketConnect *pConnect;
	while(g_ConnectQueue.try_dequeue(pConnect))
	{
		if(!pConnect->pSocketContext->m_Deleted)
		{
			if(pConnect->pSocketContext->m_Server)
			{
				CAsyncSocketContext *pSocketContext = new CAsyncSocketContext(pConnect->pSocketContext->m_pContext);
				pSocketContext->m_Handle = handlesys->CreateHandle(g_AsyncSocket.socketHandleType, pSocketContext,
					pConnect->pSocketContext->m_pContext->GetIdentity(), myself->GetIdentity(), NULL);

				pSocketContext->m_pStream = pConnect->pClientSocket;
				pSocketContext->m_pStream->data = pSocketContext;

				if (pConnect->pClientIP)
				{
					pSocketContext->m_pClientIP = pConnect->pClientIP;
				}
				else
				{
					pSocketContext->m_pClientIP = (char *)malloc(MAX_IP_BUFFER_LENGTH);
					if (pSocketContext->m_pClientIP)
					{
						pSocketContext->m_pClientIP[0] = '\0';
					}
				}

				pConnect->pSocketContext->OnConnect(pSocketContext);

				if(!pSocketContext->m_Deleted)
				{
					CAsyncAddJob Job;
					Job.CallbackFn = UV_StartRead;
					Job.pData = pSocketContext;
					g_AsyncAddQueue.enqueue(Job);

					uv_async_send(&g_UV_AsyncAdded);
				}
			}
			else
			{
				if (pConnect->pClientIP)
				{
					pConnect->pSocketContext->m_pClientIP = pConnect->pClientIP;
				}

				pConnect->pSocketContext->Connected();
			}
		}
		else
		{
			// The listening socket was destroyed while this connection was on its way to
			// us. Only an accepted socket is orphaned here: on an outgoing connection
			// pClientSocket is the context's own handle and UV_DeleteAsyncContext owns it,
			// so closing it here would close it twice.
			if(pConnect->bAccepted && pConnect->pClientSocket)
			{
				CAsyncAddJob Job;
				Job.CallbackFn = UV_CloseOrphanedClient;
				Job.pData = pConnect->pClientSocket;
				g_AsyncAddQueue.enqueue(Job);
				uv_async_send(&g_UV_AsyncAdded);
			}

			if(pConnect->pClientIP)
			{
				free(pConnect->pClientIP);
			}
		}

		free(pConnect);
	}

	CSocketData *pData;
	while (g_DataQueue.try_dequeue(pData))
	{
		if (pData->pSocketContext && !pData->pSocketContext->m_Deleted)
		{
			pData->pSocketContext->OnData(pData->pBuffer, pData->BufferSize);
		}

		free(pData->pBuffer);
		free(pData);
	}

	CSocketError *pError;
	while(g_ErrorQueue.try_dequeue(pError))
	{
		if(!pError->pSocketContext->m_Deleted)
		{
			pError->pSocketContext->OnError(pError->Error);
		}

		free(pError);
	}

	// Everything queued before these contexts were handed over has now been drained,
	// so nothing can reference them anymore.
	for(size_t i = 0; i < g_ContextsToDelete.size(); i++)
	{
		delete g_ContextsToDelete[i];
	}
	g_ContextsToDelete.clear();

	// Collected here, deleted on the next frame: the UV thread may have queued a
	// callback for this context just after this frame drained the queues above.
	CAsyncSocketContext *pDeleteContext;
	while(g_DeleteQueue.try_dequeue(pDeleteContext))
	{
		g_ContextsToDelete.push_back(pDeleteContext);
	}
}

// main event loop thread
void UV_EventLoop(void *data)
{
	uv_run(g_UV_Loop, UV_RUN_DEFAULT);
}

void UV_OnAsyncAdded(uv_async_t *pHandle)
{
	CAsyncAddJob Job;
	while(g_AsyncAddQueue.try_dequeue(Job))
	{
		uv_async_t *pAsync = (uv_async_t *)malloc(sizeof(uv_async_t));
		uv_async_init(g_UV_Loop, pAsync, Job.CallbackFn);
		pAsync->data = Job.pData;
		pAsync->close_cb = UV_FreeHandle;
		uv_async_send(pAsync);
	}
}

void UV_FreeHandle(uv_handle_t *handle)
{
	free(handle);
}

void UV_AllocBuffer(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf)
{
	buf->base = (char *)malloc(suggested_size);
	buf->len = suggested_size;
}

void UV_FreeBuffer(const uv_buf_t *buf)
{
	if(buf && buf->base)
		free(buf->base);
}

void UV_Quit(uv_async_t *pHandle)
{
	uv_close((uv_handle_t *)pHandle, pHandle->close_cb);

	uv_close((uv_handle_t *)&g_UV_AsyncAdded, NULL);

	uv_stop(g_UV_Loop);
}

// UV thread only. Drops one in-flight libuv reference on the context and, once the
// last one is gone and the plugin handle has been destroyed, hands it to the game
// thread for deletion. The caller must not touch the context after calling this.
void UV_ReleaseContext(CAsyncSocketContext *pSocketContext)
{
	if(--pSocketContext->m_UvRefs > 0)
		return;

	if(!pSocketContext->m_DeleteRequested)
		return;

	g_DeleteQueue.enqueue(pSocketContext);
}

void UV_OnContextHandleClosed(uv_handle_t *handle)
{
	CAsyncSocketContext *pSocketContext = (CAsyncSocketContext *)handle->data;
	free(handle);

	UV_ReleaseContext(pSocketContext);
}

void UV_DeleteAsyncContext(uv_async_t *pHandle)
{
	CAsyncSocketContext *pSocketContext = (CAsyncSocketContext *)pHandle->data;
	uv_close((uv_handle_t *)pHandle, pHandle->close_cb);

	pSocketContext->m_DeleteRequested = true;

	// Hold a reference of our own so the context cannot be handed over while this
	// function is still working with it.
	pSocketContext->m_UvRefs++;

	// A DNS lookup is still in flight: this job is queued after the resolve job and
	// libuv runs async callbacks in creation order, so UV_OnAsyncResolve has already
	// run and either started the lookup or cleared m_Pending. Whether uv_cancel()
	// succeeds or not, UV_OnAsyncResolved still runs and releases its reference.
	if(pSocketContext->m_Pending)
		uv_cancel((uv_req_t *)&pSocketContext->m_Resolver);

	if(pSocketContext->m_pClientIP)
	{
		free(pSocketContext->m_pClientIP);
		pSocketContext->m_pClientIP = NULL;
	}

	// For an outgoing connection m_pSocket and m_pStream are the same handle: the
	// resolver allocates it and UV_OnConnect stores req->handle in m_pStream. Resolve
	// the aliasing once, up front, so it cannot be closed twice.
	uv_handle_t *pStream = (uv_handle_t *)pSocketContext->m_pStream;
	uv_handle_t *pSocket = (uv_handle_t *)pSocketContext->m_pSocket;

	if(pSocket == pStream)
		pSocket = NULL;

	pSocketContext->m_pStream = NULL;
	pSocketContext->m_pSocket = NULL;

	if(pStream && !uv_is_closing(pStream))
	{
		pStream->data = pSocketContext;
		pSocketContext->m_UvRefs++;
		uv_close(pStream, UV_OnContextHandleClosed);
	}

	if(pSocket && !uv_is_closing(pSocket))
	{
		pSocket->data = pSocketContext;
		pSocketContext->m_UvRefs++;
		uv_close(pSocket, UV_OnContextHandleClosed);
	}

	UV_ReleaseContext(pSocketContext);
}

void UV_PushError(CAsyncSocketContext *pSocketContext, int error)
{
	pSocketContext->m_PendingCallback = true;
	CSocketError *pError = (CSocketError *)malloc(sizeof(CSocketError));

	pError->pSocketContext = pSocketContext;
	pError->Error = error;

	g_ErrorQueue.enqueue(pError);
}

void UV_OnRead(uv_stream_t *client, ssize_t nread, const uv_buf_t *buf)
{
	CAsyncSocketContext *pSocketContext = (CAsyncSocketContext *)client->data;

	if (!pSocketContext || pSocketContext->m_Deleted)
	{
		UV_FreeBuffer(buf);
		return;
	}

	if (nread < 0)
	{
		UV_FreeBuffer(buf);
		UV_PushError(pSocketContext, (int)nread);
		return;
	}

	if (nread == 0)
	{
		UV_FreeBuffer(buf);
		return;
	}

	char *data = (char *)malloc((size_t)nread + 1);

	if (!data)
	{
		UV_FreeBuffer(buf);
		return;
	}

	memcpy(data, buf->base, (size_t)nread);
	data[nread] = '\0';

	UV_FreeBuffer(buf);

	CSocketData *pData = (CSocketData *)malloc(sizeof(CSocketData));

	if (!pData)
	{
		free(data);
		return;
	}

	if (pSocketContext->m_Deleted)
	{
		free(data);
		free(pData);
		return;
	}

	pData->pSocketContext = pSocketContext;
	pData->pBuffer = data;
	pData->BufferSize = nread;

	g_DataQueue.enqueue(pData);
}

void UV_OnConnect(uv_connect_t *req, int status)
{
	CAsyncSocketContext *pSocketContext = (CAsyncSocketContext *)req->data;
	if(pSocketContext->m_Deleted)
	{
		free(req);
		return;
	}

	if(status < 0)
	{
		// Close the handle of the failed attempt, as the bind/listen error paths do, so
		// the plugin can call Connect() again on this socket and the fd is not kept open.
		uv_handle_t *pSocket = (uv_handle_t *)req->handle;
		free(req);

		if(pSocketContext->m_pSocket == (uv_tcp_t *)pSocket)
			pSocketContext->m_pSocket = NULL;

		if(!uv_is_closing(pSocket))
			uv_close(pSocket, pSocket->close_cb);

		UV_PushError(pSocketContext, status);
		return;
	}

	pSocketContext->m_PendingCallback = true;

	pSocketContext->m_pStream = req->handle;
	free(req);
	pSocketContext->m_pStream->data = pSocketContext;

	CSocketConnect *pConnect = (CSocketConnect *)malloc(sizeof(CSocketConnect));
	pConnect->pSocketContext = pSocketContext;
	pConnect->pClientSocket = pSocketContext->m_pStream;
	pConnect->pClientIP = NULL;
	// This handle belongs to the context, not to the connect item.
	pConnect->bAccepted = false;
	g_ConnectQueue.enqueue(pConnect);

	uv_read_start(pSocketContext->m_pStream, UV_AllocBuffer, UV_OnRead);
}

void UV_StartRead(uv_async_t *pHandle)
{
	CAsyncSocketContext *pSocketContext = (CAsyncSocketContext *)pHandle->data;
	uv_close((uv_handle_t *)pHandle, pHandle->close_cb);

	if(pSocketContext->m_Deleted || !pSocketContext->m_pStream)
		return;

	uv_read_start(pSocketContext->m_pStream, UV_AllocBuffer, UV_OnRead);
}

void UV_OnNewConnection(uv_stream_t *server, int status)
{
	// server context
	CAsyncSocketContext *pSocketContext = (CAsyncSocketContext *)server->data;
	if(pSocketContext->m_Deleted)
	{
		// The queued UV_DeleteAsyncContext owns the listening socket, closing it here
		// would close it twice.
		return;
	}

	if(status < 0)
	{
		// server is the context's own listening socket and close_cb frees it, so drop
		// our pointers to it before it goes away.
		if((uv_stream_t *)pSocketContext->m_pSocket == server)
			pSocketContext->m_pSocket = NULL;

		if(pSocketContext->m_pStream == server)
			pSocketContext->m_pStream = NULL;

		uv_close((uv_handle_t *)server, server->close_cb);
		UV_PushError(pSocketContext, status);
		return;
	}

	uv_tcp_t *pClientSocket = (uv_tcp_t *)malloc(sizeof(uv_tcp_t));
	uv_tcp_init(g_UV_Loop, pClientSocket);
	pClientSocket->close_cb = UV_FreeHandle;

	if(uv_accept((uv_stream_t *)pSocketContext->m_pSocket, (uv_stream_t *)pClientSocket) == 0)
	{
		pSocketContext->m_PendingCallback = true;
		CSocketConnect *pConnect = (CSocketConnect *)malloc(sizeof(CSocketConnect));
		pConnect->pSocketContext = pSocketContext;
		pConnect->pClientSocket = (uv_stream_t *)pClientSocket;
		// Nothing owns this handle until the game thread builds a context for it.
		pConnect->bAccepted = true;

		pConnect->pClientIP = (char *)malloc(MAX_IP_BUFFER_LENGTH);
		if (pConnect->pClientIP)
		{
			pConnect->pClientIP[0] = '\0';
			struct sockaddr_storage name;
			int namelen = sizeof(name);
			if (uv_tcp_getpeername(pClientSocket, (struct sockaddr *)&name, &namelen) == 0) 
			{
				if (name.ss_family == AF_INET) 
				{
					uv_ip4_name((const struct sockaddr_in *)&name, pConnect->pClientIP, MAX_IP_BUFFER_LENGTH);
				} 
				else if (name.ss_family == AF_INET6) 
				{
					uv_ip6_name((const struct sockaddr_in6 *)&name, pConnect->pClientIP, MAX_IP_BUFFER_LENGTH);
				}
			}
		}

		g_ConnectQueue.enqueue(pConnect);
	}
	else
	{
		uv_close((uv_handle_t *)pClientSocket, pClientSocket->close_cb);
	}
}

void UV_OnAsyncResolved(uv_getaddrinfo_t *resolver, int status, struct addrinfo *res)
{
	CAsyncSocketContext *pSocketContext = (CAsyncSocketContext *)resolver->data;
	pSocketContext->m_Pending = false;

	// The lookup failed, was cancelled by UV_DeleteAsyncContext, or the plugin handle
	// is gone and there is nothing left to connect. Note that uv_cancel() fails when
	// the lookup is already running, so a cancelled teardown can still land here with
	// any status.
	if(status < 0 || pSocketContext->m_Deleted || pSocketContext->m_pSocket)
	{
		if(res)
			uv_freeaddrinfo(res);

		if(status < 0 && status != UV_ECANCELED && !pSocketContext->m_Deleted)
			UV_PushError(pSocketContext, status);

		UV_ReleaseContext(pSocketContext);
		return;
	}

	uv_tcp_t *pSocket = (uv_tcp_t *)malloc(sizeof(uv_tcp_t));
	uv_tcp_init(g_UV_Loop, pSocket);
	pSocket->close_cb = UV_FreeHandle;
	pSocket->data = pSocketContext;

	pSocketContext->m_pSocket = pSocket;

	if(pSocketContext->m_Server)
	{
		int bind_err = uv_tcp_bind(pSocket, (const struct sockaddr *)res->ai_addr, 0);
		if(bind_err)
		{
			pSocketContext->m_pSocket = NULL;
			uv_close((uv_handle_t *)pSocket, pSocket->close_cb);
			UV_PushError(pSocketContext, bind_err);
			uv_freeaddrinfo(res);
			UV_ReleaseContext(pSocketContext);
			return;
		}

		int err = uv_listen((uv_stream_t *)pSocket, 32, UV_OnNewConnection);
		if(err)
		{
			pSocketContext->m_pSocket = NULL;
			uv_close((uv_handle_t *)pSocket, pSocket->close_cb);
			UV_PushError(pSocketContext, err);
		}
	}
	else
	{
		uv_connect_t *pConnectReq = (uv_connect_t *)malloc(sizeof(uv_connect_t));
		pConnectReq->data = pSocketContext;

		uv_tcp_connect(pConnectReq, pSocket, (const struct sockaddr *)res->ai_addr, UV_OnConnect);
	}

	uv_freeaddrinfo(res);

	UV_ReleaseContext(pSocketContext);
}

void UV_OnAsyncResolve(uv_async_t *pHandle)
{
	CAsyncSocketContext *pSocketContext = (CAsyncSocketContext *)pHandle->data;
	uv_close((uv_handle_t *)pHandle, pHandle->close_cb);

	if(pSocketContext->m_Deleted || pSocketContext->m_pSocket)
	{
		// No lookup will be started, so nothing will ever clear this for us. Leaving it
		// set would make the teardown wait forever on a request that never ran.
		pSocketContext->m_Pending = false;
		return;
	}

	pSocketContext->m_Resolver.data = pSocketContext;

	char service[16];
	snprintf(service, sizeof(service), "%d", pSocketContext->m_Port);

	struct addrinfo hints;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = PF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	hints.ai_flags = 0;

	// The lookup holds a reference until UV_OnAsyncResolved runs.
	pSocketContext->m_UvRefs++;

	int err = uv_getaddrinfo(g_UV_Loop, &pSocketContext->m_Resolver, UV_OnAsyncResolved, pSocketContext->m_pHost, service, &hints);
	if(err)
	{
		// No callback will ever run for this request, so release it here. Leaving
		// m_Pending set would make the teardown wait forever on a request that is not
		// running, and would keep the natives rejecting this socket as pending.
		pSocketContext->m_Pending = false;
		UV_PushError(pSocketContext, err);
		UV_ReleaseContext(pSocketContext);
	}
}

void UV_OnAsyncWriteCleanup(uv_write_t *req, int status)
{
	CAsyncWrite *pWrite = (CAsyncWrite *)req->data;

	free(pWrite->pBuffer->base);
	free(pWrite->pBuffer);
	free(pWrite);
	free(req);
}

void UV_OnAsyncWrite(uv_async_t *handle)
{
	CAsyncWrite *pWrite = (CAsyncWrite *)handle->data;
	uv_close((uv_handle_t *)handle, handle->close_cb);

	if(pWrite == NULL || pWrite->pBuffer == NULL)
		return;

	if(pWrite->pSocketContext == NULL || pWrite->pSocketContext->m_pStream == NULL)
	{
		free(pWrite->pBuffer->base);
		free(pWrite->pBuffer);
		free(pWrite);
		return;
	}

	uv_write_t *req = (uv_write_t *)malloc(sizeof(uv_write_t));
	req->data = pWrite;

	uv_write(req, pWrite->pSocketContext->m_pStream, pWrite->pBuffer, 1, UV_OnAsyncWriteCleanup);
}

cell_t Native_AsyncSocket_Create(IPluginContext *pContext, const cell_t *params)
{
	CAsyncSocketContext *pSocketContext = new CAsyncSocketContext(pContext);

	pSocketContext->m_Handle = handlesys->CreateHandle(g_AsyncSocket.socketHandleType, pSocketContext,
		pContext->GetIdentity(), myself->GetIdentity(), NULL);

	return pSocketContext->m_Handle;
}

cell_t Native_AsyncSocket_Connect(IPluginContext *pContext, const cell_t *params)
{
	CAsyncSocketContext *pSocketContext = g_AsyncSocket.GetSocketInstanceByHandle(params[1]);

	if(pSocketContext == NULL)
		return pContext->ThrowNativeError("Invalid socket handle");

	if(params[3] < 0 || params[3] > 65535)
		return pContext->ThrowNativeError("Invalid port specified");

	if(pSocketContext->m_pSocket)
		return pContext->ThrowNativeError("Socket is already connected");

	if(pSocketContext->m_Pending)
		return pContext->ThrowNativeError("Socket is currently pending");

	char *address = NULL;
	pContext->LocalToString(params[2], &address);

	free(pSocketContext->m_pHost); // a retry after a failed lookup, bind or connect
	pSocketContext->m_pHost = strdup(address);
	pSocketContext->m_Port = params[3];
	pSocketContext->m_Server = false;
	pSocketContext->m_Pending = true;

	CAsyncAddJob Job;
	Job.CallbackFn = UV_OnAsyncResolve;
	Job.pData = pSocketContext;
	g_AsyncAddQueue.enqueue(Job);

	uv_async_send(&g_UV_AsyncAdded);

	return 1;
}

cell_t Native_AsyncSocket_Listen(IPluginContext *pContext, const cell_t *params)
{
	CAsyncSocketContext *pSocketContext = g_AsyncSocket.GetSocketInstanceByHandle(params[1]);

	if(pSocketContext == NULL)
		return pContext->ThrowNativeError("Invalid socket handle");

	if(params[3] < 0 || params[3] > 65535)
		return pContext->ThrowNativeError("Invalid port specified");

	if(pSocketContext->m_pSocket)
		return pContext->ThrowNativeError("Socket is already connected");

	if(pSocketContext->m_Pending)
		return pContext->ThrowNativeError("Socket is currently pending");

	char *address = NULL;
	pContext->LocalToString(params[2], &address);

	free(pSocketContext->m_pHost); // a retry after a failed lookup, bind or connect
	pSocketContext->m_pHost = strdup(address);
	pSocketContext->m_Port = params[3];
	pSocketContext->m_Server = true;
	pSocketContext->m_Pending = true;

	CAsyncAddJob Job;
	Job.CallbackFn = UV_OnAsyncResolve;
	Job.pData = pSocketContext;
	g_AsyncAddQueue.enqueue(Job);

	uv_async_send(&g_UV_AsyncAdded);

	return 1;
}

cell_t Native_AsyncSocket_Write(IPluginContext *pContext, const cell_t *params)
{
	CAsyncSocketContext *pSocketContext = g_AsyncSocket.GetSocketInstanceByHandle(params[1]);

	if(pSocketContext == NULL)
		return pContext->ThrowNativeError("Invalid socket handle");

	if(!pSocketContext->m_pStream)
		return pContext->ThrowNativeError("Socket is not connected");

	char *data = NULL;
	pContext->LocalToPhysAddr(params[2], (cell_t **)&data);

	uv_buf_t *buffer = (uv_buf_t *)malloc(sizeof(uv_buf_t));

	if(params[3] >= 0)
		buffer->len = params[3];
	else
		buffer->len = strlen(data);

	// Copy exactly len bytes: the plugin's buffer may end right after them.
	buffer->base = (char *)malloc(buffer->len + 1);
	memcpy(buffer->base, data, buffer->len);
	buffer->base[buffer->len] = 0;

	CAsyncWrite *pWrite = (CAsyncWrite *)malloc(sizeof(CAsyncWrite));

	pWrite->pSocketContext = pSocketContext;
	pWrite->pBuffer = buffer;

	CAsyncAddJob Job;
	Job.CallbackFn = UV_OnAsyncWrite;
	Job.pData = pWrite;
	g_AsyncAddQueue.enqueue(Job);

	uv_async_send(&g_UV_AsyncAdded);

	return 1;
}

cell_t Native_AsyncSocket_SetConnectCallback(IPluginContext *pContext, const cell_t *params)
{
	CAsyncSocketContext *pSocketContext = g_AsyncSocket.GetSocketInstanceByHandle(params[1]);

	if(pSocketContext == NULL)
		return pContext->ThrowNativeError("Invalid socket handle");

	if(!pSocketContext->SetConnectCallback(params[2]))
		return pContext->ThrowNativeError("Invalid callback");

	return true;
}

cell_t Native_AsyncSocket_SetErrorCallback(IPluginContext *pContext, const cell_t *params)
{
	CAsyncSocketContext *pSocketContext = g_AsyncSocket.GetSocketInstanceByHandle(params[1]);

	if(pSocketContext == NULL)
		return pContext->ThrowNativeError("Invalid socket handle");

	if(!pSocketContext->SetErrorCallback(params[2]))
		return pContext->ThrowNativeError("Invalid callback");

	return true;
}

cell_t Native_AsyncSocket_SetDataCallback(IPluginContext *pContext, const cell_t *params)
{
	CAsyncSocketContext *pSocketContext = g_AsyncSocket.GetSocketInstanceByHandle(params[1]);

	if(pSocketContext == NULL)
		return pContext->ThrowNativeError("Invalid socket handle");

	if(!pSocketContext->SetDataCallback(params[2]))
		return pContext->ThrowNativeError("Invalid callback");

	return true;
}

cell_t Native_AsyncSocket_GetClientIP(IPluginContext *pContext, const cell_t *params)
{
	CAsyncSocketContext *pSocketContext = g_AsyncSocket.GetSocketInstanceByHandle(params[1]);

	if (pSocketContext == NULL)
	{
		return pContext->ThrowNativeError("Invalid socket handle");
	}

	const char *ip = (pSocketContext->m_pClientIP != NULL) ? pSocketContext->m_pClientIP : "";
	pContext->StringToLocal(params[2], params[3], ip);

	return 1;
}

// Sourcemod Plugin Events
bool AsyncSocket::SDK_OnLoad(char *error, size_t maxlength, bool late)
{
	g_Running = true;
	sharesys->AddNatives(myself, AsyncSocketNatives);
	sharesys->RegisterLibrary(myself, "AsyncSocket");

	socketHandleType = handlesys->CreateType("AsyncSocket", this, 0, NULL, NULL, myself->GetIdentity(), NULL);

	smutils->AddGameFrameHook(OnGameFrame);

	g_UV_Loop = uv_default_loop();

	uv_async_init(g_UV_Loop, &g_UV_AsyncAdded, UV_OnAsyncAdded);
	g_UV_AsyncAdded.close_cb = NULL;

	uv_thread_create(&g_UV_LoopThread, UV_EventLoop, NULL);

	return true;
}

void UV_OnWalk(uv_handle_t *pHandle, void *pArg)
{
	if(uv_is_closing(pHandle))
		return;

	uv_close(pHandle, pHandle->close_cb);
}

// Drops everything still queued once the loop is gone. Nothing may run plugin
// callbacks at this point, the payloads just need to be released.
void UV_DrainQueues()
{
	CSocketConnect *pConnect;
	while(g_ConnectQueue.try_dequeue(pConnect))
	{
		if(pConnect->pClientIP)
			free(pConnect->pClientIP);

		free(pConnect);
	}

	CSocketData *pData;
	while(g_DataQueue.try_dequeue(pData))
	{
		free(pData->pBuffer);
		free(pData);
	}

	CSocketError *pError;
	while(g_ErrorQueue.try_dequeue(pError))
	{
		free(pError);
	}

	CAsyncSocketContext *pSocketContext;
	while(g_DeleteQueue.try_dequeue(pSocketContext))
	{
		g_ContextsToDelete.push_back(pSocketContext);
	}

	for(size_t i = 0; i < g_ContextsToDelete.size(); i++)
	{
		delete g_ContextsToDelete[i];
	}
	g_ContextsToDelete.clear();
}

void AsyncSocket::SDK_OnUnload()
{
	g_Running = false;

	// Stop the event loop before destroying anything: OnHandleDestroy deletes contexts
	// inline once g_Running is false, which is only safe when the UV thread can no
	// longer touch them.
	CAsyncAddJob Job;
	Job.CallbackFn = UV_Quit;
	Job.pData = NULL;
	g_AsyncAddQueue.enqueue(Job);

	uv_async_send(&g_UV_AsyncAdded);

	uv_thread_join(&g_UV_LoopThread);

	uv_walk(g_UV_Loop, UV_OnWalk, NULL);

	uv_run(g_UV_Loop, UV_RUN_DEFAULT);

	uv_loop_close(g_UV_Loop);

	handlesys->RemoveType(socketHandleType, myself->GetIdentity());

	smutils->RemoveGameFrameHook(OnGameFrame);

	UV_DrainQueues();
}

const sp_nativeinfo_t AsyncSocketNatives[] = {
	{"AsyncSocket.AsyncSocket", Native_AsyncSocket_Create},
	{"AsyncSocket.Connect", Native_AsyncSocket_Connect},
	{"AsyncSocket.Listen", Native_AsyncSocket_Listen},
	{"AsyncSocket.Write", Native_AsyncSocket_Write},
	{"AsyncSocket.SetConnectCallback", Native_AsyncSocket_SetConnectCallback},
	{"AsyncSocket.SetErrorCallback", Native_AsyncSocket_SetErrorCallback},
	{"AsyncSocket.SetDataCallback", Native_AsyncSocket_SetDataCallback},
	{"AsyncSocket.GetClientIP", Native_AsyncSocket_GetClientIP},
	{NULL, NULL}
};
