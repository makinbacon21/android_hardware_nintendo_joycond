#include <iostream>
#include <utils/Log.h>

#include "ctlr_detector.h"
#include "ctlr_mgr.h"
#include "epoll_mgr.h"

#include "Joycond.h"

namespace aidl::android::hardware::nintendo::joycond {

using ::ndk::ScopedAStatus;

Joycond::Joycond() {
    // TODO: this will be prop-based for preservation
    // mMapping.layout = nullptr;
    mMapping.combined = true;
    mMapping.analog = false;

    ready.store(true);
    if (pthread_mutex_init(&mapLock, NULL)) {
        ALOGE("pthread_mutex_init failed!");
        return;
    }

    if (pthread_create(&pollThread, NULL, __threadLoop, this)) {
        ALOGE("pthread_create failed!");
        pthread_mutex_destroy(&mapLock);
        return;
    }

    pthread_setname_np(pollThread, "joycond_poll_thread");
}

Joycond::~Joycond() {
    ready.store(false);
    pthread_join(pollThread, NULL);
    pthread_mutex_destroy(&mapLock);
}

::ndk::ScopedAStatus Joycond::restartService() {
    int ret;

    ready.store(false);
    ret = pthread_join(pollThread, NULL);
    if (ret) {
        return ScopedAStatus::fromServiceSpecificError(ret);
    }

    ready.store(true);
    ret = pthread_create(&pollThread, NULL, __threadLoop, this);
    if (ret) {
        return ScopedAStatus::fromServiceSpecificError(ret);
    }

    return ScopedAStatus::ok();
}

::ndk::ScopedAStatus Joycond::setLayout(const std::vector<KeyMap> &layout) {
    pthread_mutex_lock(&mapLock);
    for (auto &entry : layout) {
        ALOGI("Mapping %d to %d", entry.from, entry.to);
        mMapping.layout[entry.from] = entry.to;
    }
    pthread_mutex_unlock(&mapLock);
    return ScopedAStatus::ok();
}

::ndk::ScopedAStatus Joycond::getLayout(std::vector<KeyMap> *_aidl_return) {
    pthread_mutex_lock(&mapLock);
    for (auto &entry : mMapping.layout) {
        KeyMap k;
        k.from = entry.first;
        k.to = entry.second;
        _aidl_return->push_back(k);
    }
    pthread_mutex_unlock(&mapLock);
    return ScopedAStatus::ok();
}

::ndk::ScopedAStatus Joycond::setCombined(bool combined) {
    pthread_mutex_lock(&mapLock);
    mMapping.combined = combined;
    pthread_mutex_unlock(&mapLock);

    return ScopedAStatus::ok();
}

::ndk::ScopedAStatus Joycond::getCombined(bool *_aidl_return) {
    pthread_mutex_lock(&mapLock);
    *_aidl_return = mMapping.combined;
    pthread_mutex_unlock(&mapLock);

    return ScopedAStatus::ok();
}

::ndk::ScopedAStatus Joycond::setAnalog(bool analog) {
    pthread_mutex_lock(&mapLock);
    mMapping.analog = analog;
    pthread_mutex_unlock(&mapLock);

    return ScopedAStatus::ok();
}

::ndk::ScopedAStatus Joycond::getAnalog(bool *_aidl_return) {
    pthread_mutex_lock(&mapLock);
    *_aidl_return = mMapping.analog;
    pthread_mutex_unlock(&mapLock);

    return ScopedAStatus::ok();
}

void *Joycond::__threadLoop(void *args) {
    Joycond *const self = static_cast<Joycond *>(args);

    epoll_mgr epoll_manager;
    ctlr_mgr ctlr_manager(epoll_manager, &(self->mMapping), &(self->mapLock));
    ctlr_detector ctlr_detector(ctlr_manager, epoll_manager);

    while (self->ready.load()) {
        epoll_manager.loop();
    }

    return NULL;
}

} // namespace aidl::android::hardware::nintendo::joycond
