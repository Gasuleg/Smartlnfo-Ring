/*
 *  Copyright (C) 2012-2015 Savoir-Faire Linux Inc.
 *  Author: Tristan Matthews <tristan.matthews@savoirfairelinux.com>
 *  Author: Guillaume Roguez <guillaume.roguez@savoirfairelinux.com>
 *
 *  Portions derived from GStreamer:
 *  Copyright (C) <2009> Collabora Ltd
 *  @author: Olivier Crete <olivier.crete@collabora.co.uk
 *  Copyright (C) <2009> Nokia Inc
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA.
 *
 *  Additional permission under GNU GPL version 3 section 7:
 *
 *  If you modify this program, or any covered work, by linking or
 *  combining it with the OpenSSL project's OpenSSL library (or a
 *  modified version of that library), containing parts covered by the
 *  terms of the OpenSSL or SSLeay licenses, Savoir-Faire Linux Inc.
 *  grants you additional permission to convey the resulting work.
 *  Corresponding Source for a non-source form of such a combination
 *  shall include the source code for the parts of OpenSSL used as well
 *  as that of the covered work.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "sinkclient.h"

#if HAVE_SHM
#include "shm_header.h"
#endif // HAVE_SHM

#include "video_scaler.h"
#include "media_buffer.h"
#include "logger.h"
#include "noncopyable.h"
#include "client/ring_signal.h"
#include "dring/videomanager_interface.h"

#include <sys/mman.h>
#include <fcntl.h>
#include <cstdio>
#include <sstream>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace ring { namespace video {

#if HAVE_SHM
// RAII class helper on sem_wait/sem_post sempahore operations
class SemGuardLock {
    public:
        explicit SemGuardLock(sem_t& mutex) : m_(mutex) {
            auto ret = ::sem_wait(&m_);
            if (ret < 0) {
                std::ostringstream msg;
                msg << "SHM mutex@" << &m_
                    << " lock failed (" << ret << ")";
                throw std::logic_error {msg.str()};
            }
        }

        ~SemGuardLock() {
            ::sem_post(&m_);
        }

    private:
        sem_t& m_;
};

class ShmHolder
{
    public:
        ShmHolder(const std::string& name={});
        ~ShmHolder();

        std::string name() const noexcept {
            return openedName_;
        }

        void renderFrame(VideoFrame& src) noexcept;

    private:
        bool resizeArea(std::size_t desired_length) noexcept;
        char* getShmAreaDataPtr() noexcept;

        void unMapShmArea() noexcept {
            if (area_ != MAP_FAILED and ::munmap(area_, areaSize_) < 0) {
                RING_ERR("ShmHolder[%s]: munmap(%u) failed with errno %d",
                         openedName_.c_str(), areaSize_, errno);
            }
        }

        SHMHeader* area_ {static_cast<SHMHeader*>(MAP_FAILED)};
        std::size_t areaSize_ {0};
        std::string openedName_;
        int fd_ {-1};
};

ShmHolder::ShmHolder(const std::string& name)
{
    static constexpr int flags = O_RDWR | O_CREAT | O_TRUNC | O_EXCL;
    static constexpr int perms = S_IRUSR | S_IWUSR;

    static auto shmFailedWithErrno = [this](const std::string& what) {
        std::ostringstream msg;
        msg << "ShmHolder[" << openedName_ << "]: "
        << what << " failed, errno=" << errno;
        throw std::runtime_error {msg.str()};
    };

    if (not name.empty()) {
        openedName_ = name;
        fd_ = ::shm_open(openedName_.c_str(), flags, perms);
        if (fd_ < 0)
            shmFailedWithErrno("shm_open");
    } else {
        for (int i = 0; fd_ < 0; ++i) {
            std::ostringstream tmpName;
            tmpName << PACKAGE_NAME << "_shm_" << getpid() << "_" << i;
            openedName_ = tmpName.str();
            fd_ = ::shm_open(openedName_.c_str(), flags, perms);
            if (fd_ < 0 and errno != EEXIST)
                shmFailedWithErrno("shm_open");
        }
    }

    // Set size enough for header only (no frame data)
    if (!resizeArea(0))
        shmFailedWithErrno("resizeArea");

    // Header fields initialization
    std::memset(area_, 0, areaSize_);

    if (::sem_init(&area_->mutex, 1, 1) < 0)
        shmFailedWithErrno("sem_init(mutex)");

    if (::sem_init(&area_->frameGenMutex, 1, 0) < 0)
        shmFailedWithErrno("sem_init(frameGenMutex)");

    RING_DBG("ShmHolder: new holder '%s'", openedName_.c_str());
}

ShmHolder::~ShmHolder()
{
    if (fd_ < 0)
        return;

    ::close(fd_);
    ::shm_unlink(openedName_.c_str());

    if (area_ == MAP_FAILED)
        return;

    area_->frameSize = 0;
    ::sem_post(&area_->frameGenMutex);
    unMapShmArea();
}

bool
ShmHolder::resizeArea(std::size_t frameSize) noexcept
{
    // aligned on 16-byte boundary frameSize
    frameSize = (frameSize + 15) & ~15;

    if (area_ != MAP_FAILED and frameSize == area_->frameSize)
        return true;

    // full area size: +15 to take care of maximum padding size
    const auto areaSize = sizeof(SHMHeader) + 2 * frameSize + 15;
    RING_DBG("ShmHolder[%s]: new sizes: f=%u, a=%u", openedName_.c_str(),
             frameSize, areaSize);

    unMapShmArea();

    if (::ftruncate(fd_, areaSize) < 0) {
        RING_ERR("ShmHolder[%s]: ftruncate(%u) failed with errno %d",
                 openedName_.c_str(), areaSize, errno);
        return false;
    }

    area_ = static_cast<SHMHeader*>(::mmap(nullptr, areaSize,
                                           PROT_READ | PROT_WRITE,
                                           MAP_SHARED, fd_, 0));

    if (area_ == MAP_FAILED) {
        areaSize_ = 0;
        RING_ERR("ShmHolder[%s]: mmap(%u) failed with errno %d",
                 openedName_.c_str(), areaSize, errno);
        return false;
    }

    areaSize_ = areaSize;

    if (frameSize) {
        SemGuardLock lk {area_->mutex};

        area_->frameSize = frameSize;
        area_->mapSize = areaSize;

        // Compute aligned IO pointers
        // Note: we not using std::align as not implemented in 4.9
        // https://gcc.gnu.org/bugzilla/show_bug.cgi?id=57350
        auto p = reinterpret_cast<std::uintptr_t>(area_->data);
        area_->writeOffset = ((p + 15) & ~15) - p;
        area_->readOffset = area_->writeOffset + frameSize;
    }

    return true;
}

void
ShmHolder::renderFrame(VideoFrame& src) noexcept
{
    const auto width = src.width();
    const auto height = src.height();
    const auto format = VIDEO_PIXFMT_BGRA;
    const auto frameSize = videoFrameSize(format, width, height);

    if (!resizeArea(frameSize)) {
        RING_ERR("ShmHolder[%s]: could not resize area",
                 openedName_.c_str());
        return;
    }

    {
        VideoFrame dst;
        VideoScaler scaler;

        dst.setFromMemory(area_->data + area_->writeOffset, format, width, height);
        scaler.scale(src, dst);
    }

    {
        SemGuardLock lk {area_->mutex};

        ++area_->frameGen;
        std::swap(area_->readOffset, area_->writeOffset);
        ::sem_post(&area_->frameGenMutex);
    }
}

std::string
SinkClient::openedName() const noexcept
{
    return shm_->name();
}

bool
SinkClient::start() noexcept
{
    if (not shm_) {
        try {
            shm_ = std::make_shared<ShmHolder>();
        } catch (const std::runtime_error& e) {
            RING_ERR("SHMHolder ctor failure: %s", e.what());
        }
    }
    return static_cast<bool>(shm_);
}

bool
SinkClient::stop() noexcept
{
    shm_.reset();
    return true;
}

#else // HAVE_SHM

std::string
SinkClient::openedName() const noexcept
{
    return {};
}

bool
SinkClient::start() noexcept
{
    return true;
}

bool
SinkClient::stop() noexcept
{
    return true;
}

#endif // !HAVE_SHM

SinkClient::SinkClient(const std::string& id) : id_ {id}
#ifdef DEBUG_FPS
    , frameCount_(0u)
    , lastFrameDebug_(std::chrono::system_clock::now())
#endif
{}

void
SinkClient::update(Observable<std::shared_ptr<VideoFrame>>* /*obs*/,
                   std::shared_ptr<VideoFrame>& frame_p)
{
    auto f = frame_p; // keep a local reference during rendering

#ifdef DEBUG_FPS
    auto currentTime = std::chrono::system_clock::now();
    const std::chrono::duration<double> seconds = currentTime - lastFrameDebug_;
    ++frameCount_;
    if (seconds.count() > 1) {
        RING_DBG("%s: FPS %f", id_.c_str(), frameCount_ / seconds.count());
        frameCount_ = 0;
        lastFrameDebug_ = currentTime;
    }
#endif

#if HAVE_SHM
    shm_->renderFrame(*f.get());
#endif

    if (target_) {
        VideoFrame dst;
        VideoScaler scaler;
        const int width = f->width();
        const int height = f->height();
        const int format = VIDEO_PIXFMT_RGBA;
        const auto bytes = videoFrameSize(format, width, height);

        targetData_.resize(bytes);
        auto data = targetData_.data();

        dst.setFromMemory(data, format, width, height);
        scaler.scale(*f, dst);
        target_(data);
    }
}

}} // namespace ring::video