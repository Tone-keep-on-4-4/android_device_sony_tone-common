/*
 * Copyright (C) 2018 Shane Francis / Jens Andersen
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

#define LOG_TAG "AOSP FPC HAL (Binder)"
#define LOG_VERBOSE "AOSP FPC HAL (Binder)"

#include <hardware/hw_auth_token.h>

#include <hardware/hardware.h>
#include <hardware/fingerprint.h>
#include "BiometricsFingerprint.h"

#include <inttypes.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>
#include <byteswap.h>

#include "android-base/macros.h"

namespace android {
namespace hardware {
namespace biometrics {
namespace fingerprint {
namespace V2_1 {
namespace implementation {

using RequestStatus =
        android::hardware::biometrics::fingerprint::V2_1::RequestStatus;

BiometricsFingerprint *BiometricsFingerprint::sInstance = nullptr;

BiometricsFingerprint::BiometricsFingerprint() : mClientCallback(nullptr), mDevice(nullptr) {
    mDevice = openHal();
    if (!mDevice) {
        ALOGE("Can't open HAL module");
        return;
    }
    sInstance = this; // keep track of the most recent instance
}

BiometricsFingerprint::~BiometricsFingerprint() {
    ALOGV("~BiometricsFingerprint()");
    if (mDevice == nullptr) {
        ALOGE("No valid device");
        return;
    }
    mDevice = nullptr;
}

Return<RequestStatus> BiometricsFingerprint::ErrorFilter(int32_t error) {
    switch(error) {
        case 0: return RequestStatus::SYS_OK;
        case -2: return RequestStatus::SYS_ENOENT;
        case -4: return RequestStatus::SYS_EINTR;
        case -5: return RequestStatus::SYS_EIO;
        case -11: return RequestStatus::SYS_EAGAIN;
        case -12: return RequestStatus::SYS_ENOMEM;
        case -13: return RequestStatus::SYS_EACCES;
        case -14: return RequestStatus::SYS_EFAULT;
        case -16: return RequestStatus::SYS_EBUSY;
        case -22: return RequestStatus::SYS_EINVAL;
        case -28: return RequestStatus::SYS_ENOSPC;
        case -110: return RequestStatus::SYS_ETIMEDOUT;
        default:
            ALOGE("An unknown error returned from fingerprint vendor library: %d", error);
            return RequestStatus::SYS_UNKNOWN;
    }
}

Return<uint64_t> BiometricsFingerprint::setNotify(
        const sp<IBiometricsFingerprintClientCallback>& clientCallback) {
    std::lock_guard<std::mutex> lock(mClientCallbackMutex);
    mClientCallback = clientCallback;
    // This is here because HAL 2.1 doesn't have a way to propagate a
    // unique token for its driver. Subsequent versions should send a unique
    // token for each call to setNotify(). This is fine as long as there's only
    // one fingerprint device on the platform.
    return reinterpret_cast<uint64_t>(mDevice);
}

Return<uint64_t> BiometricsFingerprint::preEnroll()  {
    sony_fingerprint_device_t *sdev = mDevice;
    sdev->challenge = fpc_load_auth_challenge(sdev->fpc);
    ALOGI("%s : Challenge is : %ju",__func__, sdev->challenge);
    return sdev->challenge;
}

Return<RequestStatus> BiometricsFingerprint::enroll(const hidl_array<uint8_t, 69>& hat,
        uint32_t gid ATTRIBUTE_UNUSED,
        uint32_t timeoutSec ATTRIBUTE_UNUSED) {
    const hw_auth_token_t* authToken =
        reinterpret_cast<const hw_auth_token_t*>(hat.data());

    sony_fingerprint_device_t *sdev = mDevice;


    ALOGI("%s : hat->challenge %lu",__func__,(unsigned long) authToken->challenge);
    ALOGI("%s : hat->user_id %lu",__func__,(unsigned long) authToken->user_id);
    ALOGI("%s : hat->authenticator_id %lu",__func__,(unsigned long) authToken->authenticator_id);
    ALOGI("%s : hat->authenticator_type %d",__func__,authToken->authenticator_type);
    ALOGI("%s : hat->timestamp %lu",__func__,(unsigned long) authToken->timestamp);
    ALOGI("%s : hat size %lu",__func__,(unsigned long) sizeof(hw_auth_token_t));

    fpc_verify_auth_challenge(sdev->fpc, (void*) authToken, sizeof(hw_auth_token_t));

    bool success = setState(sdev, STATE_ENROLL);
    return success ? RequestStatus::SYS_OK : RequestStatus::SYS_EAGAIN;
}

Return<RequestStatus> BiometricsFingerprint::postEnroll() {

    sony_fingerprint_device_t *sdev = mDevice;
    ALOGI("%s: Resetting challenge", __func__);
    sdev->challenge = 0;
    return ErrorFilter(0);
}

Return<uint64_t> BiometricsFingerprint::getAuthenticatorId() {
    sony_fingerprint_device_t *sdev = mDevice;
    uint64_t id = fpc_load_db_id(sdev->fpc);
    ALOGI("%s : ID : %ju",__func__,id );
    return id;
}

Return<RequestStatus> BiometricsFingerprint::cancel() {

    ALOGI("%s",__func__);

    sony_fingerprint_device_t *sdev = mDevice;

    if (setState(sdev, STATE_CANCEL)) {
        // NOTE: In it's current form, setState will never fail for CANCEL.
        ALOGI("%s : Successfully moved to cancel state", __func__);
        return RequestStatus::SYS_OK;
    }

    ALOGE("%s : Failed to move to cancel state", __func__);
    return RequestStatus::SYS_UNKNOWN;
}

Return<RequestStatus> BiometricsFingerprint::enumerate()  {

    const uint64_t devId = reinterpret_cast<uint64_t>(mDevice);
    if (mClientCallback == nullptr) {
        ALOGE("Client callback not set");
        return RequestStatus::SYS_EFAULT;
    }

    ALOGV(__func__);
    sony_fingerprint_device_t *sdev = mDevice;

    fpc_fingerprint_index_t print_indexs = fpc_get_print_index(sdev->fpc);

    if (!print_indexs.print_count)
        // When there are no fingers, the service still needs to know that (potentially async)
        // enumeration has finished. By convention, send fid=0 and remaining=0 to signal this:
        mClientCallback->onEnumerate(devId, 0, mDevice->gid, 0);
    else
        for (size_t i = 0; i < print_indexs.print_count; i++) {
            ALOGD("%s : found print : %lu at index %zu", __func__, (unsigned long) print_indexs.prints[i], i);

            uint32_t  remaining_templates = (uint32_t)(print_indexs.print_count - i - 1);

            mClientCallback->onEnumerate(devId, print_indexs.prints[i], mDevice->gid, remaining_templates);
        }

    return ErrorFilter(0);
}

Return<RequestStatus> BiometricsFingerprint::remove(uint32_t gid, uint32_t fid) {

    const uint64_t devId = reinterpret_cast<uint64_t>(mDevice);

    sony_fingerprint_device_t *sdev = mDevice;

    if (mClientCallback == nullptr) {
        ALOGE("Client callback not set");
        return ErrorFilter(-1);
    }

    if (fpc_del_print_id(sdev->fpc, fid) == 0){

        mClientCallback->onRemoved(devId, fid, gid,0);

        uint32_t db_length = fpc_get_user_db_length(sdev->fpc);
        ALOGD("%s : User Database Length Is : %lu", __func__,(unsigned long) db_length);
        fpc_store_user_db(sdev->fpc, db_length, sdev->db_path);
        return ErrorFilter(0);
    } else {
        mClientCallback->onError(devId, FingerprintError::ERROR_UNABLE_TO_REMOVE, -1);
        return ErrorFilter(-1);
    }
}

int BiometricsFingerprint::__setActiveGroup(sony_fingerprint_device_t *sdev,
        uint32_t gid) {
    int result;
    bool created_empty_db = false;
    struct stat sb;

    if(stat(sdev->db_path, &sb) == -1) {
        // No existing database, load an empty one
        if ((result = fpc_load_empty_db(sdev->fpc)) != 0) {
            ALOGE("Error creating empty user database: %d\n", result);
            return result;
        }
        created_empty_db = true;
    } else {
        if ((result = fpc_load_user_db(sdev->fpc, sdev->db_path)) != 0) {
            ALOGE("Error loading existing user database: %d\n", result);
            return result;
        }
    }

    if((result = fpc_set_gid(sdev->fpc, gid)) != 0)
    {
        ALOGE("Error setting current gid: %d\n", result);
    }

    // if user database was created in this instance, store it directly
    if(created_empty_db)
    {
        int length  = fpc_get_user_db_length(sdev->fpc);
        fpc_store_user_db(sdev->fpc, length, sdev->db_path);
        if ((result = fpc_load_user_db(sdev->fpc, sdev->db_path)) != 0) {
            ALOGE("Error loading empty user database: %d\n", result);
            return result;
        }
    }
    return result;
}

Return<RequestStatus> BiometricsFingerprint::setActiveGroup(uint32_t gid,
        const hidl_string& storePath) {

    int result;
    sony_fingerprint_device_t *sdev = mDevice;
    if (storePath.size() >= PATH_MAX || storePath.size() <= 0) {
        ALOGE("Bad path length: %zd", storePath.size());
        return RequestStatus::SYS_EINVAL;
    }
    if (access(storePath.c_str(), W_OK)) {
        return RequestStatus::SYS_EINVAL;
    }

    sprintf(sdev->db_path,"%s/user.db", storePath.c_str());
    sdev->gid = gid;

    ALOGI("%s : storage path set to : %s", __func__, sdev->db_path);
    result = __setActiveGroup(sdev, gid);

    return ErrorFilter(result);
}

Return<RequestStatus> BiometricsFingerprint::authenticate(uint64_t operation_id,
        uint32_t gid ATTRIBUTE_UNUSED) {

    err_t r;
    sony_fingerprint_device_t *sdev = mDevice;

    ALOGI("%s: operation_id=%ju", __func__, operation_id);
    r = fpc_set_auth_challenge(sdev->fpc, operation_id);
    if (r < 0) {
        ALOGE("%s: Error setting auth challenge to %ju. r=0x%08X",__func__, operation_id, r);
        return RequestStatus::SYS_EAGAIN;
    }

    bool success = setState(sdev, STATE_AUTH);
    return success ? RequestStatus::SYS_OK : RequestStatus::SYS_EAGAIN;
}

IBiometricsFingerprint* BiometricsFingerprint::getInstance() {
    if (!sInstance) {
        // The constructor will set sInstance.
        /*sInstance =*/ new BiometricsFingerprint();
    }
    return sInstance;
}

sony_fingerprint_device_t* BiometricsFingerprint::openHal() {
    ALOGI("%s",__func__);

    fpc_imp_data_t *fpc_data = NULL;

    sony_fingerprint_device_t *sdev = (sony_fingerprint_device_t*) malloc(sizeof(sony_fingerprint_device_t));
    memset(sdev, 0, sizeof(sony_fingerprint_device_t));

    sdev->worker.event_fd = eventfd(0, EFD_NONBLOCK);

    if (fpc_init(&fpc_data, sdev->worker.event_fd) < 0) {
        ALOGE("Could not init FPC device");
        return nullptr;
    }
    sdev->fpc = fpc_data;

    sdev->worker.epoll_fd = epoll_create1(0);
    struct epoll_event evnt = {
        .data.fd = sdev->worker.event_fd,
        .events = EPOLLIN | EPOLLET,
    };

    epoll_ctl(sdev->worker.epoll_fd, EPOLL_CTL_ADD, sdev->worker.event_fd, &evnt);

    sdev->worker.running_state = STATE_IDLE;

    if(pthread_create(&sdev->worker.thread, NULL, worker_thread, (void*)sdev)) {
        ALOGE("%s : Error creating worker thread\n", __func__);
        sdev->worker.thread_running  = false;
        return nullptr;
    }

    return sdev;
}

enum worker_state BiometricsFingerprint::getNextState(sony_fingerprint_device_t* sdev) {
    eventfd_t requestedState;
    enum worker_state state = STATE_IDLE;

    int rc = eventfd_read(sdev->worker.event_fd, &requestedState);
    if (!rc)
        state = (enum worker_state)requestedState;

    ALOGV("%s : %d", __func__, state);
    return state;
}

bool BiometricsFingerprint::isCanceled(sony_fingerprint_device_t *sdev) {
    enum worker_state state = getNextState(sdev);

    if (state == STATE_CANCEL)
    {
        ALOGI("%s : Operation canceled", __func__);
        return true;
    }

    if (state != STATE_IDLE)
        ALOGW("%s : Unexpected state %d", __func__, state);

    return false;
}

bool BiometricsFingerprint::setState(sony_fingerprint_device_t* sdev, enum worker_state state) {
    enum worker_state current_state = sdev->worker.running_state;

    if (current_state != STATE_IDLE && state & ~STATE_CANCEL)
    {
        ALOGE("%s : Invalid state transition to %d when still processing %d", __func__, state, current_state);
        return false;
    }

    if (sdev->worker.running_state == state) {
        ALOGW("%s : Already running in state = %d", __func__, state);
        // Still okay - this is a very unlikely sitation.
        return true;
    }

    ALOGD("%s : Setting state to = %d", __func__, state);
    int rc = eventfd_write(sdev->worker.event_fd, state);
    if (rc)
        ALOGE("%s : Failed to write state to eventfd: %d", __func__, rc);
    return !rc;
}

void * BiometricsFingerprint::worker_thread(void *args){

    sony_fingerprint_device_t *sdev = (sony_fingerprint_device_t*)args;

    bool thread_running = true;
    static const int EVENTS = 2;
    struct epoll_event evnts[EVENTS];

    ALOGI("START");

    while (thread_running) {
        sdev->worker.running_state = STATE_IDLE;
        epoll_wait(sdev->worker.epoll_fd, evnts, EVENTS, -1);
        // Poll always returns if the data in the eventfd is non-zero.

        switch (getNextState(sdev)) {
            case STATE_IDLE:
                ALOGI("%s : IDLE", __func__);
                break;
            case STATE_ENROLL:
                sdev->worker.running_state =  STATE_ENROLL;
                ALOGI("%s : ENROLL", __func__);
                process_enroll(sdev);
                break;
            case STATE_AUTH:
                sdev->worker.running_state = STATE_AUTH;
                ALOGI("%s : AUTH", __func__);
                process_auth(sdev);
                break;
            case STATE_EXIT:
                sdev->worker.running_state = STATE_EXIT;
                ALOGI("%s : EXIT", __func__);
                thread_running = false;
                break;
            case STATE_CANCEL:
                ALOGW("%s : Unexpected STATE_CANCEL", __func__);
                break;
            default:
                ALOGI("%s : UNKNOWN", __func__);
                break;
        }
    }

    ALOGI("%s -", __func__);
    return NULL;
}

    void BiometricsFingerprint::process_enroll(sony_fingerprint_device_t *sdev) {
        // WARNING: Not implemented on any platform
        int32_t print_count = 0;
        // ALOGD("%s : print count is : %u", __func__, print_count);

        BiometricsFingerprint* thisPtr = static_cast<BiometricsFingerprint*>(
                BiometricsFingerprint::getInstance());

        const uint64_t devId = reinterpret_cast<uint64_t>(thisPtr->mDevice);

        std::lock_guard<std::mutex> lock(thisPtr->mClientCallbackMutex);
        if (thisPtr == nullptr || thisPtr->mClientCallback == nullptr) {
            ALOGE("Receiving callbacks before the client callback is registered.");
            return;
        }

        if (fpc_set_power(&sdev->fpc->event, FPC_PWRON) < 0) {
            ALOGE("Error starting device");
            thisPtr->mClientCallback->onError(devId, FingerprintError::ERROR_UNABLE_TO_PROCESS, 0);
            return;
        }

        int ret = fpc_enroll_start(sdev->fpc, print_count);
        if(ret < 0)
        {
            ALOGE("Starting enroll failed: %d\n", ret);
        }

        int status = 1;

        while((status = fpc_capture_image(sdev->fpc)) >= 0) {
            ALOGD("%s : Got Input status=%d", __func__, status);

            if (isCanceled(sdev)) {
                thisPtr->mClientCallback->onError(devId, FingerprintError::ERROR_CANCELED, 0);
                break;
            }

            FingerprintAcquiredInfo hidlStatus = (FingerprintAcquiredInfo)status;

            if (hidlStatus <= FingerprintAcquiredInfo::ACQUIRED_TOO_FAST)
                thisPtr->mClientCallback->onAcquired(devId, hidlStatus, 0);

            //image captured
            if (status == FINGERPRINT_ACQUIRED_GOOD) {
                ALOGI("%s : Enroll Step", __func__);
                uint32_t remaining_touches = 0;
                int ret = fpc_enroll_step(sdev->fpc, &remaining_touches);
                ALOGI("%s: step: %d, touches=%d\n", __func__, ret, remaining_touches);
                if (ret > 0) {
                    ALOGI("%s : Touches Remaining : %d", __func__, remaining_touches);
                    if (remaining_touches > 0) {
                        thisPtr->mClientCallback->onEnrollResult(devId, 0, 0,remaining_touches);
                    }
                }
                else if (ret == 0) {

                    uint32_t print_id = 0;
                    int print_index = fpc_enroll_end(sdev->fpc, &print_id);

                    if (print_index < 0){
                        ALOGE("%s : Error getting new print index : %d", __func__,print_index);
                        thisPtr->mClientCallback->onError(devId, FingerprintError::ERROR_UNABLE_TO_PROCESS, 0);
                        break;
                    }

                    uint32_t db_length = fpc_get_user_db_length(sdev->fpc);
                    ALOGI("%s : User Database Length Is : %lu", __func__,(unsigned long) db_length);
                    fpc_store_user_db(sdev->fpc, db_length, sdev->db_path);
                    ALOGI("%s : Got print id : %lu", __func__,(unsigned long) print_id);
                    thisPtr->mClientCallback->onEnrollResult(devId, print_id, sdev->gid, 0);
                    setState(sdev, STATE_IDLE);
                    break;
                }
                else {
                    ALOGE("Error in enroll step, aborting enroll: %d\n", ret);
                    thisPtr->mClientCallback->onError(devId, FingerprintError::ERROR_UNABLE_TO_PROCESS, 0);
                    break;
                }
            }
        }

        if (fpc_set_power(&sdev->fpc->event, FPC_PWROFF) < 0)
            ALOGE("Error stopping device");
    }


    void BiometricsFingerprint::process_auth(sony_fingerprint_device_t *sdev) {
        int result;
        int status = 1;

        BiometricsFingerprint* thisPtr = static_cast<BiometricsFingerprint*>(
                BiometricsFingerprint::getInstance());

        const uint64_t devId = reinterpret_cast<uint64_t>(thisPtr->mDevice);

        std::lock_guard<std::mutex> lock(thisPtr->mClientCallbackMutex);
        if (thisPtr == nullptr || thisPtr->mClientCallback == nullptr) {
            ALOGE("Receiving callbacks before the client callback is registered.");
            return;
        }

        if (fpc_set_power(&sdev->fpc->event, FPC_PWRON) < 0) {
            ALOGE("Error starting device");
            thisPtr->mClientCallback->onError(devId, FingerprintError::ERROR_UNABLE_TO_PROCESS, 0);
            return;
        }

        fpc_auth_start(sdev->fpc);

        while((status = fpc_capture_image(sdev->fpc)) >= 0 ) {
            ALOGV("%s : Got Input with status %d", __func__, status);

            if (isCanceled(sdev)) {
                thisPtr->mClientCallback->onError(devId, FingerprintError::ERROR_CANCELED, 0);
                break;
            }

            FingerprintAcquiredInfo hidlStatus = (FingerprintAcquiredInfo)status;

            if (hidlStatus <= FingerprintAcquiredInfo::ACQUIRED_TOO_FAST)
                thisPtr->mClientCallback->onAcquired(devId, hidlStatus, 0);

            if (status == FINGERPRINT_ACQUIRED_GOOD) {

                uint32_t print_id = 0;
                int verify_state = fpc_auth_step(sdev->fpc, &print_id);
                ALOGI("%s : Auth step = %d", __func__, verify_state);

                /* After getting something that ought to have been
                 * recognizable: Either send proper notification, or
                 * dummy one where fid=zero stands for unrecognized.
                 */
                uint32_t gid = sdev->gid;
                uint32_t fid = 0;

                if (verify_state >= 0) {
                    if(print_id > 0)
                    {
                        hw_auth_token_t hat;
                        ALOGI("%s : Got print id : %u", __func__, print_id);

                        result = fpc_update_template(sdev->fpc);
                        if(result)
                        {
                            ALOGE("Error updating template: %d", result);
                        } else {
                            result = fpc_store_user_db(sdev->fpc, 0, sdev->db_path);
                            if (result) ALOGE("Error storing database: %d", result);
                        }

                        fpc_get_hw_auth_obj(sdev->fpc, &hat, sizeof(hw_auth_token_t));

                        ALOGI("%s : hat->challenge %ju", __func__, hat.challenge);
                        ALOGI("%s : hat->user_id %ju", __func__, hat.user_id);
                        ALOGI("%s : hat->authenticator_id %ju",  __func__, hat.authenticator_id);
                        ALOGI("%s : hat->authenticator_type %u", __func__, ntohl(hat.authenticator_type));
                        ALOGI("%s : hat->timestamp %lu", __func__, bswap_64(hat.timestamp));
                        ALOGI("%s : hat size %zu", __func__, sizeof(hw_auth_token_t));

                        fid = print_id;

                        const uint8_t* hat2 = reinterpret_cast<const uint8_t *>(&hat);
                        const hidl_vec<uint8_t> token(std::vector<uint8_t>(hat2, hat2 + sizeof(hat)));

                        thisPtr->mClientCallback->onAuthenticated(devId, fid, gid, token);
                        setState(sdev, STATE_IDLE);
                        break;
                    } else {
                        ALOGI("%s : Got print id : %u", __func__, print_id);
                        thisPtr->mClientCallback->onAuthenticated(devId, fid, gid, hidl_vec<uint8_t>());
                    }
                } else {
                    int grp_err = -1;
                    /*
                     * Reinitialize the TZ app and parameters
                     * to clear the TZ error generated by flooding it
                     */
                    fpc_close(&sdev->fpc);
                    fpc_init(&sdev->fpc, sdev->worker.event_fd);
                    grp_err = __setActiveGroup(sdev, gid);
                    if (grp_err)
                        ALOGE("%s : Cannot reinitialize database", __func__);
                }
            }
        }

        if (fpc_set_power(&sdev->fpc->event, FPC_PWROFF) < 0)
            ALOGE("Error stopping device");
    }

} // namespace implementation
}  // namespace V2_1
}  // namespace fingerprint
}  // namespace biometrics
}  // namespace hardware
}  // namespace android
