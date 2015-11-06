/*
 * Copyright 2014 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <inttypes.h>

#define LOG_TAG "BufferQueueConsumer"
#define ATRACE_TAG ATRACE_TAG_GRAPHICS
//#define LOG_NDEBUG 0

#include <gui/BufferItem.h>
#include <gui/BufferQueueConsumer.h>
#include <gui/BufferQueueCore.h>
#include <gui/IConsumerListener.h>
#include <gui/IProducerListener.h>

namespace android {

BufferQueueConsumer::BufferQueueConsumer(const sp<BufferQueueCore>& core) :
    mCore(core),
    mSlots(core->mSlots),
    mConsumerName() {}

BufferQueueConsumer::~BufferQueueConsumer() {}

status_t BufferQueueConsumer::acquireBuffer(BufferItem* outBuffer,
        nsecs_t expectedPresent, uint64_t maxFrameNumber) {
    ATRACE_CALL();

    int numDroppedBuffers = 0;
    sp<IProducerListener> listener;
    {
        Mutex::Autolock lock(mCore->mMutex);

        // Check that the consumer doesn't currently have the maximum number of
        // buffers acquired. We allow the max buffer count to be exceeded by one
        // buffer so that the consumer can successfully set up the newly acquired
        // buffer before releasing the old one.
        int numAcquiredBuffers = 0;
        for (int s = 0; s < BufferQueueDefs::NUM_BUFFER_SLOTS; ++s) {
            if (mSlots[s].mBufferState == BufferSlot::ACQUIRED) {
                ++numAcquiredBuffers;
            }
        }
        if (numAcquiredBuffers >= mCore->mMaxAcquiredBufferCount + 1) {
            BQ_LOGE("acquireBuffer: max acquired buffer count reached: %d (max %d)",
                    numAcquiredBuffers, mCore->mMaxAcquiredBufferCount);
            return INVALID_OPERATION;
        }

        // Check if the queue is empty.
        // In asynchronous mode the list is guaranteed to be one buffer deep,
        // while in synchronous mode we use the oldest buffer.
        if (mCore->mQueue.empty()) {
            return NO_BUFFER_AVAILABLE;
        }

        BufferQueueCore::Fifo::iterator front(mCore->mQueue.begin());

        // If expectedPresent is specified, we may not want to return a buffer yet.
        // If it's specified and there's more than one buffer queued, we may want
        // to drop a buffer.
        if (expectedPresent != 0) {
            const int MAX_REASONABLE_NSEC = 1000000000ULL; // 1 second

            // The 'expectedPresent' argument indicates when the buffer is expected
            // to be presented on-screen. If the buffer's desired present time is
            // earlier (less) than expectedPresent -- meaning it will be displayed
            // on time or possibly late if we show it as soon as possible -- we
            // acquire and return it. If we don't want to display it until after the
            // expectedPresent time, we return PRESENT_LATER without acquiring it.
            //
            // To be safe, we don't defer acquisition if expectedPresent is more
            // than one second in the future beyond the desired present time
            // (i.e., we'd be holding the buffer for a long time).
            //
            // NOTE: Code assumes monotonic time values from the system clock
            // are positive.

            // Start by checking to see if we can drop frames. We skip this check if
            // the timestamps are being auto-generated by Surface. If the app isn't
            // generating timestamps explicitly, it probably doesn't want frames to
            // be discarded based on them.

            while (mCore->mQueue.size() > 1 && !mCore->mQueue[0].mIsAutoTimestamp) {
                const BufferItem& bufferItem(mCore->mQueue[1]);

                // If dropping entry[0] would leave us with a buffer that the
                // consumer is not yet ready for, don't drop it.
                if (maxFrameNumber && bufferItem.mFrameNumber > maxFrameNumber) {
                    break;
                }

                // If entry[1] is timely, drop entry[0] (and repeat). We apply an
                // additional criterion here: we only drop the earlier buffer if our
                // desiredPresent falls within +/- 1 second of the expected present.
                // Otherwise, bogus desiredPresent times (e.g., 0 or a small
                // relative timestamp), which normally mean "ignore the timestamp
                // and acquire immediately", would cause us to drop frames.
                //
                // We may want to add an additional criterion: don't drop the
                // earlier buffer if entry[1]'s fence hasn't signaled yet.
                nsecs_t desiredPresent = bufferItem.mTimestamp;
                if (desiredPresent < expectedPresent - MAX_REASONABLE_NSEC ||
                        desiredPresent > expectedPresent) {
                    // This buffer is set to display in the near future, or
                    // desiredPresent is garbage. Either way we don't want to drop
                    // the previous buffer just to get this on the screen sooner.
                    BQ_LOGV("acquireBuffer: nodrop desire=%" PRId64 " expect=%"
                            PRId64 " (%" PRId64 ") now=%" PRId64,
                            desiredPresent, expectedPresent,
                            desiredPresent - expectedPresent,
                            systemTime(CLOCK_MONOTONIC));
                    break;
                }

                BQ_LOGV("acquireBuffer: drop desire=%" PRId64 " expect=%" PRId64
                        " size=%zu",
                        desiredPresent, expectedPresent, mCore->mQueue.size());
                if (mCore->stillTracking(front)) {
                    // Front buffer is still in mSlots, so mark the slot as free
                    mSlots[front->mSlot].mBufferState = BufferSlot::FREE;
                    mCore->mFreeBuffers.push_back(front->mSlot);
                    listener = mCore->mConnectedProducerListener;
                    ++numDroppedBuffers;
                }
                mCore->mQueue.erase(front);
                front = mCore->mQueue.begin();
            }

            // See if the front buffer is ready to be acquired
            nsecs_t desiredPresent = front->mTimestamp;
            bool bufferIsDue = desiredPresent <= expectedPresent ||
                    desiredPresent > expectedPresent + MAX_REASONABLE_NSEC;
            bool consumerIsReady = maxFrameNumber > 0 ?
                    front->mFrameNumber <= maxFrameNumber : true;
            if (!bufferIsDue || !consumerIsReady) {
                BQ_LOGV("acquireBuffer: defer desire=%" PRId64 " expect=%" PRId64
                        " (%" PRId64 ") now=%" PRId64 " frame=%" PRIu64
                        " consumer=%" PRIu64,
                        desiredPresent, expectedPresent,
                        desiredPresent - expectedPresent,
                        systemTime(CLOCK_MONOTONIC),
                        front->mFrameNumber, maxFrameNumber);
                return PRESENT_LATER;
            }

            BQ_LOGV("acquireBuffer: accept desire=%" PRId64 " expect=%" PRId64 " "
                    "(%" PRId64 ") now=%" PRId64, desiredPresent, expectedPresent,
                    desiredPresent - expectedPresent,
                    systemTime(CLOCK_MONOTONIC));
        }

        int slot = front->mSlot;
        *outBuffer = *front;
        ATRACE_BUFFER_INDEX(slot);

        BQ_LOGV("acquireBuffer: acquiring { slot=%d/%" PRIu64 " buffer=%p }",
                slot, front->mFrameNumber, front->mGraphicBuffer->handle);
        // If the front buffer is still being tracked, update its slot state
        if (mCore->stillTracking(front)) {
            mSlots[slot].mAcquireCalled = true;
            mSlots[slot].mNeedsCleanupOnRelease = false;
            mSlots[slot].mBufferState = BufferSlot::ACQUIRED;
            mSlots[slot].mFence = Fence::NO_FENCE;
        }

        // If the buffer has previously been acquired by the consumer, set
        // mGraphicBuffer to NULL to avoid unnecessarily remapping this buffer
        // on the consumer side
        if (outBuffer->mAcquireCalled) {
            outBuffer->mGraphicBuffer = NULL;
        }

        mCore->mQueue.erase(front);

        // We might have freed a slot while dropping old buffers, or the producer
        // may be blocked waiting for the number of buffers in the queue to
        // decrease.
        mCore->mDequeueCondition.broadcast();

        ATRACE_INT(mCore->mConsumerName.string(), mCore->mQueue.size());

        mCore->validateConsistencyLocked();
    }

    if (listener != NULL) {
        for (int i = 0; i < numDroppedBuffers; ++i) {
            listener->onBufferReleased();
        }
    }

    return NO_ERROR;
}

status_t BufferQueueConsumer::detachBuffer(int slot) {
    ATRACE_CALL();
    ATRACE_BUFFER_INDEX(slot);
    BQ_LOGV("detachBuffer(C): slot %d", slot);
    Mutex::Autolock lock(mCore->mMutex);

    if (mCore->mIsAbandoned) {
        BQ_LOGE("detachBuffer(C): BufferQueue has been abandoned");
        return NO_INIT;
    }

    if (slot < 0 || slot >= BufferQueueDefs::NUM_BUFFER_SLOTS) {
        BQ_LOGE("detachBuffer(C): slot index %d out of range [0, %d)",
                slot, BufferQueueDefs::NUM_BUFFER_SLOTS);
        return BAD_VALUE;
    } else if (mSlots[slot].mBufferState != BufferSlot::ACQUIRED) {
        BQ_LOGE("detachBuffer(C): slot %d is not owned by the consumer "
                "(state = %d)", slot, mSlots[slot].mBufferState);
        return BAD_VALUE;
    }

    mCore->freeBufferLocked(slot);
    mCore->mDequeueCondition.broadcast();
    mCore->validateConsistencyLocked();

    return NO_ERROR;
}

status_t BufferQueueConsumer::attachBuffer(int* outSlot,
        const sp<android::GraphicBuffer>& buffer) {
    ATRACE_CALL();

    if (outSlot == NULL) {
        BQ_LOGE("attachBuffer(P): outSlot must not be NULL");
        return BAD_VALUE;
    } else if (buffer == NULL) {
        BQ_LOGE("attachBuffer(P): cannot attach NULL buffer");
        return BAD_VALUE;
    }

    Mutex::Autolock lock(mCore->mMutex);

    // Make sure we don't have too many acquired buffers
    int numAcquiredBuffers = 0;
    for (int s = 0; s < BufferQueueDefs::NUM_BUFFER_SLOTS; ++s) {
        if (mSlots[s].mBufferState == BufferSlot::ACQUIRED) {
            ++numAcquiredBuffers;
        }
    }

    if (numAcquiredBuffers >= mCore->mMaxAcquiredBufferCount + 1) {
        BQ_LOGE("attachBuffer(P): max acquired buffer count reached: %d "
                "(max %d)", numAcquiredBuffers,
                mCore->mMaxAcquiredBufferCount);
        return INVALID_OPERATION;
    }

    if (buffer->getGenerationNumber() != mCore->mGenerationNumber) {
        BQ_LOGE("attachBuffer: generation number mismatch [buffer %u] "
                "[queue %u]", buffer->getGenerationNumber(),
                mCore->mGenerationNumber);
        return BAD_VALUE;
    }

    // Find a free slot to put the buffer into
    int found = BufferQueueCore::INVALID_BUFFER_SLOT;
    if (!mCore->mFreeSlots.empty()) {
        auto slot = mCore->mFreeSlots.begin();
        found = *slot;
        mCore->mFreeSlots.erase(slot);
    } else if (!mCore->mFreeBuffers.empty()) {
        found = mCore->mFreeBuffers.front();
        mCore->mFreeBuffers.remove(found);
    }
    if (found == BufferQueueCore::INVALID_BUFFER_SLOT) {
        BQ_LOGE("attachBuffer(P): could not find free buffer slot");
        return NO_MEMORY;
    }

    *outSlot = found;
    ATRACE_BUFFER_INDEX(*outSlot);
    BQ_LOGV("attachBuffer(C): returning slot %d", *outSlot);

    mSlots[*outSlot].mGraphicBuffer = buffer;
    mSlots[*outSlot].mBufferState = BufferSlot::ACQUIRED;
    mSlots[*outSlot].mAttachedByConsumer = true;
    mSlots[*outSlot].mNeedsCleanupOnRelease = false;
    mSlots[*outSlot].mFence = Fence::NO_FENCE;
    mSlots[*outSlot].mFrameNumber = 0;

    // mAcquireCalled tells BufferQueue that it doesn't need to send a valid
    // GraphicBuffer pointer on the next acquireBuffer call, which decreases
    // Binder traffic by not un/flattening the GraphicBuffer. However, it
    // requires that the consumer maintain a cached copy of the slot <--> buffer
    // mappings, which is why the consumer doesn't need the valid pointer on
    // acquire.
    //
    // The StreamSplitter is one of the primary users of the attach/detach
    // logic, and while it is running, all buffers it acquires are immediately
    // detached, and all buffers it eventually releases are ones that were
    // attached (as opposed to having been obtained from acquireBuffer), so it
    // doesn't make sense to maintain the slot/buffer mappings, which would
    // become invalid for every buffer during detach/attach. By setting this to
    // false, the valid GraphicBuffer pointer will always be sent with acquire
    // for attached buffers.
    mSlots[*outSlot].mAcquireCalled = false;

    mCore->validateConsistencyLocked();

    return NO_ERROR;
}

status_t BufferQueueConsumer::releaseBuffer(int slot, uint64_t frameNumber,
        const sp<Fence>& releaseFence, EGLDisplay eglDisplay,
        EGLSyncKHR eglFence) {
    ATRACE_CALL();
    ATRACE_BUFFER_INDEX(slot);

    if (slot < 0 || slot >= BufferQueueDefs::NUM_BUFFER_SLOTS ||
            releaseFence == NULL) {
        BQ_LOGE("releaseBuffer: slot %d out of range or fence %p NULL", slot,
                releaseFence.get());
        return BAD_VALUE;
    }

    sp<IProducerListener> listener;
    { // Autolock scope
        Mutex::Autolock lock(mCore->mMutex);

        // If the frame number has changed because the buffer has been reallocated,
        // we can ignore this releaseBuffer for the old buffer
        if (frameNumber != mSlots[slot].mFrameNumber) {
            return STALE_BUFFER_SLOT;
        }

        // Make sure this buffer hasn't been queued while acquired by the consumer
        BufferQueueCore::Fifo::iterator current(mCore->mQueue.begin());
        while (current != mCore->mQueue.end()) {
            if (current->mSlot == slot) {
                BQ_LOGE("releaseBuffer: buffer slot %d pending release is "
                        "currently queued", slot);
                return BAD_VALUE;
            }
            ++current;
        }

        if (mSlots[slot].mBufferState == BufferSlot::ACQUIRED) {
            mSlots[slot].mEglDisplay = eglDisplay;
            mSlots[slot].mEglFence = eglFence;
            mSlots[slot].mFence = releaseFence;
            mSlots[slot].mBufferState = BufferSlot::FREE;
            mCore->mFreeBuffers.push_back(slot);
            listener = mCore->mConnectedProducerListener;
            BQ_LOGV("releaseBuffer: releasing slot %d", slot);
        } else if (mSlots[slot].mNeedsCleanupOnRelease) {
            BQ_LOGV("releaseBuffer: releasing a stale buffer slot %d "
                    "(state = %d)", slot, mSlots[slot].mBufferState);
            mSlots[slot].mNeedsCleanupOnRelease = false;
            return STALE_BUFFER_SLOT;
        } else {
            BQ_LOGE("releaseBuffer: attempted to release buffer slot %d "
                    "but its state was %d", slot, mSlots[slot].mBufferState);
            return BAD_VALUE;
        }

        mCore->mDequeueCondition.broadcast();
        mCore->validateConsistencyLocked();
    } // Autolock scope

    // Call back without lock held
    if (listener != NULL) {
        listener->onBufferReleased();
    }

    return NO_ERROR;
}

status_t BufferQueueConsumer::connect(
        const sp<IConsumerListener>& consumerListener, bool controlledByApp) {
    ATRACE_CALL();

    if (consumerListener == NULL) {
        BQ_LOGE("connect(C): consumerListener may not be NULL");
        return BAD_VALUE;
    }

    BQ_LOGV("connect(C): controlledByApp=%s",
            controlledByApp ? "true" : "false");

    Mutex::Autolock lock(mCore->mMutex);

    if (mCore->mIsAbandoned) {
        BQ_LOGE("connect(C): BufferQueue has been abandoned");
        return NO_INIT;
    }

    mCore->mConsumerListener = consumerListener;
    mCore->mConsumerControlledByApp = controlledByApp;

    return NO_ERROR;
}

status_t BufferQueueConsumer::disconnect() {
    ATRACE_CALL();

    BQ_LOGV("disconnect(C)");

    Mutex::Autolock lock(mCore->mMutex);

    if (mCore->mConsumerListener == NULL) {
        BQ_LOGE("disconnect(C): no consumer is connected");
        return BAD_VALUE;
    }

    mCore->mIsAbandoned = true;
    mCore->mConsumerListener = NULL;
    mCore->mQueue.clear();
    mCore->freeAllBuffersLocked();
    mCore->mDequeueCondition.broadcast();
    return NO_ERROR;
}

status_t BufferQueueConsumer::getReleasedBuffers(uint64_t *outSlotMask) {
    ATRACE_CALL();

    if (outSlotMask == NULL) {
        BQ_LOGE("getReleasedBuffers: outSlotMask may not be NULL");
        return BAD_VALUE;
    }

    Mutex::Autolock lock(mCore->mMutex);

    if (mCore->mIsAbandoned) {
        BQ_LOGE("getReleasedBuffers: BufferQueue has been abandoned");
        return NO_INIT;
    }

    uint64_t mask = 0;
    for (int s = 0; s < BufferQueueDefs::NUM_BUFFER_SLOTS; ++s) {
        if (!mSlots[s].mAcquireCalled) {
            mask |= (1ULL << s);
        }
    }

    // Remove from the mask queued buffers for which acquire has been called,
    // since the consumer will not receive their buffer addresses and so must
    // retain their cached information
    BufferQueueCore::Fifo::iterator current(mCore->mQueue.begin());
    while (current != mCore->mQueue.end()) {
        if (current->mAcquireCalled) {
            mask &= ~(1ULL << current->mSlot);
        }
        ++current;
    }

    BQ_LOGV("getReleasedBuffers: returning mask %#" PRIx64, mask);
    *outSlotMask = mask;
    return NO_ERROR;
}

status_t BufferQueueConsumer::setDefaultBufferSize(uint32_t width,
        uint32_t height) {
    ATRACE_CALL();

    if (width == 0 || height == 0) {
        BQ_LOGV("setDefaultBufferSize: dimensions cannot be 0 (width=%u "
                "height=%u)", width, height);
        return BAD_VALUE;
    }

    BQ_LOGV("setDefaultBufferSize: width=%u height=%u", width, height);

    Mutex::Autolock lock(mCore->mMutex);
    mCore->mDefaultWidth = width;
    mCore->mDefaultHeight = height;
    return NO_ERROR;
}

status_t BufferQueueConsumer::setDefaultMaxBufferCount(int bufferCount) {
    ATRACE_CALL();
    Mutex::Autolock lock(mCore->mMutex);
    return mCore->setDefaultMaxBufferCountLocked(bufferCount);
}

status_t BufferQueueConsumer::disableAsyncBuffer() {
    ATRACE_CALL();

    Mutex::Autolock lock(mCore->mMutex);

    if (mCore->mConsumerListener != NULL) {
        BQ_LOGE("disableAsyncBuffer: consumer already connected");
        return INVALID_OPERATION;
    }

    BQ_LOGV("disableAsyncBuffer");
    mCore->mUseAsyncBuffer = false;
    return NO_ERROR;
}

status_t BufferQueueConsumer::setMaxAcquiredBufferCount(
        int maxAcquiredBuffers) {
    ATRACE_CALL();

    if (maxAcquiredBuffers < 1 ||
            maxAcquiredBuffers > BufferQueueCore::MAX_MAX_ACQUIRED_BUFFERS) {
        BQ_LOGE("setMaxAcquiredBufferCount: invalid count %d",
                maxAcquiredBuffers);
        return BAD_VALUE;
    }

    Mutex::Autolock lock(mCore->mMutex);

    if (mCore->mConnectedApi != BufferQueueCore::NO_CONNECTED_API) {
        BQ_LOGE("setMaxAcquiredBufferCount: producer is already connected");
        return INVALID_OPERATION;
    }

    BQ_LOGV("setMaxAcquiredBufferCount: %d", maxAcquiredBuffers);
    mCore->mMaxAcquiredBufferCount = maxAcquiredBuffers;
    return NO_ERROR;
}

void BufferQueueConsumer::setConsumerName(const String8& name) {
    ATRACE_CALL();
    BQ_LOGV("setConsumerName: '%s'", name.string());
    Mutex::Autolock lock(mCore->mMutex);
    mCore->mConsumerName = name;
    mConsumerName = name;
}

status_t BufferQueueConsumer::setDefaultBufferFormat(PixelFormat defaultFormat) {
    ATRACE_CALL();
    BQ_LOGV("setDefaultBufferFormat: %u", defaultFormat);
    Mutex::Autolock lock(mCore->mMutex);
    mCore->mDefaultBufferFormat = defaultFormat;
    return NO_ERROR;
}

status_t BufferQueueConsumer::setDefaultBufferDataSpace(
        android_dataspace defaultDataSpace) {
    ATRACE_CALL();
    BQ_LOGV("setDefaultBufferDataSpace: %u", defaultDataSpace);
    Mutex::Autolock lock(mCore->mMutex);
    mCore->mDefaultBufferDataSpace = defaultDataSpace;
    return NO_ERROR;
}

status_t BufferQueueConsumer::setConsumerUsageBits(uint32_t usage) {
    ATRACE_CALL();
    BQ_LOGV("setConsumerUsageBits: %#x", usage);
    Mutex::Autolock lock(mCore->mMutex);
    mCore->mConsumerUsageBits = usage;
    return NO_ERROR;
}

status_t BufferQueueConsumer::setTransformHint(uint32_t hint) {
    ATRACE_CALL();
    BQ_LOGV("setTransformHint: %#x", hint);
    Mutex::Autolock lock(mCore->mMutex);
    mCore->mTransformHint = hint;
    return NO_ERROR;
}

sp<NativeHandle> BufferQueueConsumer::getSidebandStream() const {
    return mCore->mSidebandStream;
}

void BufferQueueConsumer::dump(String8& result, const char* prefix) const {
    mCore->dump(result, prefix);
}

} // namespace android
