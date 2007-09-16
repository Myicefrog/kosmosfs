//---------------------------------------------------------- -*- Mode: C++ -*-
// $Id$ 
//
// Created 2006/03/14
// Author: Sriram Rao (Kosmix Corp.) 
//
// Copyright 2006 Kosmix Corp.
//
// This file is part of Kosmos File System (KFS).
//
// Licensed under the Apache License, Version 2.0
// (the "License"); you may not use this file except in compliance with
// the License. You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
// implied. See the License for the specific language governing
// permissions and limitations under the License.
//
// 
//----------------------------------------------------------------------------

#ifndef _LIBIO_NETCONNECTION_H
#define _LIBIO_NETCONNECTION_H

#include "KfsCallbackObj.h"
#include "Event.h"
#include "IOBuffer.h"
#include "TcpSocket.h"

#include "common/log.h"

#include <boost/shared_ptr.hpp>

///
/// \file NetConnection.h
/// \brief A network connection uses TCP sockets for doing I/O.
/// 
/// A network connection contains a socket and data in buffers.
/// Whenever data is read from the socket it is held in the "in"
/// buffer; whenever data needs to be written out on the socket, that
/// data should be dropped into the "out" buffer and it will
/// eventually get sent out.
/// 

///
/// \class NetConnection
/// A net connection contains an underlying socket and is associated
/// with a KfsCallbackObj.  Whenever I/O is done on the socket (either
/// for read or write) or when an error occurs (such as the remote
/// peer closing the connection), the associated KfsCallbackObj is
/// called back with an event notification.
/// 
class NetConnection {
public:
    /// @param[in] sock TcpSocket on which I/O can be done
    /// @param[in] c KfsCallbackObj associated with this connection
    NetConnection(TcpSocket *sock, KfsCallbackObj *c) {
	mSock = sock;
	mCallbackObj = c;
	mListenOnly = false;
	mInBuffer = mOutBuffer = NULL;
        mNumBytesOut = 0;
    }

    /// @param[in] sock TcpSocket on which I/O can be done
    /// @param[in] c KfsCallbackObj associated with this connection
    /// @param[in] listenOnly boolean that specifies whether this
    /// connection is setup only for accepting new connections.
    NetConnection(TcpSocket *sock, KfsCallbackObj *c, bool listenOnly) {
        mSock = sock;
        mCallbackObj = c;
        mListenOnly = listenOnly;
        mInBuffer = NULL;
        mOutBuffer = NULL;
        mNumBytesOut = 0;
    }

    ~NetConnection() {
        delete mSock;
        delete mOutBuffer;
        delete mInBuffer;
    }

    void SetOwningKfsCallbackObj(KfsCallbackObj *c) {
        mCallbackObj = c;
    }

    int GetFd() { return mSock->GetFd(); }

    /// Callback for handling a read.  That is, select() thinks that
    /// data is available for reading. So, do something.
    void HandleReadEvent();

    /// Callback for handling a writing.  That is, select() thinks that
    /// data can be sent out.  So, do something.
    void HandleWriteEvent();

    /// Callback for handling errors.  That is, select() thinks that
    /// an error occurred.  So, do something.
    void HandleErrorEvent();

    /// Do we expect data to be read in?
    bool IsReadReady();
    /// Is data available for writing?
    bool IsWriteReady();

    /// Is the connection still good?
    bool IsGood();

    /// Enqueue data to be sent out.
    void Write(IOBufferDataPtr &ioBufData) {
        mOutBuffer->Append(ioBufData);
        mNumBytesOut += ioBufData->BytesConsumable();
    }

    void Write(IOBuffer *ioBuf, int numBytes) {
        mOutBuffer->Append(ioBuf);
        mNumBytesOut += numBytes;
    }
    
    /// Enqueue data to be sent out.
    void Write(const char *data, int numBytes) {
	if (mOutBuffer == NULL) {
		mOutBuffer = new IOBuffer();
	}
        mOutBuffer->CopyIn(data, numBytes);
        mNumBytesOut += numBytes;
    }

    /// Close the connection.
    void Close() {
        COSMIX_LOG_DEBUG("Closing socket: %d", mSock->GetFd());
        mSock->Close();
    }
    

private:
    bool		mListenOnly;
    /// KfsCallbackObj that will be notified whenever "events" occur.
    KfsCallbackObj	*mCallbackObj;
    /// Socket on which I/O will be done.
    TcpSocket		*mSock;
    /// Buffer that contains data read from the socket
    IOBuffer		*mInBuffer;
    /// Buffer that contains data that should be sent out on the socket.
    IOBuffer		*mOutBuffer;
    /// # of bytes from the out buffer that should be sent out.
    int			mNumBytesOut;
};

typedef boost::shared_ptr<NetConnection> NetConnectionPtr;

#endif // LIBIO_NETCONNECTION_H
