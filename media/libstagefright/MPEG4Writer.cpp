/*
 * Copyright (C) 2009 The Android Open Source Project
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

//#define LOG_NDEBUG 0
#define LOG_TAG "MPEG4Writer"
#include <utils/Log.h>

#include <arpa/inet.h>

#include <ctype.h>
#include <pthread.h>

#include <media/stagefright/MPEG4Writer.h>
#include <media/stagefright/MediaBuffer.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/MediaDebug.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/MediaSource.h>
#include <media/stagefright/Utils.h>
#include <media/mediarecorder.h>

namespace android {

class MPEG4Writer::Track {
public:
    Track(MPEG4Writer *owner, const sp<MediaSource> &source);
    ~Track();

    status_t start();
    void stop();
    bool reachedEOS();

    int64_t getDurationUs() const;
    int64_t getEstimatedTrackSizeBytes() const;
    void writeTrackHeader(int32_t trackID);

private:
    MPEG4Writer *mOwner;
    sp<MetaData> mMeta;
    sp<MediaSource> mSource;
    volatile bool mDone;
    int64_t mMaxTimeStampUs;
    int64_t mEstimatedTrackSizeBytes;

    pthread_t mThread;

    struct SampleInfo {
        size_t size;
        int64_t timestamp;
    };
    List<SampleInfo>    mSampleInfos;
    bool                mSamplesHaveSameSize;

    List<MediaBuffer *> mChunkSamples;
    List<off_t>         mChunkOffsets;

    struct StscTableEntry {

        StscTableEntry(uint32_t chunk, uint32_t samples, uint32_t id)
            : firstChunk(chunk),
              samplesPerChunk(samples),
              sampleDescriptionId(id) {}

        uint32_t firstChunk;
        uint32_t samplesPerChunk;
        uint32_t sampleDescriptionId;
    };
    List<StscTableEntry> mStscTableEntries;

    List<int32_t> mStssTableEntries;

    struct SttsTableEntry {

        SttsTableEntry(uint32_t count, uint32_t duration)
            : sampleCount(count), sampleDuration(duration) {}

        uint32_t sampleCount;
        uint32_t sampleDuration;
    };
    List<SttsTableEntry> mSttsTableEntries;

    void *mCodecSpecificData;
    size_t mCodecSpecificDataSize;
    bool mGotAllCodecSpecificData;

    bool mReachedEOS;
    int64_t mStartTimestampUs;

    static void *ThreadWrapper(void *me);
    void threadEntry();

    status_t makeAVCCodecSpecificData(
            const uint8_t *data, size_t size);
    void writeOneChunk(bool isAvc);

    Track(const Track &);
    Track &operator=(const Track &);
};

#define USE_NALLEN_FOUR         1

MPEG4Writer::MPEG4Writer(const char *filename)
    : mFile(fopen(filename, "wb")),
      mOffset(0),
      mMdatOffset(0),
      mEstimatedMoovBoxSize(0),
      mInterleaveDurationUs(500000) {
    CHECK(mFile != NULL);
}

MPEG4Writer::MPEG4Writer(int fd)
    : mFile(fdopen(fd, "wb")),
      mOffset(0),
      mMdatOffset(0),
      mEstimatedMoovBoxSize(0),
      mInterleaveDurationUs(500000) {
    CHECK(mFile != NULL);
}

MPEG4Writer::~MPEG4Writer() {
    stop();

    for (List<Track *>::iterator it = mTracks.begin();
         it != mTracks.end(); ++it) {
        delete *it;
    }
    mTracks.clear();
}

status_t MPEG4Writer::addSource(const sp<MediaSource> &source) {
    Track *track = new Track(this, source);
    mTracks.push_back(track);

    return OK;
}

status_t MPEG4Writer::start() {
    if (mFile == NULL) {
        return UNKNOWN_ERROR;
    }

    mStartTimestampUs = 0;
    mStreamableFile = true;
    mWriteMoovBoxToMemory = false;
    mMoovBoxBuffer = NULL;
    mMoovBoxBufferOffset = 0;

    beginBox("ftyp");
      writeFourcc("isom");
      writeInt32(0);
      writeFourcc("isom");
    endBox();

    mFreeBoxOffset = mOffset;

    if (mEstimatedMoovBoxSize == 0) {
        // XXX: Estimate the moov box size
        //      based on max file size or duration limit
        mEstimatedMoovBoxSize = 0x0F00;
    }
    CHECK(mEstimatedMoovBoxSize >= 8);
    fseeko(mFile, mFreeBoxOffset, SEEK_SET);
    writeInt32(mEstimatedMoovBoxSize);
    write("free", 4);

    mMdatOffset = mFreeBoxOffset + mEstimatedMoovBoxSize;
    mOffset = mMdatOffset;
    fseeko(mFile, mMdatOffset, SEEK_SET);
    write("\x00\x00\x00\x01mdat????????", 16);
    for (List<Track *>::iterator it = mTracks.begin();
         it != mTracks.end(); ++it) {
        status_t err = (*it)->start();

        if (err != OK) {
            for (List<Track *>::iterator it2 = mTracks.begin();
                 it2 != it; ++it2) {
                (*it2)->stop();
            }

            return err;
        }
    }

    return OK;
}

void MPEG4Writer::stop() {
    if (mFile == NULL) {
        return;
    }

    int64_t max_duration = 0;
    for (List<Track *>::iterator it = mTracks.begin();
         it != mTracks.end(); ++it) {
        (*it)->stop();

        int64_t duration = (*it)->getDurationUs();
        if (duration > max_duration) {
            max_duration = duration;
        }
    }


    // Fix up the size of the 'mdat' chunk.
    fseeko(mFile, mMdatOffset + 8, SEEK_SET);
    int64_t size = mOffset - mMdatOffset;
    size = hton64(size);
    fwrite(&size, 1, 8, mFile);
    fseeko(mFile, mOffset, SEEK_SET);

    time_t now = time(NULL);
    const off_t moovOffset = mOffset;
    mWriteMoovBoxToMemory = true;
    mMoovBoxBuffer = (uint8_t *) malloc(mEstimatedMoovBoxSize);
    mMoovBoxBufferOffset = 0;
    CHECK(mMoovBoxBuffer != NULL);

    beginBox("moov");

      beginBox("mvhd");
        writeInt32(0);             // version=0, flags=0
        writeInt32(now);           // creation time
        writeInt32(now);           // modification time
        writeInt32(1000);          // timescale
        writeInt32(max_duration / 1000);
        writeInt32(0x10000);       // rate
        writeInt16(0x100);         // volume
        writeInt16(0);             // reserved
        writeInt32(0);             // reserved
        writeInt32(0);             // reserved
        writeInt32(0x10000);       // matrix
        writeInt32(0);
        writeInt32(0);
        writeInt32(0);
        writeInt32(0x10000);
        writeInt32(0);
        writeInt32(0);
        writeInt32(0);
        writeInt32(0x40000000);
        writeInt32(0);             // predefined
        writeInt32(0);             // predefined
        writeInt32(0);             // predefined
        writeInt32(0);             // predefined
        writeInt32(0);             // predefined
        writeInt32(0);             // predefined
        writeInt32(mTracks.size() + 1);  // nextTrackID
      endBox();  // mvhd

      int32_t id = 1;
      for (List<Track *>::iterator it = mTracks.begin();
           it != mTracks.end(); ++it, ++id) {
          (*it)->writeTrackHeader(id);
      }
    endBox();  // moov

    mWriteMoovBoxToMemory = false;
    if (mStreamableFile) {
        CHECK(mMoovBoxBufferOffset + 8 <= mEstimatedMoovBoxSize);

        // Moov box
        fseeko(mFile, mFreeBoxOffset, SEEK_SET);
        mOffset = mFreeBoxOffset;
        write(mMoovBoxBuffer, 1, mMoovBoxBufferOffset, mFile);

        // Free box
        mFreeBoxOffset = mStreamableFile? mOffset: mFreeBoxOffset;
        fseeko(mFile, mFreeBoxOffset, SEEK_SET);
        writeInt32(mEstimatedMoovBoxSize - mMoovBoxBufferOffset);
        write("free", 4);

        // Free temp memory
        free(mMoovBoxBuffer);
        mMoovBoxBuffer = NULL;
        mMoovBoxBufferOffset = 0;
    }

    CHECK(mBoxes.empty());

    fflush(mFile);
    fclose(mFile);
    mFile = NULL;
}

status_t MPEG4Writer::setInterleaveDuration(uint32_t durationUs) {
    mInterleaveDurationUs = durationUs;
    return OK;
}

void MPEG4Writer::lock() {
    mLock.lock();
}

void MPEG4Writer::unlock() {
    mLock.unlock();
}

off_t MPEG4Writer::addSample_l(MediaBuffer *buffer) {
    off_t old_offset = mOffset;

    fwrite((const uint8_t *)buffer->data() + buffer->range_offset(),
           1, buffer->range_length(), mFile);

    mOffset += buffer->range_length();

    return old_offset;
}

static void StripStartcode(MediaBuffer *buffer) {
    if (buffer->range_length() < 4) {
        return;
    }

    const uint8_t *ptr =
        (const uint8_t *)buffer->data() + buffer->range_offset();

    if (!memcmp(ptr, "\x00\x00\x00\x01", 4)) {
        buffer->set_range(
                buffer->range_offset() + 4, buffer->range_length() - 4);
    }
}

off_t MPEG4Writer::addLengthPrefixedSample_l(MediaBuffer *buffer) {
    off_t old_offset = mOffset;

    size_t length = buffer->range_length();

#if USE_NALLEN_FOUR
    uint8_t x = length >> 24;
    fwrite(&x, 1, 1, mFile);
    x = (length >> 16) & 0xff;
    fwrite(&x, 1, 1, mFile);
    x = (length >> 8) & 0xff;
    fwrite(&x, 1, 1, mFile);
    x = length & 0xff;
    fwrite(&x, 1, 1, mFile);
#else
    CHECK(length < 65536);

    uint8_t x = length >> 8;
    fwrite(&x, 1, 1, mFile);
    x = length & 0xff;
    fwrite(&x, 1, 1, mFile);
#endif

    fwrite((const uint8_t *)buffer->data() + buffer->range_offset(),
           1, length, mFile);

#if USE_NALLEN_FOUR
    mOffset += length + 4;
#else
    mOffset += length + 2;
#endif

    return old_offset;
}

size_t MPEG4Writer::write(
        const void *ptr, size_t size, size_t nmemb, FILE *stream) {

    const size_t bytes = size * nmemb;
    if (mWriteMoovBoxToMemory) {
        if (8 + mMoovBoxBufferOffset + bytes > mEstimatedMoovBoxSize) {
            for (List<off_t>::iterator it = mBoxes.begin();
                 it != mBoxes.end(); ++it) {
                (*it) += mOffset;
            }
            fseeko(mFile, mOffset, SEEK_SET);
            fwrite(mMoovBoxBuffer, 1, mMoovBoxBufferOffset, stream);
            fwrite(ptr, size, nmemb, stream);
            mOffset += (bytes + mMoovBoxBufferOffset);
            free(mMoovBoxBuffer);
            mMoovBoxBuffer = NULL;
            mMoovBoxBufferOffset = 0;
            mWriteMoovBoxToMemory = false;
            mStreamableFile = false;
        } else {
            memcpy(mMoovBoxBuffer + mMoovBoxBufferOffset, ptr, bytes);
            mMoovBoxBufferOffset += bytes;
        }
    } else {
        fwrite(ptr, size, nmemb, stream);
        mOffset += bytes;
    }
    return bytes;
}

void MPEG4Writer::beginBox(const char *fourcc) {
    CHECK_EQ(strlen(fourcc), 4);

    mBoxes.push_back(mWriteMoovBoxToMemory?
            mMoovBoxBufferOffset: mOffset);

    writeInt32(0);
    writeFourcc(fourcc);
}

void MPEG4Writer::endBox() {
    CHECK(!mBoxes.empty());

    off_t offset = *--mBoxes.end();
    mBoxes.erase(--mBoxes.end());

    if (mWriteMoovBoxToMemory) {
       int32_t x = htonl(mMoovBoxBufferOffset - offset);
       memcpy(mMoovBoxBuffer + offset, &x, 4);
    } else {
        fseeko(mFile, offset, SEEK_SET);
        writeInt32(mOffset - offset);
        mOffset -= 4;
        fseeko(mFile, mOffset, SEEK_SET);
    }
}

void MPEG4Writer::writeInt8(int8_t x) {
    write(&x, 1, 1, mFile);
}

void MPEG4Writer::writeInt16(int16_t x) {
    x = htons(x);
    write(&x, 1, 2, mFile);
}

void MPEG4Writer::writeInt32(int32_t x) {
    x = htonl(x);
    write(&x, 1, 4, mFile);
}

void MPEG4Writer::writeInt64(int64_t x) {
    x = hton64(x);
    write(&x, 1, 8, mFile);
}

void MPEG4Writer::writeCString(const char *s) {
    size_t n = strlen(s);
    write(s, 1, n + 1, mFile);
}

void MPEG4Writer::writeFourcc(const char *s) {
    CHECK_EQ(strlen(s), 4);
    write(s, 1, 4, mFile);
}

void MPEG4Writer::write(const void *data, size_t size) {
    write(data, 1, size, mFile);
}

bool MPEG4Writer::exceedsFileSizeLimit() {
    // No limit
    if (mMaxFileSizeLimitBytes == 0) {
        return false;
    }

    int64_t nTotalBytesEstimate = static_cast<int64_t>(mEstimatedMoovBoxSize);
    for (List<Track *>::iterator it = mTracks.begin();
         it != mTracks.end(); ++it) {
        nTotalBytesEstimate += (*it)->getEstimatedTrackSizeBytes();
    }
    return (nTotalBytesEstimate >= mMaxFileSizeLimitBytes);
}

bool MPEG4Writer::exceedsFileDurationLimit() {
    // No limit
    if (mMaxFileDurationLimitUs == 0) {
        return false;
    }

    for (List<Track *>::iterator it = mTracks.begin();
         it != mTracks.end(); ++it) {
        if ((*it)->getDurationUs() >= mMaxFileDurationLimitUs) {
            return true;
        }
    }
    return false;
}

bool MPEG4Writer::reachedEOS() {
    bool allDone = true;
    for (List<Track *>::iterator it = mTracks.begin();
         it != mTracks.end(); ++it) {
        if (!(*it)->reachedEOS()) {
            allDone = false;
            break;
        }
    }

    return allDone;
}

void MPEG4Writer::setStartTimestamp(int64_t timeUs) {
    LOGI("setStartTimestamp: %lld", timeUs);
    Mutex::Autolock autoLock(mLock);
    if (mStartTimestampUs != 0) {
        return;  // Sorry, too late
    }
    mStartTimestampUs = timeUs;
}

int64_t MPEG4Writer::getStartTimestamp() {
    LOGI("getStartTimestamp: %lld", mStartTimestampUs);
    Mutex::Autolock autoLock(mLock);
    return mStartTimestampUs;
}

////////////////////////////////////////////////////////////////////////////////

MPEG4Writer::Track::Track(
        MPEG4Writer *owner, const sp<MediaSource> &source)
    : mOwner(owner),
      mMeta(source->getFormat()),
      mSource(source),
      mDone(false),
      mMaxTimeStampUs(0),
      mEstimatedTrackSizeBytes(0),
      mSamplesHaveSameSize(true),
      mCodecSpecificData(NULL),
      mCodecSpecificDataSize(0),
      mGotAllCodecSpecificData(false),
      mReachedEOS(false) {
}

MPEG4Writer::Track::~Track() {
    stop();

    if (mCodecSpecificData != NULL) {
        free(mCodecSpecificData);
        mCodecSpecificData = NULL;
    }
}

status_t MPEG4Writer::Track::start() {
    status_t err = mSource->start();

    if (err != OK) {
        mDone = mReachedEOS = true;
        return err;
    }

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

    mDone = false;
    mMaxTimeStampUs = 0;
    mReachedEOS = false;
    mEstimatedTrackSizeBytes = 0;

    pthread_create(&mThread, &attr, ThreadWrapper, this);
    pthread_attr_destroy(&attr);

    return OK;
}

void MPEG4Writer::Track::stop() {
    if (mDone) {
        return;
    }

    mDone = true;

    void *dummy;
    pthread_join(mThread, &dummy);

    mSource->stop();
}

bool MPEG4Writer::Track::reachedEOS() {
    return mReachedEOS;
}

// static
void *MPEG4Writer::Track::ThreadWrapper(void *me) {
    Track *track = static_cast<Track *>(me);

    track->threadEntry();

    return NULL;
}

#include <ctype.h>
static void hexdump(const void *_data, size_t size) {
    const uint8_t *data = (const uint8_t *)_data;
    size_t offset = 0;
    while (offset < size) {
        printf("0x%04x  ", offset);

        size_t n = size - offset;
        if (n > 16) {
            n = 16;
        }

        for (size_t i = 0; i < 16; ++i) {
            if (i == 8) {
                printf(" ");
            }

            if (offset + i < size) {
                printf("%02x ", data[offset + i]);
            } else {
                printf("   ");
            }
        }

        printf(" ");

        for (size_t i = 0; i < n; ++i) {
            if (isprint(data[offset + i])) {
                printf("%c", data[offset + i]);
            } else {
                printf(".");
            }
        }

        printf("\n");

        offset += 16;
    }
}


status_t MPEG4Writer::Track::makeAVCCodecSpecificData(
        const uint8_t *data, size_t size) {
    // hexdump(data, size);

    if (mCodecSpecificData != NULL) {
        LOGE("Already have codec specific data");
        return ERROR_MALFORMED;
    }

    if (size < 4 || memcmp("\x00\x00\x00\x01", data, 4)) {
        LOGE("Must start with a start code");
        return ERROR_MALFORMED;
    }

    size_t picParamOffset = 4;
    while (picParamOffset + 3 < size
            && memcmp("\x00\x00\x00\x01", &data[picParamOffset], 4)) {
        ++picParamOffset;
    }

    if (picParamOffset + 3 >= size) {
        LOGE("Could not find start-code for pictureParameterSet");
        return ERROR_MALFORMED;
    }

    size_t seqParamSetLength = picParamOffset - 4;
    size_t picParamSetLength = size - picParamOffset - 4;

    mCodecSpecificDataSize =
        6 + 1 + seqParamSetLength + 2 + picParamSetLength + 2;

    mCodecSpecificData = malloc(mCodecSpecificDataSize);
    uint8_t *header = (uint8_t *)mCodecSpecificData;
    header[0] = 1;
    header[1] = 0x42;  // profile
    header[2] = 0x80;
    header[3] = 0x1e;  // level

#if USE_NALLEN_FOUR
    header[4] = 0xfc | 3;  // length size == 4 bytes
#else
    header[4] = 0xfc | 1;  // length size == 2 bytes
#endif

    header[5] = 0xe0 | 1;
    header[6] = seqParamSetLength >> 8;
    header[7] = seqParamSetLength & 0xff;
    memcpy(&header[8], &data[4], seqParamSetLength);
    header += 8 + seqParamSetLength;
    header[0] = 1;
    header[1] = picParamSetLength >> 8;
    header[2] = picParamSetLength & 0xff;
    memcpy(&header[3], &data[picParamOffset + 4], picParamSetLength);

    return OK;
}

void MPEG4Writer::Track::threadEntry() {
    sp<MetaData> meta = mSource->getFormat();
    const char *mime;
    meta->findCString(kKeyMIMEType, &mime);
    bool is_mpeg4 = !strcasecmp(mime, MEDIA_MIMETYPE_VIDEO_MPEG4) ||
                    !strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_AAC);
    bool is_avc = !strcasecmp(mime, MEDIA_MIMETYPE_VIDEO_AVC);
    bool is_audio = !strncasecmp(mime, "audio/", 6);
    int32_t count = 0;
    const int64_t interleaveDurationUs = mOwner->interleaveDuration();
    int64_t chunkTimestampUs = 0;
    int32_t nChunks = 0;
    int32_t nZeroLengthFrames = 0;
    int64_t lastTimestamp = 0;  // Timestamp of the previous sample
    int64_t lastDuration = 0;   // Time spacing between the previous two samples
    int32_t sampleCount = 1;    // Sample count in the current stts table entry
    uint32_t previousSampleSize = 0;  // Size of the previous sample
    sp<MetaData> meta_data;

    MediaBuffer *buffer;
    while (!mDone && mSource->read(&buffer) == OK) {
        if (buffer->range_length() == 0) {
            buffer->release();
            buffer = NULL;
            ++nZeroLengthFrames;
            continue;
        }

        ++count;

        int32_t isCodecConfig;
        if (buffer->meta_data()->findInt32(kKeyIsCodecConfig, &isCodecConfig)
                && isCodecConfig) {
            CHECK(!mGotAllCodecSpecificData);

            if (is_avc) {
                status_t err = makeAVCCodecSpecificData(
                        (const uint8_t *)buffer->data()
                            + buffer->range_offset(),
                        buffer->range_length());
                CHECK_EQ(OK, err);
            } else if (is_mpeg4) {
                mCodecSpecificDataSize = buffer->range_length();
                mCodecSpecificData = malloc(mCodecSpecificDataSize);
                memcpy(mCodecSpecificData,
                        (const uint8_t *)buffer->data()
                            + buffer->range_offset(),
                       buffer->range_length());
            }

            buffer->release();
            buffer = NULL;

            mGotAllCodecSpecificData = true;
            continue;
        } else if (!mGotAllCodecSpecificData &&
                count == 1 && is_mpeg4 && mCodecSpecificData == NULL) {
            // The TI mpeg4 encoder does not properly set the
            // codec-specific-data flag.

            const uint8_t *data =
                (const uint8_t *)buffer->data() + buffer->range_offset();

            const size_t size = buffer->range_length();

            size_t offset = 0;
            while (offset + 3 < size) {
                if (data[offset] == 0x00 && data[offset + 1] == 0x00
                    && data[offset + 2] == 0x01 && data[offset + 3] == 0xb6) {
                    break;
                }

                ++offset;
            }

            // CHECK(offset + 3 < size);
            if (offset + 3 >= size) {
                // XXX assume the entire first chunk of data is the codec specific
                // data.
                offset = size;
            }

            mCodecSpecificDataSize = offset;
            mCodecSpecificData = malloc(offset);
            memcpy(mCodecSpecificData, data, offset);

            buffer->set_range(buffer->range_offset() + offset, size - offset);

            if (size == offset) {
                buffer->release();
                buffer = NULL;

                continue;
            }

            mGotAllCodecSpecificData = true;
        } else if (!mGotAllCodecSpecificData && is_avc && count < 3) {
            // The TI video encoder does not flag codec specific data
            // as such and also splits up SPS and PPS across two buffers.

            const uint8_t *data =
                (const uint8_t *)buffer->data() + buffer->range_offset();

            size_t size = buffer->range_length();

            CHECK(count == 2 || mCodecSpecificData == NULL);

            size_t offset = mCodecSpecificDataSize;
            mCodecSpecificDataSize += size + 4;
            mCodecSpecificData =
                realloc(mCodecSpecificData, mCodecSpecificDataSize);

            memcpy((uint8_t *)mCodecSpecificData + offset,
                   "\x00\x00\x00\x01", 4);

            memcpy((uint8_t *)mCodecSpecificData + offset + 4, data, size);

            buffer->release();
            buffer = NULL;

            if (count == 2) {
                void *tmp = mCodecSpecificData;
                size = mCodecSpecificDataSize;
                mCodecSpecificData = NULL;
                mCodecSpecificDataSize = 0;

                status_t err = makeAVCCodecSpecificData(
                        (const uint8_t *)tmp, size);
                free(tmp);
                tmp = NULL;
                CHECK_EQ(OK, err);

                mGotAllCodecSpecificData = true;
            }

            continue;
        }

        // Make a deep copy of the MediaBuffer and Metadata and release
        // the original as soon as we can
        MediaBuffer *copy = new MediaBuffer(buffer->range_length());
        memcpy(copy->data(), (uint8_t *)buffer->data() + buffer->range_offset(),
                buffer->range_length());
        copy->set_range(0, buffer->range_length());
        meta_data = new MetaData(*buffer->meta_data().get());
        buffer->release();
        buffer = NULL;

        if (is_avc) StripStartcode(copy);

        SampleInfo info;
        info.size = is_avc
#if USE_NALLEN_FOUR
                ? copy->range_length() + 4
#else
                ? copy->range_length() + 2
#endif
                : copy->range_length();

        // Max file size or duration handling
        mEstimatedTrackSizeBytes += info.size;
        if (mOwner->exceedsFileSizeLimit()) {
            mOwner->notify(MEDIA_RECORDER_EVENT_INFO, MEDIA_RECORDER_INFO_MAX_FILESIZE_REACHED, 0);
            break;
        }
        if (mOwner->exceedsFileDurationLimit()) {
            mOwner->notify(MEDIA_RECORDER_EVENT_INFO, MEDIA_RECORDER_INFO_MAX_DURATION_REACHED, 0);
            break;
        }


        int32_t isSync = false;
        meta_data->findInt32(kKeyIsSyncFrame, &isSync);

        int64_t timestampUs;
        CHECK(meta_data->findInt64(kKeyTime, &timestampUs));

////////////////////////////////////////////////////////////////////////////////
        if (mSampleInfos.empty()) {
            mOwner->setStartTimestamp(timestampUs);
            mStartTimestampUs = (timestampUs - mOwner->getStartTimestamp());
        }

        if (timestampUs > mMaxTimeStampUs) {
            mMaxTimeStampUs = timestampUs;
        }

        // Our timestamp is in ms.
        info.timestamp = (timestampUs + 500) / 1000;
        mSampleInfos.push_back(info);
        if (mSampleInfos.size() > 2) {
            if (lastDuration != info.timestamp - lastTimestamp) {
                SttsTableEntry sttsEntry(sampleCount, lastDuration);
                mSttsTableEntries.push_back(sttsEntry);
                sampleCount = 1;
            } else {
                ++sampleCount;
            }
        }
        if (mSamplesHaveSameSize) {
            if (mSampleInfos.size() >= 2 && previousSampleSize != info.size) {
                mSamplesHaveSameSize = false;
            }
            previousSampleSize = info.size;
        }
        lastDuration = info.timestamp - lastTimestamp;
        lastTimestamp = info.timestamp;

        if (isSync != 0) {
            mStssTableEntries.push_back(mSampleInfos.size());
        }


        mChunkSamples.push_back(copy);
        if (interleaveDurationUs == 0) {
            StscTableEntry stscEntry(++nChunks, 1, 1);
            mStscTableEntries.push_back(stscEntry);
            writeOneChunk(is_avc);
        } else {
            if (chunkTimestampUs == 0) {
                chunkTimestampUs = timestampUs;
            } else {
                if (timestampUs - chunkTimestampUs > interleaveDurationUs) {
                    ++nChunks;
                    if (nChunks == 1 ||  // First chunk
                        (--(mStscTableEntries.end()))->samplesPerChunk !=
                         mChunkSamples.size()) {
                        StscTableEntry stscEntry(nChunks,
                                mChunkSamples.size(), 1);
                        mStscTableEntries.push_back(stscEntry);
                    }
                    writeOneChunk(is_avc);
                    chunkTimestampUs = timestampUs;
                }
            }
        }

    }

    if (mSampleInfos.empty()) {
        mOwner->notify(MEDIA_RECORDER_EVENT_INFO, MEDIA_RECORDER_INFO_STOP_PREMATURELY, 0);
    }

    // Last chunk
    if (!mChunkSamples.empty()) {
        ++nChunks;
        StscTableEntry stscEntry(nChunks, mChunkSamples.size(), 1);
        mStscTableEntries.push_back(stscEntry);
        writeOneChunk(is_avc);
    }

    // We don't really know how long the last frame lasts, since
    // there is no frame time after it, just repeat the previous
    // frame's duration.
    if (mSampleInfos.size() == 1) {
        lastDuration = 0;  // A single sample's duration
    } else {
        ++sampleCount;  // Count for the last sample
    }
    SttsTableEntry sttsEntry(sampleCount, lastDuration);
    mSttsTableEntries.push_back(sttsEntry);
    mReachedEOS = true;
    LOGI("Received total/0-length (%d/%d) buffers and encoded %d frames - %s",
            count, nZeroLengthFrames, mSampleInfos.size(), is_audio? "audio": "video");
}

void MPEG4Writer::Track::writeOneChunk(bool isAvc) {
    mOwner->lock();
    for (List<MediaBuffer *>::iterator it = mChunkSamples.begin();
         it != mChunkSamples.end(); ++it) {
        off_t offset = isAvc? mOwner->addLengthPrefixedSample_l(*it)
                            : mOwner->addSample_l(*it);
        if (it == mChunkSamples.begin()) {
            mChunkOffsets.push_back(offset);
        }
    }
    mOwner->unlock();
    while (!mChunkSamples.empty()) {
        List<MediaBuffer *>::iterator it = mChunkSamples.begin();
        (*it)->release();
        (*it) = NULL;
        mChunkSamples.erase(it);
    }
    mChunkSamples.clear();
}

int64_t MPEG4Writer::Track::getDurationUs() const {
    return mMaxTimeStampUs;
}

int64_t MPEG4Writer::Track::getEstimatedTrackSizeBytes() const {
    return mEstimatedTrackSizeBytes;
}

void MPEG4Writer::Track::writeTrackHeader(int32_t trackID) {
    const char *mime;
    bool success = mMeta->findCString(kKeyMIMEType, &mime);
    CHECK(success);

    bool is_audio = !strncasecmp(mime, "audio/", 6);

    time_t now = time(NULL);

    mOwner->beginBox("trak");

      mOwner->beginBox("tkhd");
        mOwner->writeInt32(0);             // version=0, flags=0
        mOwner->writeInt32(now);           // creation time
        mOwner->writeInt32(now);           // modification time
        mOwner->writeInt32(trackID);
        mOwner->writeInt32(0);             // reserved
        mOwner->writeInt32(getDurationUs() / 1000);
        mOwner->writeInt32(0);             // reserved
        mOwner->writeInt32(0);             // reserved
        mOwner->writeInt16(0);             // layer
        mOwner->writeInt16(0);             // alternate group
        mOwner->writeInt16(is_audio ? 0x100 : 0);  // volume
        mOwner->writeInt16(0);             // reserved

        mOwner->writeInt32(0x10000);       // matrix
        mOwner->writeInt32(0);
        mOwner->writeInt32(0);
        mOwner->writeInt32(0);
        mOwner->writeInt32(0x10000);
        mOwner->writeInt32(0);
        mOwner->writeInt32(0);
        mOwner->writeInt32(0);
        mOwner->writeInt32(0x40000000);

        if (is_audio) {
            mOwner->writeInt32(0);
            mOwner->writeInt32(0);
        } else {
            int32_t width, height;
            bool success = mMeta->findInt32(kKeyWidth, &width);
            success = success && mMeta->findInt32(kKeyHeight, &height);
            CHECK(success);

            mOwner->writeInt32(width << 16);   // 32-bit fixed-point value
            mOwner->writeInt32(height << 16);  // 32-bit fixed-point value
        }
      mOwner->endBox();  // tkhd

      if (mStartTimestampUs != 0) {
        mOwner->beginBox("edts");
          mOwner->writeInt32(0);             // version=0, flags=0
          mOwner->beginBox("elst");
            mOwner->writeInt32(0);           // version=0, flags=0
            mOwner->writeInt32(1);           // a single entry
            mOwner->writeInt32(mStartTimestampUs / 1000);  // edit duration
            mOwner->writeInt32(-1);          // empty edit box to signal starting time offset
            mOwner->writeInt32(1);           // x1 rate
          mOwner->endBox();
        mOwner->endBox();
      }

      mOwner->beginBox("mdia");

        mOwner->beginBox("mdhd");
          mOwner->writeInt32(0);             // version=0, flags=0
          mOwner->writeInt32(now);           // creation time
          mOwner->writeInt32(now);           // modification time
          mOwner->writeInt32(1000);          // timescale
          mOwner->writeInt32(getDurationUs() / 1000);
          mOwner->writeInt16(0);             // language code XXX
          mOwner->writeInt16(0);             // predefined
        mOwner->endBox();

        mOwner->beginBox("hdlr");
          mOwner->writeInt32(0);             // version=0, flags=0
          mOwner->writeInt32(0);             // component type: should be mhlr
          mOwner->writeFourcc(is_audio ? "soun" : "vide");  // component subtype
          mOwner->writeInt32(0);             // reserved
          mOwner->writeInt32(0);             // reserved
          mOwner->writeInt32(0);             // reserved
          mOwner->writeCString(is_audio ? "SoundHandler": "");  // name
        mOwner->endBox();

        mOwner->beginBox("minf");
          if (is_audio) {
              mOwner->beginBox("smhd");
              mOwner->writeInt32(0);           // version=0, flags=0
              mOwner->writeInt16(0);           // balance
              mOwner->writeInt16(0);           // reserved
              mOwner->endBox();
          } else {
              mOwner->beginBox("vmhd");
              mOwner->writeInt32(0x00000001);  // version=0, flags=1
              mOwner->writeInt16(0);           // graphics mode
              mOwner->writeInt16(0);           // opcolor
              mOwner->writeInt16(0);
              mOwner->writeInt16(0);
              mOwner->endBox();
          }

          mOwner->beginBox("dinf");
            mOwner->beginBox("dref");
              mOwner->writeInt32(0);  // version=0, flags=0
              mOwner->writeInt32(1);
              mOwner->beginBox("url ");
                mOwner->writeInt32(1);  // version=0, flags=1
              mOwner->endBox();  // url
            mOwner->endBox();  // dref
          mOwner->endBox();  // dinf

       mOwner->endBox();  // minf

        mOwner->beginBox("stbl");

          mOwner->beginBox("stsd");
            mOwner->writeInt32(0);               // version=0, flags=0
            mOwner->writeInt32(1);               // entry count
            if (is_audio) {
                const char *fourcc = NULL;
                if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_AMR_NB, mime)) {
                    fourcc = "samr";
                } else if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_AMR_WB, mime)) {
                    fourcc = "sawb";
                } else if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_AAC, mime)) {
                    fourcc = "mp4a";
                } else {
                    LOGE("Unknown mime type '%s'.", mime);
                    CHECK(!"should not be here, unknown mime type.");
                }

                mOwner->beginBox(fourcc);          // audio format
                  mOwner->writeInt32(0);           // reserved
                  mOwner->writeInt16(0);           // reserved
                  mOwner->writeInt16(0x1);         // data ref index
                  mOwner->writeInt32(0);           // reserved
                  mOwner->writeInt32(0);           // reserved
                  int32_t nChannels;
                  CHECK_EQ(true, mMeta->findInt32(kKeyChannelCount, &nChannels));
                  mOwner->writeInt16(nChannels);   // channel count
                  mOwner->writeInt16(16);          // sample size
                  mOwner->writeInt16(0);           // predefined
                  mOwner->writeInt16(0);           // reserved

                  int32_t samplerate;
                  bool success = mMeta->findInt32(kKeySampleRate, &samplerate);
                  CHECK(success);

                  mOwner->writeInt32(samplerate << 16);
                  if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_AAC, mime)) {
                    mOwner->beginBox("esds");

                        mOwner->writeInt32(0);     // version=0, flags=0
                        mOwner->writeInt8(0x03);   // ES_DescrTag
                        mOwner->writeInt8(23 + mCodecSpecificDataSize);
                        mOwner->writeInt16(0x0000);// ES_ID
                        mOwner->writeInt8(0x00);

                        mOwner->writeInt8(0x04);   // DecoderConfigDescrTag
                        mOwner->writeInt8(15 + mCodecSpecificDataSize);
                        mOwner->writeInt8(0x40);   // objectTypeIndication ISO/IEC 14492-2
                        mOwner->writeInt8(0x15);   // streamType AudioStream

                        mOwner->writeInt16(0x03);  // XXX
                        mOwner->writeInt8(0x00);   // buffer size 24-bit
                        mOwner->writeInt32(96000); // max bit rate
                        mOwner->writeInt32(96000); // avg bit rate

                        mOwner->writeInt8(0x05);   // DecoderSpecificInfoTag
                        mOwner->writeInt8(mCodecSpecificDataSize);
                        mOwner->write(mCodecSpecificData, mCodecSpecificDataSize);

                        static const uint8_t kData2[] = {
                            0x06,  // SLConfigDescriptorTag
                            0x01,
                            0x02
                        };
                        mOwner->write(kData2, sizeof(kData2));

                    mOwner->endBox();  // esds
                  }
                mOwner->endBox();
            } else {
                if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_MPEG4, mime)) {
                    mOwner->beginBox("mp4v");
                } else if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_H263, mime)) {
                    mOwner->beginBox("s263");
                } else if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_AVC, mime)) {
                    mOwner->beginBox("avc1");
                } else {
                    LOGE("Unknown mime type '%s'.", mime);
                    CHECK(!"should not be here, unknown mime type.");
                }

                  mOwner->writeInt32(0);           // reserved
                  mOwner->writeInt16(0);           // reserved
                  mOwner->writeInt16(0);           // data ref index
                  mOwner->writeInt16(0);           // predefined
                  mOwner->writeInt16(0);           // reserved
                  mOwner->writeInt32(0);           // predefined
                  mOwner->writeInt32(0);           // predefined
                  mOwner->writeInt32(0);           // predefined

                  int32_t width, height;
                  bool success = mMeta->findInt32(kKeyWidth, &width);
                  success = success && mMeta->findInt32(kKeyHeight, &height);
                  CHECK(success);

                  mOwner->writeInt16(width);
                  mOwner->writeInt16(height);
                  mOwner->writeInt32(0x480000);    // horiz resolution
                  mOwner->writeInt32(0x480000);    // vert resolution
                  mOwner->writeInt32(0);           // reserved
                  mOwner->writeInt16(1);           // frame count
                  mOwner->write("                                ", 32);
                  mOwner->writeInt16(0x18);        // depth
                  mOwner->writeInt16(-1);          // predefined

                  CHECK(23 + mCodecSpecificDataSize < 128);

                  if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_MPEG4, mime)) {
                      mOwner->beginBox("esds");

                        mOwner->writeInt32(0);           // version=0, flags=0

                        mOwner->writeInt8(0x03);  // ES_DescrTag
                        mOwner->writeInt8(23 + mCodecSpecificDataSize);
                        mOwner->writeInt16(0x0000);  // ES_ID
                        mOwner->writeInt8(0x1f);

                        mOwner->writeInt8(0x04);  // DecoderConfigDescrTag
                        mOwner->writeInt8(15 + mCodecSpecificDataSize);
                        mOwner->writeInt8(0x20);  // objectTypeIndication ISO/IEC 14492-2
                        mOwner->writeInt8(0x11);  // streamType VisualStream

                        static const uint8_t kData[] = {
                            0x01, 0x77, 0x00,
                            0x00, 0x03, 0xe8, 0x00,
                            0x00, 0x03, 0xe8, 0x00
                        };
                        mOwner->write(kData, sizeof(kData));

                        mOwner->writeInt8(0x05);  // DecoderSpecificInfoTag

                        mOwner->writeInt8(mCodecSpecificDataSize);
                        mOwner->write(mCodecSpecificData, mCodecSpecificDataSize);

                        static const uint8_t kData2[] = {
                            0x06,  // SLConfigDescriptorTag
                            0x01,
                            0x02
                        };
                        mOwner->write(kData2, sizeof(kData2));

                      mOwner->endBox();  // esds
                  } else if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_H263, mime)) {
                      mOwner->beginBox("d263");

                          mOwner->writeInt32(0);  // vendor
                          mOwner->writeInt8(0);   // decoder version
                          mOwner->writeInt8(10);  // level: 10
                          mOwner->writeInt8(0);   // profile: 0

                      mOwner->endBox();  // d263
                  } else if (!strcasecmp(MEDIA_MIMETYPE_VIDEO_AVC, mime)) {
                      mOwner->beginBox("avcC");
                        mOwner->write(mCodecSpecificData, mCodecSpecificDataSize);
                      mOwner->endBox();  // avcC
                  }

                mOwner->endBox();  // mp4v, s263 or avc1
            }
          mOwner->endBox();  // stsd

          mOwner->beginBox("stts");
            mOwner->writeInt32(0);  // version=0, flags=0
            mOwner->writeInt32(mSttsTableEntries.size());
            for (List<SttsTableEntry>::iterator it = mSttsTableEntries.begin();
                 it != mSttsTableEntries.end(); ++it) {
                mOwner->writeInt32(it->sampleCount);
                mOwner->writeInt32(it->sampleDuration);
            }
          mOwner->endBox();  // stts

          if (!is_audio) {
            mOwner->beginBox("stss");
              mOwner->writeInt32(0);  // version=0, flags=0
              mOwner->writeInt32(mStssTableEntries.size());  // number of sync frames
              for (List<int32_t>::iterator it = mStssTableEntries.begin();
                   it != mStssTableEntries.end(); ++it) {
                  mOwner->writeInt32(*it);
              }
            mOwner->endBox();  // stss
          }

          mOwner->beginBox("stsz");
            mOwner->writeInt32(0);  // version=0, flags=0
            if (mSamplesHaveSameSize) {
                List<SampleInfo>::iterator it = mSampleInfos.begin();
                mOwner->writeInt32(it->size);  // default sample size
            } else {
                mOwner->writeInt32(0);
            }
            mOwner->writeInt32(mSampleInfos.size());
            if (!mSamplesHaveSameSize) {
                for (List<SampleInfo>::iterator it = mSampleInfos.begin();
                     it != mSampleInfos.end(); ++it) {
                    mOwner->writeInt32((*it).size);
                }
            }
          mOwner->endBox();  // stsz

          mOwner->beginBox("stsc");
            mOwner->writeInt32(0);  // version=0, flags=0
            mOwner->writeInt32(mStscTableEntries.size());
            for (List<StscTableEntry>::iterator it = mStscTableEntries.begin();
                 it != mStscTableEntries.end(); ++it) {
                mOwner->writeInt32(it->firstChunk);
                mOwner->writeInt32(it->samplesPerChunk);
                mOwner->writeInt32(it->sampleDescriptionId);
            }
          mOwner->endBox();  // stsc

          mOwner->beginBox("co64");
            mOwner->writeInt32(0);  // version=0, flags=0
            mOwner->writeInt32(mChunkOffsets.size());
            for (List<off_t>::iterator it = mChunkOffsets.begin();
                 it != mChunkOffsets.end(); ++it) {
                mOwner->writeInt64((*it));
            }
          mOwner->endBox();  // co64

        mOwner->endBox();  // stbl
      mOwner->endBox();  // mdia
    mOwner->endBox();  // trak
}

}  // namespace android
