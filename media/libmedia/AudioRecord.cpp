/*
**
** Copyright 2008, The Android Open Source Project
** Copyright (c) 2012, Code Aurora Forum. All rights reserved.
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

//#define LOG_NDEBUG 0
#define LOG_TAG "AudioRecord"

#include <stdint.h>
#include <sys/types.h>

#include <sched.h>
#include <sys/resource.h>

#include <private/media/AudioTrackShared.h>

#include <media/AudioSystem.h>
#include <media/AudioRecord.h>
#include <media/mediarecorder.h>

#include <binder/IServiceManager.h>
#include <utils/Log.h>
#include <binder/Parcel.h>
#include <binder/IPCThreadState.h>
#include <utils/Timers.h>
#include <utils/Atomic.h>

#include <system/audio.h>
#include <cutils/bitops.h>
#include <cutils/compiler.h>

namespace android {
// ---------------------------------------------------------------------------

// static
status_t AudioRecord::getMinFrameCount(
        int* frameCount,
        uint32_t sampleRate,
        audio_format_t format,
        int channelCount)
{
    size_t size = 0;
    if (AudioSystem::getInputBufferSize(sampleRate, format, channelCount, &size)
            != NO_ERROR) {
        ALOGE("AudioSystem could not query the input buffer size.");
        return NO_INIT;
    }

    if (size == 0) {
        ALOGE("Unsupported configuration: sampleRate %d, format %d, channelCount %d",
            sampleRate, format, channelCount);
        return BAD_VALUE;
    }

    // We double the size of input buffer for ping pong use of record buffer.
    size <<= 1;

    if (audio_is_linear_pcm(format)) {
        size /= channelCount * audio_bytes_per_sample(format);
    }

    *frameCount = size;
    return NO_ERROR;
}

// ---------------------------------------------------------------------------

AudioRecord::AudioRecord()
    : mStatus(NO_INIT), mSessionId(0),
      mPreviousPriority(ANDROID_PRIORITY_NORMAL), mPreviousSchedulingGroup(SP_DEFAULT)
{
}

AudioRecord::AudioRecord(
        audio_source_t inputSource,
        uint32_t sampleRate,
        audio_format_t format,
        uint32_t channelMask,
        int frameCount,
        record_flags flags,
        callback_t cbf,
        void* user,
        int notificationFrames,
        int sessionId)
    : mStatus(NO_INIT), mSessionId(0),
      mPreviousPriority(ANDROID_PRIORITY_NORMAL), mPreviousSchedulingGroup(SP_DEFAULT)
{
    mStatus = set(inputSource, sampleRate, format, channelMask,
            frameCount, flags, cbf, user, notificationFrames, sessionId);
}

#ifdef USE_KINETO_COMPATIBILITY
// Really dirty hack to give a Froyo-compatible constructor
extern "C" AudioRecord *_ZN7android11AudioRecordC1EijijijPFviPvS1_ES1_ii(
        AudioRecord *This,
        int inputSource,
        uint32_t sampleRate,
        int format,
        uint32_t channels,
        int frameCount,
        uint32_t flags,
        AudioRecord::callback_t cbf,
        void* user,
        int notificationFrames,
        int sessionId);
extern "C" AudioRecord *_ZN7android11AudioRecordC1EijijijPFviPvS1_ES1_i(
        AudioRecord *This,
        int inputSource,
        uint32_t sampleRate,
        int format,
        uint32_t channels,
        int frameCount,
        uint32_t flags,
        AudioRecord::callback_t cbf,
        void* user,
        int notificationFrames)
{
    return _ZN7android11AudioRecordC1EijijijPFviPvS1_ES1_ii(This,
        inputSource, sampleRate, format, channels,
        frameCount, flags, cbf, user, notificationFrames, 0);
}
#endif

AudioRecord::~AudioRecord()
{
    if (mStatus == NO_ERROR) {
        // Make sure that callback function exits in the case where
        // it is looping on buffer empty condition in obtainBuffer().
        // Otherwise the callback thread will never exit.
        stop();
        if (mClientRecordThread != 0) {
            mClientRecordThread->requestExitAndWait();
            mClientRecordThread.clear();
        }
        mAudioRecord.clear();
        IPCThreadState::self()->flushCommands();
        AudioSystem::releaseAudioSessionId(mSessionId);
    }
}

status_t AudioRecord::set(
        audio_source_t inputSource,
        uint32_t sampleRate,
        audio_format_t format,
        uint32_t channelMask,
        int frameCount,
        record_flags flags,
        callback_t cbf,
        void* user,
        int notificationFrames,
        bool threadCanCallJava,
        int sessionId)
{

    ALOGV("set(): sampleRate %d, channelMask %d, frameCount %d",sampleRate, channelMask, frameCount);

    AutoMutex lock(mLock);

    if (mAudioRecord != 0) {
        return INVALID_OPERATION;
    }

    if (inputSource == AUDIO_SOURCE_DEFAULT) {
        inputSource = AUDIO_SOURCE_MIC;
    }

    if (sampleRate == 0) {
        sampleRate = DEFAULT_SAMPLE_RATE;
    }
    // these below should probably come from the audioFlinger too...
    if (format == AUDIO_FORMAT_DEFAULT) {
        format = AUDIO_FORMAT_PCM_16_BIT;
    }
    // validate parameters
    if (!audio_is_valid_format(format)) {
        ALOGE("Invalid format");
        return BAD_VALUE;
    }

    if (!audio_is_input_channel(channelMask)) {
        return BAD_VALUE;
    }

#ifdef QCOM_HARDWARE
    int channelCount = popcount((channelMask) & (AUDIO_CHANNEL_IN_STEREO | AUDIO_CHANNEL_IN_MONO));
#else
    int channelCount = popcount(channelMask);
#endif

    if (sessionId == 0 ) {
        mSessionId = AudioSystem::newAudioSessionId();
    } else {
        mSessionId = sessionId;
    }
    ALOGV("set(): mSessionId %d", mSessionId);

    audio_io_handle_t input = AudioSystem::getInput(inputSource,
                                                    sampleRate,
                                                    format,
                                                    channelMask,
                                                    (audio_in_acoustics_t)flags,
                                                    mSessionId);
    if (input == 0) {
        ALOGE("Could not get audio input for record source %d", inputSource);
        return BAD_VALUE;
    }

#ifdef QCOM_HARDWARE
    size_t inputBuffSizeInBytes = -1;
    if (AudioSystem::getInputBufferSize(sampleRate, format, channelCount, &inputBuffSizeInBytes)
            != NO_ERROR) {
        ALOGE("AudioSystem could not query the input buffer size.");
        return NO_INIT;
    }
    ALOGV("AudioRecord::set() inputBuffSizeInBytes = %d", inputBuffSizeInBytes );

    if (inputBuffSizeInBytes == 0) {
        ALOGE("Recording parameters are not supported: sampleRate %d, channelCount %d, format %d",
            sampleRate, channelCount, format);
        return BAD_VALUE;
    }

    mFirstread = false;
    // Change for Codec type
    int frameSizeInBytes = 0;
    if(inputSource == AUDIO_SOURCE_VOICE_COMMUNICATION) {
        if (audio_is_linear_pcm(format)) {
             frameSizeInBytes = channelCount * (format == AUDIO_FORMAT_PCM_16_BIT ? sizeof(int16_t) 
: sizeof(int8_t));
        } else {
             frameSizeInBytes = channelCount *sizeof(int16_t);
        }
        mFirstread = true;
    } else {
        if (format ==AUDIO_FORMAT_AMR_NB) {
             frameSizeInBytes = channelCount * 32; // Full rate framesize
        } else if (format ==AUDIO_FORMAT_EVRC) {
             frameSizeInBytes = channelCount * 23; // Full rate framesize
        } else if (format ==AUDIO_FORMAT_QCELP) {
             frameSizeInBytes = channelCount * 35; // Full rate framesize
        } else if (format ==AUDIO_FORMAT_AAC) {
             frameSizeInBytes = 2048;
        } else if ((format ==AUDIO_FORMAT_PCM_16_BIT) || (format ==AUDIO_FORMAT_PCM_8_BIT)) {
             if (audio_is_linear_pcm(format)) {
                  frameSizeInBytes = channelCount * (format == AUDIO_FORMAT_PCM_16_BIT ? sizeof(int16_t) : sizeof(int8_t));
             } else {
                  frameSizeInBytes = sizeof(int8_t);
             }
             mFirstread = true;
        }
    }
    // We use 2* size of input buffer for ping pong use of record buffer.
    int minFrameCount = 2 * inputBuffSizeInBytes / frameSizeInBytes;
#else
    int minFrameCount = 0;
    status_t status = getMinFrameCount(&minFrameCount, sampleRate, format, channelCount);
    if (status != NO_ERROR) {
        return status;
    }
#endif

    ALOGV("AudioRecord::set() minFrameCount = %d", minFrameCount);
    if (frameCount == 0) {
        frameCount = minFrameCount;
    } else if (frameCount < minFrameCount) {
        return BAD_VALUE;
    }

    if (notificationFrames == 0) {
        notificationFrames = frameCount/2;
    }

    mInputSource = inputSource;
    // create the IAudioRecord
#ifdef QCOM_HARDWARE
    status_t status = openRecord_l(sampleRate, format, channelMask,
                        frameCount, flags, input);
#else
    status = openRecord_l(sampleRate, format, channelMask,
                        frameCount, input);
#endif

    if (status != NO_ERROR) {
        return status;
    }

    if (cbf != NULL) {
        mClientRecordThread = new ClientRecordThread(*this, threadCanCallJava);
    }

    mStatus = NO_ERROR;

    mFormat = format;
    // Update buffer size in case it has been limited by AudioFlinger during track creation
    mFrameCount = mCblk->frameCount;
    mChannelCount = (uint8_t)channelCount;
    mChannelMask = channelMask;
    mActive = 0;
    mCbf = cbf;
    mNotificationFrames = notificationFrames;
    mRemainingFrames = notificationFrames;
    mUserData = user;
    // TODO: add audio hardware input latency here
    mLatency = (1000*mFrameCount) / sampleRate;
    mMarkerPosition = 0;
    mMarkerReached = false;
    mNewPosition = 0;
    mUpdatePeriod = 0;
    mFlags = flags;
    mInput = input;
#ifdef QCOM_HARDWARE
    mFirstread = false;
#endif
    AudioSystem::acquireAudioSessionId(mSessionId);

    return NO_ERROR;
}

status_t AudioRecord::initCheck() const
{
    return mStatus;
}

// -------------------------------------------------------------------------

uint32_t AudioRecord::latency() const
{
    return mLatency;
}

audio_format_t AudioRecord::format() const
{
    return mFormat;
}

int AudioRecord::channelCount() const
{
    return mChannelCount;
}

uint32_t AudioRecord::frameCount() const
{
    return mFrameCount;
}

size_t AudioRecord::frameSize() const
{
#ifdef QCOM_HARDWARE
    if(inputSource() == AUDIO_SOURCE_VOICE_COMMUNICATION) {
        if (audio_is_linear_pcm(mFormat)) {
             return channelCount()*audio_bytes_per_sample(mFormat);
        } else {
            return channelCount()*sizeof(int16_t);
        }
    } else {
        if (format() ==AUDIO_FORMAT_AMR_NB) {
            return channelCount() * 32; // Full rate framesize
        } else if (format() == AUDIO_FORMAT_EVRC) {
            return channelCount() * 23; // Full rate framesize
        } else if (format() == AUDIO_FORMAT_QCELP) {
            return channelCount() * 35; // Full rate framesize
        } else if (format() == AUDIO_FORMAT_AAC) {
            // Not actual framsize but for variable frame rate AAC encoding,
            // buffer size is treated as a frame size
            return 2048;
        }
#endif
        if (audio_is_linear_pcm(mFormat)) {
            return channelCount()*audio_bytes_per_sample(mFormat);
        } else {
            return sizeof(uint8_t);
        }
#ifdef QCOM_HARDWARE
    }
#endif
}

audio_source_t AudioRecord::inputSource() const
{
    return mInputSource;
}

// -------------------------------------------------------------------------

status_t AudioRecord::start(AudioSystem::sync_event_t event, int triggerSession)
{
    status_t ret = NO_ERROR;
    sp<ClientRecordThread> t = mClientRecordThread;

    ALOGV("start, sync event %d trigger session %d", event, triggerSession);

    if (t != 0) {
        if (t->exitPending()) {
            if (t->requestExitAndWait() == WOULD_BLOCK) {
                ALOGE("AudioRecord::start called from thread");
                return WOULD_BLOCK;
            }
        }
    }

    AutoMutex lock(mLock);
    // acquire a strong reference on the IAudioRecord and IMemory so that they cannot be destroyed
    // while we are accessing the cblk
    sp<IAudioRecord> audioRecord = mAudioRecord;
    sp<IMemory> iMem = mCblkMemory;
    audio_track_cblk_t* cblk = mCblk;
    if (mActive == 0) {
        mActive = 1;

        pid_t tid;
        if (t != 0) {
            mReadyToRun = WOULD_BLOCK;
            t->run("AudioRecord", ANDROID_PRIORITY_AUDIO);
            tid = t->getTid();  // pid_t is unknown until run()
            ALOGV("getTid=%d", tid);
            if (tid == -1) {
                tid = 0;
            }
            // thread blocks in readyToRun()
        } else {
            tid = 0;    // not gettid()
        }

        cblk->lock.lock();
        if (!(cblk->flags & CBLK_INVALID_MSK)) {
            cblk->lock.unlock();
            ALOGV("mAudioRecord->start()");
            ret = mAudioRecord->start(event, triggerSession);
            cblk->lock.lock();
            if (ret == DEAD_OBJECT) {
                android_atomic_or(CBLK_INVALID_ON, &cblk->flags);
            }
        }
        if (cblk->flags & CBLK_INVALID_MSK) {
            ret = restoreRecord_l(cblk);
        }
        cblk->lock.unlock();
        if (ret == NO_ERROR) {
            mNewPosition = cblk->user + mUpdatePeriod;
            cblk->bufferTimeoutMs = (event == AudioSystem::SYNC_EVENT_NONE) ? MAX_RUN_TIMEOUT_MS :
                                            AudioSystem::kSyncRecordStartTimeOutMs;
            cblk->waitTimeMs = 0;
            if (t != 0) {
                // thread unblocks in readyToRun() and returns NO_ERROR
                mReadyToRun = NO_ERROR;
                mCondition.signal();
            } else {
                mPreviousPriority = getpriority(PRIO_PROCESS, 0);
                get_sched_policy(0, &mPreviousSchedulingGroup);
                androidSetThreadPriority(0, ANDROID_PRIORITY_AUDIO);
            }
        } else {
            mActive = 0;
            // thread unblocks in readyToRun() and returns NO_INIT
            mReadyToRun = NO_INIT;
            mCondition.signal();
        }
    }

    return ret;
}

status_t AudioRecord::stop()
{
    sp<ClientRecordThread> t = mClientRecordThread;

    ALOGV("stop");

    AutoMutex lock(mLock);
    if (mActive == 1) {
        mActive = 0;
        mCblk->cv.signal();
        mAudioRecord->stop();
        // the record head position will reset to 0, so if a marker is set, we need
        // to activate it again
        mMarkerReached = false;
        if (t != 0) {
            t->requestExit();
        } else {
            setpriority(PRIO_PROCESS, 0, mPreviousPriority);
            set_sched_policy(0, mPreviousSchedulingGroup);
        }
    }

    return NO_ERROR;
}

bool AudioRecord::stopped() const
{
    return !mActive;
}

uint32_t AudioRecord::getSampleRate() const
{
    AutoMutex lock(mLock);
    return mCblk->sampleRate;
}

status_t AudioRecord::setMarkerPosition(uint32_t marker)
{
    if (mCbf == NULL) return INVALID_OPERATION;

    mMarkerPosition = marker;
    mMarkerReached = false;

    return NO_ERROR;
}

status_t AudioRecord::getMarkerPosition(uint32_t *marker) const
{
    if (marker == NULL) return BAD_VALUE;

    *marker = mMarkerPosition;

    return NO_ERROR;
}

status_t AudioRecord::setPositionUpdatePeriod(uint32_t updatePeriod)
{
    if (mCbf == NULL) return INVALID_OPERATION;

    uint32_t curPosition;
    getPosition(&curPosition);
    mNewPosition = curPosition + updatePeriod;
    mUpdatePeriod = updatePeriod;

    return NO_ERROR;
}

status_t AudioRecord::getPositionUpdatePeriod(uint32_t *updatePeriod) const
{
    if (updatePeriod == NULL) return BAD_VALUE;

    *updatePeriod = mUpdatePeriod;

    return NO_ERROR;
}

status_t AudioRecord::getPosition(uint32_t *position) const
{
    if (position == NULL) return BAD_VALUE;

    AutoMutex lock(mLock);
    *position = mCblk->user;

    return NO_ERROR;
}

unsigned int AudioRecord::getInputFramesLost() const
{
    if (mActive)
        return AudioSystem::getInputFramesLost(mInput);
    else
        return 0;
}

// -------------------------------------------------------------------------

// must be called with mLock held
status_t AudioRecord::openRecord_l(
        uint32_t sampleRate,
        audio_format_t format,
        uint32_t channelMask,
        int frameCount,
#ifdef QCOM_HARDWARE
        uint32_t flags,
#endif
        audio_io_handle_t input)
{
    status_t status;
    const sp<IAudioFlinger>& audioFlinger = AudioSystem::get_audio_flinger();
    if (audioFlinger == 0) {
        return NO_INIT;
    }

    sp<IAudioRecord> record = audioFlinger->openRecord(getpid(), input,
                                                       sampleRate, format,
                                                       channelMask,
                                                       frameCount,
#ifdef QCOM_HARDWARE
                                                       (((uint16_t)flags) << 16 |
                                                       ((int16_t)(inputSource()))),
#else
                                                       IAudioFlinger::TRACK_DEFAULT,
#endif
                                                       &mSessionId,
                                                       &status);

    if (record == 0) {
        ALOGE("AudioFlinger could not create record track, status: %d", status);
        return status;
    }
    sp<IMemory> cblk = record->getCblk();
    if (cblk == 0) {
        ALOGE("Could not get control block");
        return NO_INIT;
    }
    mAudioRecord.clear();
    mAudioRecord = record;
    mCblkMemory.clear();
    mCblkMemory = cblk;
    mCblk = static_cast<audio_track_cblk_t*>(cblk->pointer());
    mCblk->buffers = (char*)mCblk + sizeof(audio_track_cblk_t);
    android_atomic_and(~CBLK_DIRECTION_MSK, &mCblk->flags);
    mCblk->bufferTimeoutMs = MAX_RUN_TIMEOUT_MS;
    mCblk->waitTimeMs = 0;
    return NO_ERROR;
}

status_t AudioRecord::obtainBuffer(Buffer* audioBuffer, int32_t waitCount)
{
    AutoMutex lock(mLock);
    int active;
    status_t result = NO_ERROR;
    audio_track_cblk_t* cblk = mCblk;
    uint32_t framesReq = audioBuffer->frameCount;
    uint32_t waitTimeMs = (waitCount < 0) ? cblk->bufferTimeoutMs : WAIT_PERIOD_MS;

    audioBuffer->frameCount  = 0;
    audioBuffer->size        = 0;

    uint32_t framesReady = cblk->framesReady();

    if (framesReady == 0) {
        cblk->lock.lock();
        goto start_loop_here;
        while (framesReady == 0) {
            active = mActive;
            if (CC_UNLIKELY(!active)) {
                cblk->lock.unlock();
                return NO_MORE_BUFFERS;
            }
            if (CC_UNLIKELY(!waitCount)) {
                cblk->lock.unlock();
                return WOULD_BLOCK;
            }
            if (!(cblk->flags & CBLK_INVALID_MSK)) {
                mLock.unlock();
                result = cblk->cv.waitRelative(cblk->lock, milliseconds(waitTimeMs));
                cblk->lock.unlock();
                mLock.lock();
                if (mActive == 0) {
                    return status_t(STOPPED);
                }
                cblk->lock.lock();
            }
            if (cblk->flags & CBLK_INVALID_MSK) {
                goto create_new_record;
            }
            if (CC_UNLIKELY(result != NO_ERROR)) {
                cblk->waitTimeMs += waitTimeMs;
                if (cblk->waitTimeMs >= cblk->bufferTimeoutMs) {
                    ALOGW(   "obtainBuffer timed out (is the CPU pegged?) "
                            "user=%08x, server=%08x", cblk->user, cblk->server);
                    cblk->lock.unlock();
                    // callback thread or sync event hasn't changed
                    result = mAudioRecord->start(AudioSystem::SYNC_EVENT_SAME, 0);
                    cblk->lock.lock();
                    if (result == DEAD_OBJECT) {
                        android_atomic_or(CBLK_INVALID_ON, &cblk->flags);
create_new_record:
                        result = AudioRecord::restoreRecord_l(cblk);
                    }
                    if (result != NO_ERROR) {
                        ALOGW("obtainBuffer create Track error %d", result);
                        cblk->lock.unlock();
                        return result;
                    }
                    cblk->waitTimeMs = 0;
                }
                if (--waitCount == 0) {
                    cblk->lock.unlock();
                    return TIMED_OUT;
                }
            }
            // read the server count again
        start_loop_here:
            framesReady = cblk->framesReady();
        }
        cblk->lock.unlock();
    }

    cblk->waitTimeMs = 0;
    // reset time out to running value after obtaining a buffer
    cblk->bufferTimeoutMs = MAX_RUN_TIMEOUT_MS;

    if (framesReq > framesReady) {
        framesReq = framesReady;
    }

    uint32_t u = cblk->user;
    uint32_t bufferEnd = cblk->userBase + cblk->frameCount;

    if (framesReq > bufferEnd - u) {
        framesReq = bufferEnd - u;
    }


    audioBuffer->flags       = 0;
    audioBuffer->channelCount= mChannelCount;
    audioBuffer->format      = mFormat;
    audioBuffer->frameCount  = framesReq;
    audioBuffer->size        = framesReq*cblk->frameSize;
    audioBuffer->raw         = (int8_t*)cblk->buffer(u);
    active = mActive;
    return active ? status_t(NO_ERROR) : status_t(STOPPED);
}

void AudioRecord::releaseBuffer(Buffer* audioBuffer)
{
    AutoMutex lock(mLock);
    mCblk->stepUser(audioBuffer->frameCount);
}

audio_io_handle_t AudioRecord::getInput() const
{
    AutoMutex lock(mLock);
    return mInput;
}

// must be called with mLock held
audio_io_handle_t AudioRecord::getInput_l()
{
    mInput = AudioSystem::getInput(mInputSource,
                                mCblk->sampleRate,
                                mFormat,
                                mChannelMask,
                                (audio_in_acoustics_t)mFlags,
                                mSessionId);
    return mInput;
}

int AudioRecord::getSessionId() const
{
    return mSessionId;
}

// -------------------------------------------------------------------------

ssize_t AudioRecord::read(void* buffer, size_t userSize)
{
    ssize_t read = 0;
    Buffer audioBuffer;
    int8_t *dst = static_cast<int8_t*>(buffer);

    if (ssize_t(userSize) < 0) {
        // sanity-check. user is most-likely passing an error code.
        ALOGE("AudioRecord::read(buffer=%p, size=%u (%d)",
                buffer, userSize, userSize);
        return BAD_VALUE;
    }

    mLock.lock();
    // acquire a strong reference on the IAudioRecord and IMemory so that they cannot be destroyed
    // while we are accessing the cblk
    sp<IAudioRecord> audioRecord = mAudioRecord;
    sp<IMemory> iMem = mCblkMemory;
    mLock.unlock();

    do {

        audioBuffer.frameCount = userSize/frameSize();
        // By using a wait count corresponding to twice the timeout period in
        // obtainBuffer() we give a chance to recover once for a read timeout
        // (if media_server crashed for instance) before returning a length of
        // 0 bytes read to the client
        status_t err = obtainBuffer(&audioBuffer, ((2 * MAX_RUN_TIMEOUT_MS) / WAIT_PERIOD_MS));
        if (err < 0) {
            // out of buffers, return #bytes written
            if (err == status_t(NO_MORE_BUFFERS))
                break;
            if (err == status_t(TIMED_OUT))
                err = 0;
            return ssize_t(err);
        }

        size_t bytesRead = audioBuffer.size;
        memcpy(dst, audioBuffer.i8, bytesRead);

        dst += bytesRead;
        userSize -= bytesRead;
        read += bytesRead;

        releaseBuffer(&audioBuffer);
#ifdef QCOM_HARDWARE
        if(!mFirstread)
        {
           mFirstread = true;
           break;
        }
#endif
    } while (userSize);

    return read;
}

// -------------------------------------------------------------------------

bool AudioRecord::processAudioBuffer(const sp<ClientRecordThread>& thread)
{
    Buffer audioBuffer;
    uint32_t frames = mRemainingFrames;
    size_t readSize;

    mLock.lock();
    // acquire a strong reference on the IAudioRecord and IMemory so that they cannot be destroyed
    // while we are accessing the cblk
    sp<IAudioRecord> audioRecord = mAudioRecord;
    sp<IMemory> iMem = mCblkMemory;
    audio_track_cblk_t* cblk = mCblk;
    mLock.unlock();

    // Manage marker callback
    if (!mMarkerReached && (mMarkerPosition > 0)) {
        if (cblk->user >= mMarkerPosition) {
            mCbf(EVENT_MARKER, mUserData, (void *)&mMarkerPosition);
            mMarkerReached = true;
        }
    }

    // Manage new position callback
    if (mUpdatePeriod > 0) {
        while (cblk->user >= mNewPosition) {
            mCbf(EVENT_NEW_POS, mUserData, (void *)&mNewPosition);
            mNewPosition += mUpdatePeriod;
        }
    }

    do {
        audioBuffer.frameCount = frames;
        // Calling obtainBuffer() with a wait count of 1
        // limits wait time to WAIT_PERIOD_MS. This prevents from being
        // stuck here not being able to handle timed events (position, markers).
        status_t err = obtainBuffer(&audioBuffer, 1);
        if (err < NO_ERROR) {
            if (err != TIMED_OUT) {
                ALOGE_IF(err != status_t(NO_MORE_BUFFERS), "Error obtaining an audio buffer, giving up.");
                return false;
            }
            break;
        }
        if (err == status_t(STOPPED)) return false;

        size_t reqSize = audioBuffer.size;
        mCbf(EVENT_MORE_DATA, mUserData, &audioBuffer);
        readSize = audioBuffer.size;

        // Sanity check on returned size
        if (ssize_t(readSize) <= 0) {
            // The callback is done filling buffers
            // Keep this thread going to handle timed events and
            // still try to get more data in intervals of WAIT_PERIOD_MS
            // but don't just loop and block the CPU, so wait
            usleep(WAIT_PERIOD_MS*1000);
            break;
        }
        if (readSize > reqSize) readSize = reqSize;

        audioBuffer.size = readSize;
        audioBuffer.frameCount = readSize/frameSize();
        frames -= audioBuffer.frameCount;

        releaseBuffer(&audioBuffer);
#ifdef QCOM_HARDWARE
        if(!mFirstread)
        {
           mFirstread = true;
           break;
        }
#endif

    } while (frames);


    // Manage overrun callback
    if (mActive && (cblk->framesAvailable() == 0)) {
        ALOGV("Overrun user: %x, server: %x, flags %04x", cblk->user, cblk->server, cblk->flags);
        if (!(android_atomic_or(CBLK_UNDERRUN_ON, &cblk->flags) & CBLK_UNDERRUN_MSK)) {
            mCbf(EVENT_OVERRUN, mUserData, 0);
        }
    }

    if (frames == 0) {
        mRemainingFrames = mNotificationFrames;
    } else {
        mRemainingFrames = frames;
    }
    return true;
}

// must be called with mLock and cblk.lock held. Callers must also hold strong references on
// the IAudioRecord and IMemory in case they are recreated here.
// If the IAudioRecord is successfully restored, the cblk pointer is updated
status_t AudioRecord::restoreRecord_l(audio_track_cblk_t*& cblk)
{
    status_t result;

    if (!(android_atomic_or(CBLK_RESTORING_ON, &cblk->flags) & CBLK_RESTORING_MSK)) {
        ALOGW("dead IAudioRecord, creating a new one");
        // signal old cblk condition so that other threads waiting for available buffers stop
        // waiting now
        cblk->cv.broadcast();
        cblk->lock.unlock();

        // if the new IAudioRecord is created, openRecord_l() will modify the
        // following member variables: mAudioRecord, mCblkMemory and mCblk.
        // It will also delete the strong references on previous IAudioRecord and IMemory
        result = openRecord_l(cblk->sampleRate, mFormat, mChannelMask,
                mFrameCount,
#ifdef QCOM_HARDWARE
                0,
#endif
                getInput_l());
        if (result == NO_ERROR) {
            // callback thread or sync event hasn't changed
            result = mAudioRecord->start(AudioSystem::SYNC_EVENT_SAME, 0);
        }
        if (result != NO_ERROR) {
            mActive = false;
        }

        // signal old cblk condition for other threads waiting for restore completion
        android_atomic_or(CBLK_RESTORED_ON, &cblk->flags);
        cblk->cv.broadcast();
    } else {
        if (!(cblk->flags & CBLK_RESTORED_MSK)) {
            ALOGW("dead IAudioRecord, waiting for a new one to be created");
            mLock.unlock();
            result = cblk->cv.waitRelative(cblk->lock, milliseconds(RESTORE_TIMEOUT_MS));
            cblk->lock.unlock();
            mLock.lock();
        } else {
            ALOGW("dead IAudioRecord, already restored");
            result = NO_ERROR;
            cblk->lock.unlock();
        }
        if (result != NO_ERROR || mActive == 0) {
            result = status_t(STOPPED);
        }
    }
    ALOGV("restoreRecord_l() status %d mActive %d cblk %p, old cblk %p flags %08x old flags %08x",
        result, mActive, mCblk, cblk, mCblk->flags, cblk->flags);

    if (result == NO_ERROR) {
        // from now on we switch to the newly created cblk
        cblk = mCblk;
    }
    cblk->lock.lock();

    ALOGW_IF(result != NO_ERROR, "restoreRecord_l() error %d", result);

    return result;
}

// =========================================================================

AudioRecord::ClientRecordThread::ClientRecordThread(AudioRecord& receiver, bool bCanCallJava)
    : Thread(bCanCallJava), mReceiver(receiver)
{
}

bool AudioRecord::ClientRecordThread::threadLoop()
{
    return mReceiver.processAudioBuffer(this);
}

status_t AudioRecord::ClientRecordThread::readyToRun()
{
    AutoMutex(mReceiver.mLock);
    while (mReceiver.mReadyToRun == WOULD_BLOCK) {
        mReceiver.mCondition.wait(mReceiver.mLock);
    }
    return mReceiver.mReadyToRun;
}

// -------------------------------------------------------------------------

}; // namespace android
