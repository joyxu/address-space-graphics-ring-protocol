/*
* Copyright (C) 2011 The Android Open Source Project
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/
#include "asg_ring_stream_client.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

static const size_t kReadSize = 512 * 1024;
static const size_t kWriteOffset = kReadSize;
static const size_t kFlushInterval = 10000;

namespace asg {
namespace client {

RingStreamClient::RingStreamClient(void* sharedRegion, size_t ringXferBufferSize, ring_stream_client_doorbell_t) :
    IOStream(kFlushInterval),
    m_tmpBuf(0),
    m_tmpBufSize(0),
    m_tmpBufXferSize(0),
    m_usingTmpBuf(0),
    m_readBuf(0),
    m_read(0),
    m_readLeft(0),
    m_writeBufferSize(ringXferBufferSize),
    m_writeBufferMask(m_writeBufferSize - 1),
    m_buf(((unsigned char*)sharedRegion) + sizeof(struct asg_ring_storage)),
    m_writeStart(m_buf),
    m_writeStep(4096),
    m_notifs(0),
    m_written(0),
    m_backoffIters(0),
    m_backoffFactor(1) {

    m_context = asg_context_create((char*)sharedRegion, (char*)sharedRegion + sizeof(struct asg_ring_storage), ringXferBufferSize);
}

RingStreamClient::~RingStreamClient() {
    flush();
    ensureType3Finished();
    ensureType1Finished();
    if (m_readBuf) free(m_readBuf);
    if (m_tmpBuf) free(m_tmpBuf);
}

size_t RingStreamClient::idealAllocSize(size_t len) {
    if (len > m_writeStep) return len;
    return m_writeStep;
}

void *RingStreamClient::allocBuffer(size_t minSize) {
    ensureType3Finished();

    if (!m_readBuf) {
        m_readBuf = (unsigned char*)malloc(kReadSize);
    }

    size_t allocSize =
        (m_writeStep < minSize ? minSize : m_writeStep);

    if (m_writeStep < allocSize) {
        if (!m_tmpBuf) {
            m_tmpBufSize = allocSize * 2;
            m_tmpBuf = (unsigned char*)malloc(m_tmpBufSize);
        }

        if (m_tmpBufSize < allocSize) {
            m_tmpBufSize = allocSize * 2;
            m_tmpBuf = (unsigned char*)realloc(m_tmpBuf, m_tmpBufSize);
        }

        if (!m_usingTmpBuf) {
            flush();
        }

        m_usingTmpBuf = true;
        m_tmpBufXferSize = allocSize;
        return m_tmpBuf;
    } else {
        if (m_usingTmpBuf) {
            writeFully(m_tmpBuf, m_tmpBufXferSize);
            m_usingTmpBuf = false;
            m_tmpBufXferSize = 0;
        }

        return m_writeStart;
    }
};

int RingStreamClient::commitBuffer(size_t size)
{
    if (size == 0) return 0;

    if (m_usingTmpBuf) {
        writeFully(m_tmpBuf, size);
        m_tmpBufXferSize = 0;
        m_usingTmpBuf = false;
        return 0;
    } else {
        int res = type1Write(m_writeStart - m_buf, size);
        advanceWrite();
        return res;
    }
}

const unsigned char *RingStreamClient::readFully(void *ptr, size_t totalReadSize)
{

    unsigned char* userReadBuf = static_cast<unsigned char*>(ptr);

    if (!userReadBuf) {
        if (totalReadSize > 0) {
            // ALOGE("RingStreamClient::commitBufferAndReadFully failed, userReadBuf=NULL, totalReadSize %zu, lethal"
            //         " error, exiting.", totalReadSize);
            abort();
        }
        return nullptr;
    }

    // Advance buffered read if not yet consumed.
    size_t remaining = totalReadSize;
    size_t bufferedReadSize =
        m_readLeft < remaining ? m_readLeft : remaining;

    if (bufferedReadSize) {
        memcpy(userReadBuf,
               m_readBuf + (m_read - m_readLeft),
               bufferedReadSize);
        remaining -= bufferedReadSize;
        m_readLeft -= bufferedReadSize;
    }

    if (!remaining) return userReadBuf;

    // Read up to kReadSize bytes if all buffered read has been consumed.
    size_t maxRead = m_readLeft ? 0 : kReadSize;
    ssize_t actual = 0;

    if (maxRead) {
        actual = speculativeRead(m_readBuf, maxRead);

        // Updated buffered read size.
        if (actual > 0) {
            m_read = m_readLeft = actual;
        }

        if (actual == 0) {
            // ALOGD("%s: end of pipe", __FUNCTION__);
            return NULL;
        }
    }

    // Consume buffered read and read more if necessary.
    while (remaining) {
        bufferedReadSize = m_readLeft < remaining ? m_readLeft : remaining;
        if (bufferedReadSize) {
            memcpy(userReadBuf + (totalReadSize - remaining),
                   m_readBuf + (m_read - m_readLeft),
                   bufferedReadSize);
            remaining -= bufferedReadSize;
            m_readLeft -= bufferedReadSize;
            continue;
        }

        actual = speculativeRead(m_readBuf, kReadSize);

        if (actual == 0) {
            // ALOGD("%s: Failed reading from pipe: %d", __FUNCTION__,  errno);
            return NULL;
        }

        if (actual > 0) {
            m_read = m_readLeft = actual;
            continue;
        }
    }

    resetBackoff();
    return userReadBuf;
}

const unsigned char *RingStreamClient::read(void *buf, size_t *inout_len) {
    unsigned char* dst = (unsigned char*)buf;
    size_t wanted = *inout_len;
    ssize_t actual = speculativeRead(dst, wanted);

    if (actual >= 0) {
        *inout_len = actual;
    } else {
        return nullptr;
    }

    return (const unsigned char*)dst;
}

int RingStreamClient::writeFully(const void *buf, size_t size)
{
    ensureType3Finished();
    ensureType1Finished();

    m_context.ring_config->transfer_size = size;
    m_context.ring_config->transfer_mode = 3;

    size_t sent = 0;
    size_t preferredChunkSize = m_writeBufferSize / 4;
    size_t chunkSize = size < preferredChunkSize ? size : preferredChunkSize;
    const uint8_t* bufferBytes = (const uint8_t*)buf;

    bool hostPinged = false;
    while (sent < size) {
        size_t remaining = size - sent;
        size_t sendThisTime = remaining < chunkSize ? remaining : chunkSize;

        long sentChunks =
            ring_buffer_view_write(
                m_context.to_host_large_xfer.ring,
                &m_context.to_host_large_xfer.view,
                bufferBytes + sent, sendThisTime, 1);

        if (!hostPinged && *(m_context.host_state) != ASG_HOST_STATE_CAN_CONSUME &&
            *(m_context.host_state) != ASG_HOST_STATE_RENDERING) {
            notifyAvailable();
            hostPinged = true;
        }

        if (sentChunks == 0) {
            ring_buffer_yield();
            backoff();
        }

        sent += sentChunks * sendThisTime;

        if (isInError()) {
            return -1;
        }
    }

    bool isRenderingAfter = ASG_HOST_STATE_RENDERING == __atomic_load_n(m_context.host_state, __ATOMIC_ACQUIRE);

    if (!isRenderingAfter) {
        notifyAvailable();
    }

    ensureType3Finished();

    resetBackoff();
    m_context.ring_config->transfer_mode = 1;
    m_written += size;

    float mb = (float)m_written / 1048576.0f;
    if (mb > 100.0f) {
        // ALOGD("%s: %f mb in %d notifs. %f mb/notif\n", __func__,
        //       mb, m_notifs, m_notifs ? mb / (float)m_notifs : 0.0f);
        m_notifs = 0;
        m_written = 0;
    }
    return 0;
}

int RingStreamClient::writeFullyAsync(const void *buf, size_t size)
{
    ensureType3Finished();
    ensureType1Finished();

    __atomic_store_n(&m_context.ring_config->transfer_size, size, __ATOMIC_RELEASE);
    m_context.ring_config->transfer_mode = 3;

    size_t sent = 0;
    size_t preferredChunkSize = m_writeBufferSize / 2;
    size_t chunkSize = size < preferredChunkSize ? size : preferredChunkSize;
    const uint8_t* bufferBytes = (const uint8_t*)buf;

    bool pingedHost = false;

    while (sent < size) {
        size_t remaining = size - sent;
        size_t sendThisTime = remaining < chunkSize ? remaining : chunkSize;

        long sentChunks =
            ring_buffer_view_write(
                m_context.to_host_large_xfer.ring,
                &m_context.to_host_large_xfer.view,
                bufferBytes + sent, sendThisTime, 1);

        uint32_t hostState = __atomic_load_n(m_context.host_state, __ATOMIC_ACQUIRE);

        if (!pingedHost &&
            hostState != ASG_HOST_STATE_CAN_CONSUME &&
            hostState != ASG_HOST_STATE_RENDERING) {
            pingedHost = true;
            notifyAvailable();
        }

        if (sentChunks == 0) {
            ring_buffer_yield();
            backoff();
        }

        sent += sentChunks * sendThisTime;

        if (isInError()) {
            return -1;
        }
    }


    bool isRenderingAfter = ASG_HOST_STATE_RENDERING == __atomic_load_n(m_context.host_state, __ATOMIC_ACQUIRE);

    if (!isRenderingAfter) {
        notifyAvailable();
    }

    resetBackoff();
    m_context.ring_config->transfer_mode = 1;
    m_written += size;

    float mb = (float)m_written / 1048576.0f;
    if (mb > 100.0f) {
        // ALOGD("%s: %f mb in %d notifs. %f mb/notif\n", __func__,
        //       mb, m_notifs, m_notifs ? mb / (float)m_notifs : 0.0f);
        m_notifs = 0;
        m_written = 0;
    }
    return 0;
}

const unsigned char *RingStreamClient::commitBufferAndReadFully(
    size_t writeSize, void *userReadBufPtr, size_t totalReadSize) {

    if (m_usingTmpBuf) {
        writeFully(m_tmpBuf, writeSize);
        m_usingTmpBuf = false;
        m_tmpBufXferSize = 0;
        return readFully(userReadBufPtr, totalReadSize);
    } else {
        commitBuffer(writeSize);
        return readFully(userReadBufPtr, totalReadSize);
    }
}

bool RingStreamClient::isInError() const {
    return 1 == m_context.ring_config->in_error;
}

ssize_t RingStreamClient::speculativeRead(unsigned char* readBuffer, size_t trySize) {
    ensureType3Finished();
    ensureType1Finished();

    size_t actuallyRead = 0;
    size_t readIters = 0;

    while (!actuallyRead) {
        ++readIters;

        uint32_t readAvail =
            ring_buffer_available_read(
                m_context.from_host_large_xfer.ring,
                &m_context.from_host_large_xfer.view);

        if (!readAvail) {
            ring_buffer_yield();
            backoff();
            continue;
        }

        uint32_t toRead = readAvail > trySize ?  trySize : readAvail;

        long stepsRead = ring_buffer_view_read(
            m_context.from_host_large_xfer.ring,
            &m_context.from_host_large_xfer.view,
            readBuffer, toRead, 1);

        actuallyRead += stepsRead * toRead;

        if (isInError()) {
            return -1;
        }
    }

    return actuallyRead;
}

void RingStreamClient::notifyAvailable() {
    m_doorbellFunc();
    ++m_notifs;
}

uint32_t RingStreamClient::getRelativeBufferPos(uint32_t pos) {
    return pos & m_writeBufferMask;
}

void RingStreamClient::advanceWrite() {
    m_writeStart += m_context.ring_config->flush_interval;

    if (m_writeStart == m_buf + m_context.ring_config->buffer_size) {
        m_writeStart = m_buf;
    }
}

void RingStreamClient::ensureConsumerFinishing() {
    uint32_t currAvailRead = ring_buffer_available_read(m_context.to_host, 0);

    while (currAvailRead) {
        ring_buffer_yield();
        uint32_t nextAvailRead = ring_buffer_available_read(m_context.to_host, 0);

        if (nextAvailRead != currAvailRead) {
            break;
        }

        if (*(m_context.host_state) != ASG_HOST_STATE_CAN_CONSUME &&
            *(m_context.host_state) != ASG_HOST_STATE_RENDERING) {
            notifyAvailable();
            break;
        }

        backoff();
    }
}

void RingStreamClient::ensureType1Finished() {
    uint32_t currAvailRead =
        ring_buffer_available_read(m_context.to_host, 0);

    while (currAvailRead) {
        backoff();
        ring_buffer_yield();
        currAvailRead = ring_buffer_available_read(m_context.to_host, 0);
        if (isInError()) {
            return;
        }
    }
}

void RingStreamClient::ensureType3Finished() {
    uint32_t availReadLarge =
        ring_buffer_available_read(
            m_context.to_host_large_xfer.ring,
            &m_context.to_host_large_xfer.view);
    while (availReadLarge) {
        ring_buffer_yield();
        backoff();
        availReadLarge =
            ring_buffer_available_read(
                m_context.to_host_large_xfer.ring,
                &m_context.to_host_large_xfer.view);
        if (*(m_context.host_state) != ASG_HOST_STATE_CAN_CONSUME &&
            *(m_context.host_state) != ASG_HOST_STATE_RENDERING) {
            notifyAvailable();
        }
        if (isInError()) {
            return;
        }
    }
}

int RingStreamClient::type1Write(uint32_t bufferOffset, size_t size) {
    ensureType3Finished();

    size_t sent = 0;
    size_t sizeForRing = sizeof(struct asg_type1_xfer);

    struct asg_type1_xfer xfer = {
        bufferOffset,
        (uint32_t)size,
    };

    uint8_t* writeBufferBytes = (uint8_t*)(&xfer);

    uint32_t maxOutstanding = 1;
    uint32_t maxSteps = m_context.ring_config->buffer_size /
            m_context.ring_config->flush_interval;

    if (maxSteps > 1) maxOutstanding = maxSteps - 1;

    uint32_t ringAvailReadNow = ring_buffer_available_read(m_context.to_host, 0);

    while (ringAvailReadNow >= maxOutstanding * sizeForRing) {
        ringAvailReadNow = ring_buffer_available_read(m_context.to_host, 0);
    }

    bool hostPinged = false;
    while (sent < sizeForRing) {

        long sentChunks = ring_buffer_write(
            m_context.to_host,
            writeBufferBytes + sent,
            sizeForRing - sent, 1);

        if (!hostPinged &&
            *(m_context.host_state) != ASG_HOST_STATE_CAN_CONSUME &&
            *(m_context.host_state) != ASG_HOST_STATE_RENDERING) {
            notifyAvailable();
            hostPinged = true;
        }

        if (sentChunks == 0) {
            ring_buffer_yield();
            backoff();
        }

        sent += sentChunks * (sizeForRing - sent);

        if (isInError()) {
            return -1;
        }
    }

    bool isRenderingAfter = ASG_HOST_STATE_RENDERING == __atomic_load_n(m_context.host_state, __ATOMIC_ACQUIRE);

    if (!isRenderingAfter) {
        notifyAvailable();
    }

    m_written += size;

    float mb = (float)m_written / 1048576.0f;
    if (mb > 100.0f) {
        // ALOGD("%s: %f mb in %d notifs. %f mb/notif\n", __func__,
        //       mb, m_notifs, m_notifs ? mb / (float)m_notifs : 0.0f);
        m_notifs = 0;
        m_written = 0;
    }

    resetBackoff();
    return 0;
}

void RingStreamClient::backoff() {
#if defined(HOST_BUILD) || defined(__APPLE__) || defined(__MACOSX) || defined(__Fuchsia__)
    static const uint32_t kBackoffItersThreshold = 50000000;
    static const uint32_t kBackoffFactorDoublingIncrement = 50000000;
#else
    static const uint32_t kBackoffItersThreshold = property_get_int32("ro.boot.asg.backoffiters", 50000000);
    static const uint32_t kBackoffFactorDoublingIncrement = property_get_int32("ro.boot.asg.backoffincrement", 50000000);
#endif
    ++m_backoffIters;

    if (m_backoffIters > kBackoffItersThreshold) {
        usleep(m_backoffFactor);
        uint32_t itersSoFarAfterThreshold = m_backoffIters - kBackoffItersThreshold;
        if (itersSoFarAfterThreshold > kBackoffFactorDoublingIncrement) {
            m_backoffFactor = m_backoffFactor << 1;
            if (m_backoffFactor > 1000) m_backoffFactor = 1000;
            m_backoffIters = kBackoffItersThreshold;
        }
    }
}

void RingStreamClient::resetBackoff() {
    m_backoffIters = 0;
    m_backoffFactor = 1;
}

} // namespace client
} // namespace asg
