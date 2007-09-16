//---------------------------------------------------------- -*- Mode: C++ -*-
// $Id$ 
//
// Created 2006/06/20
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

#include<map>
#include<sstream>
#include "libkfsIO/Globals.h"
using namespace libkfsio;

#include "Logger.h"
#include "ChunkManager.h"

using std::ios_base;
using std::map;
using std::ifstream;
using std::istringstream;

// checksums for a 64MB chunk can make a long line...
const int MAX_LINE_LENGTH = 32768;
char ckptLogVersionStr[128];

typedef void (*ParseHandler_t)(istringstream &ist);

/*!
 * \brief check whether a file exists
 * \param[in]	name	path name of the file
 * \return		true if stat says it is a plain file
 */
static bool 
file_exists(string name)
{
    struct stat s;
    if (stat(name.c_str(), &s) == -1)
        return false;
    
    return S_ISREG(s.st_mode);
}

Logger::Logger()
{
    mLogDir = NULL;
    mLogFilename = "";
    mLoggerTimeoutImpl = new LoggerTimeoutImpl(this);
    mLogGenNum = 1;
    sprintf(ckptLogVersionStr, "version: %d", KFS_VERSION);
}

Logger::~Logger()
{
    mFile.close();
    delete mLoggerTimeoutImpl;
}

void
Logger::Init(const char *logDir)
{
    mLogDir = logDir;
    mLogFilename = mLogDir;
    mLogFilename += "/logs";

}

static void *
logger_main(void *dummy)
{
    (void) dummy; // shut-up g++
    gLogger.MainLoop();
    return NULL;
}

void
Logger::MainLoop()
{
    KfsOp *op;
    list<KfsOp *> done;
    list<KfsOp *>::iterator iter;

    while (1) {
        op = mPending.dequeue();
        while (op != NULL) {
            // pull as many as we can and log them
            if (op->op == CMD_CHECKPOINT) {
                // Checkpoint ops are special.  There is log handling
                // that needs to be done.  After writing out the
                // checkpoint, get rid of the op.
                mFile.flush();
                Checkpoint(op);
                delete op;
            } else {
                op->Log(mFile);
                done.push_back(op);
            }
            op = mPending.dequeue_nowait();
        }
        // one flush for everything we have in the queue
        mFile.flush();
        // now, allow everything that was flushed
        while ((iter = done.begin()) != done.end()) {
            op = *iter;
            done.erase(iter);
            mLogged.enqueue(op);
        }
    }
}

void
Logger::Submit(KfsOp *op)
{
    mPending.enqueue(op);
}

void
Logger::Dispatch()
{
    KfsOp *op;

    while (!mLogged.empty()) {
        op = mLogged.dequeue_nowait();
        if (op == NULL)
            break;
        assert(op->clnt != NULL);
        op->clnt->HandleEvent(EVENT_CMD_DONE, op);
    }
}


void
Logger::Start()
{
    string filename;
    bool writeHeader = false;

    if (mFile.is_open()) {
        mFile.close();
    }
    filename = MakeLogFilename();
    if (!file_exists(filename.c_str()))
        writeHeader = true;

    mFile.open(filename.c_str(), ios_base::app);
    if (writeHeader) {
        COSMIX_LOG_DEBUG("Writing out a log header");
        mFile << ckptLogVersionStr << '\n';
        mFile.flush();
    }

    if (!mFile.is_open()) {
        COSMIX_LOG_DEBUG("Unable to open: %s",
                         filename.c_str());
    }
    assert(!mFile.fail());
    globals().netManager.RegisterTimeoutHandler(mLoggerTimeoutImpl);
    mWorker.start(logger_main, NULL);
}

void
Logger::Checkpoint(KfsOp *op)
{
    CheckpointOp *cop = static_cast<CheckpointOp *> (op);
    ofstream ofs;
    string ckptFilename;
    string lastCP;

    ckptFilename = MakeCkptFilename();
    lastCP = MakeLatestCkptFilename();

    ofs.open(ckptFilename.c_str(), ios_base::out);
    if (!ofs) {
        perror("Ckpt create failed: ");
        return;
    }

    // write out a header that has version and name of log file
    ofs << ckptLogVersionStr << '\n';
    // This is the log file associated with this checkpoint.  That is,
    // this log file contains all the activities since this checkpoint.
    ofs << "log: " << mLogFilename << '.' << mLogGenNum + 1 << '\n';

    ofs << cop->data.str();
    ofs.flush();
    assert(!ofs.fail());
    ofs.close();

    // now, link the latest
    unlink(lastCP.c_str());

    if (link(ckptFilename.c_str(), lastCP.c_str()) < 0) {
        perror("link of ckpt file failed: ");
    }

    RotateLog();
}

string
Logger::MakeLogFilename()
{
    ostringstream os;

    os << mLogFilename << '.' << mLogGenNum;
    return os.str();
}

string
Logger::MakeCkptFilename()
{
    ostringstream os;

    os << mLogDir << '/' << "ckpt" << '.' << mLogGenNum;
    return os.str();
}

string 
Logger::MakeLatestCkptFilename()
{
    string s(mLogDir);

    s += "/ckpt_latest";
    return s;
}

void 
Logger::RotateLog()
{
    string filename;

    if (mFile.is_open()) {
        mFile.close();
    }

    filename = MakeLogFilename();
    // For log rotation, get rid of the old log and start a new one.
    // For now, preserve all the log files.

    // unlink(filename.c_str());

    mLogGenNum++;
    filename = MakeLogFilename();
    mFile.open(filename.c_str());
    if (!mFile.is_open()) {
        COSMIX_LOG_DEBUG("Unable to open: %s",
                         filename.c_str());
        return;
    }
    mFile << ckptLogVersionStr << '\n';
    mFile.flush();
}

void
Logger::Restore()
{
    string lastCP;
    ifstream ifs;
    char line[MAX_LINE_LENGTH], *genNum;
    ChunkInfoHandle_t *cih;
    ChunkInfo_t entry;

    lastCP = MakeLatestCkptFilename();

    if (!file_exists(lastCP.c_str()))
        goto out;

    ifs.open(lastCP.c_str(), ios_base::in);
    if (!ifs) {
        perror("Ckpt open failed: ");
        goto out;
    }
    
    // Read the header
    // Line 1 is the version
    memset(line, '\0', MAX_LINE_LENGTH);
    ifs.getline(line, MAX_LINE_LENGTH);
    if (ifs.eof())
        goto out;
    if (strncmp(line, ckptLogVersionStr, strlen(ckptLogVersionStr)) != 0) {
        COSMIX_LOG_DEBUG("Restore ckpt: Ckpt version str mismatch: read: %s",
                         line);
        goto out;
    }

    // Line 2 is the log file name
    memset(line, '\0', MAX_LINE_LENGTH);
    ifs.getline(line, MAX_LINE_LENGTH);
    if (ifs.eof())
        goto out;
    if (strncmp(line, "log:", 4) != 0) {
        COSMIX_LOG_DEBUG("Restore ckpt: Log line mismatch: read: %s",
                         line);
        goto out;
    }
    genNum = rindex(line, '.');
    if (genNum != NULL) {
        genNum++;
        mLogGenNum = atoll(genNum);
        COSMIX_LOG_DEBUG("Read log gen #: %lld",
                         mLogGenNum);
    }
    
    // Read the checkpoint file
    while (!ifs.eof()) {
        ifs.getline(line, MAX_LINE_LENGTH);
        if (!ParseCkptEntry(line, entry))
            break;

        cih = new ChunkInfoHandle_t();
        cih->chunkInfo = entry;

        COSMIX_LOG_DEBUG("Read chunk: %ld, %d, %lu", 
                         cih->chunkInfo.chunkId,
                         cih->chunkInfo.chunkVersion,
                         cih->chunkInfo.chunkSize);
        gChunkManager.AddMapping(cih);
    }
  out:
    ifs.close();

    // replay the logs
    ReplayLog();

}

bool
Logger::ParseCkptEntry(const char *line, ChunkInfo_t &entry)
{
    const string l = line;
    istringstream ist(line);
    vector<uint32_t>::size_type count;

    if (l.empty())
        return false;

    ist.str(line);
    ist >> entry.fileId;
    ist >> entry.chunkId;
    ist >> entry.chunkSize;
    ist >> entry.chunkVersion;
    ist >> count;
    entry.chunkBlockChecksum.resize(count);
    for (vector<uint32_t>::size_type i = 0; i < count; ++i) {
        ist >> entry.chunkBlockChecksum[i];
    }
    
    return true;
}

// Handlers for each of the entry types in the log file

static void
ParseAllocateChunk(istringstream &ist)
{
    kfsChunkId_t chunkId;
    kfsFileId_t fileId;
    int64_t chunkVersion;

    ist >> chunkId;
    ist >> fileId;
    ist >> chunkVersion;
    gChunkManager.ReplayAllocChunk(fileId, chunkId,
                                   chunkVersion);
    
}

static void
ParseDeleteChunk(istringstream &ist)
{
    kfsChunkId_t chunkId;

    ist >> chunkId;
    gChunkManager.ReplayDeleteChunk(chunkId);
}

static void
ParseWrite(istringstream &ist)
{
    kfsChunkId_t chunkId;
    size_t chunkSize;
    vector<uint32_t> checksums;
    uint32_t offset;
    vector<uint32_t>::size_type n;

    ist >> chunkId;
    ist >> chunkSize;
    ist >> offset;
    ist >> n;
    for (vector<uint32_t>::size_type i = 0; i < n; ++i) {
        uint32_t v;
        ist >> v;
        checksums.push_back(v);
    }
    gChunkManager.ReplayWriteDone(chunkId, chunkSize, 
                                  offset, checksums);
}

static void
ParseTruncateChunk(istringstream &ist)
{
    kfsChunkId_t chunkId;
    size_t chunkSize;

    ist >> chunkId;
    ist >> chunkSize;
    gChunkManager.ReplayTruncateDone(chunkId, chunkSize);
}

static void
ParseChangeChunkVers(istringstream &ist)
{
    kfsChunkId_t chunkId;
    kfsFileId_t fileId;
    int64_t chunkVersion;

    ist >> chunkId;
    ist >> fileId;
    ist >> chunkVersion;
    gChunkManager.ReplayChangeChunkVers(fileId, chunkId,
                                        chunkVersion);
}

//
// Each log entry is of the form <OP-NAME> <op args>\n
// To replay the log, read a line, from the <OP-NAME> identify the
// handler and call it to parse/replay the log entry.
//
void
Logger::ReplayLog()
{
    istringstream ist;
    char line[MAX_LINE_LENGTH];
    string l;
    map<string, ParseHandler_t> opHandlers;
    map<string, ParseHandler_t>::iterator iter;
    ifstream ifs;
    string filename;
    string opName;

    filename = MakeLogFilename();

    if (!file_exists(filename.c_str())) {
        COSMIX_LOG_DEBUG("File: %s doesn't exist; no log replay",
                         filename.c_str());
        return;
    }

    ifs.open(filename.c_str(), ios_base::in);
    if (!ifs) {
        COSMIX_LOG_DEBUG("Unable to open: %s",
                         filename.c_str()); 
        return;
    }

    // Read the header
    // Line 1 is the version
    memset(line, '\0', MAX_LINE_LENGTH);
    ifs.getline(line, MAX_LINE_LENGTH);
    if (ifs.eof()) {
        ifs.close();
        return;
    }

    if (strncmp(line, ckptLogVersionStr, strlen(ckptLogVersionStr)) != 0) {
        COSMIX_LOG_DEBUG("Replay log failed: Log version str mismatch: read: %s",
                         line);
        ifs.close();
        return;
    }

    opHandlers["ALLOCATE"] = ParseAllocateChunk;
    opHandlers["DELETE"] = ParseDeleteChunk;
    opHandlers["WRITE"] = ParseWrite;
    opHandlers["TRUNCATE"] = ParseTruncateChunk;
    opHandlers["CHANGE_CHUNK_VERS"] = ParseChangeChunkVers;
	
    while (!ifs.eof()) {
        ifs.getline(line, MAX_LINE_LENGTH);
        l = line;
        if (l.empty())
            break;
        ist.str(l);
        ist >> opName;

        iter = opHandlers.find(opName);

        if (iter == opHandlers.end()) {
            COSMIX_LOG_DEBUG("Unable to replay %s", line);
            ist.clear();
            continue;
        }
        iter->second(ist);
        ist.clear();
    }
    ifs.close();
}
