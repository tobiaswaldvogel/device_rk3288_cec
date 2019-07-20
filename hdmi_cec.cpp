/*
 * Copyright (C) 2017 The Android Open Source Project
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

#define LOG_TAG "android.hardware.tv.cec@1.0-service.rk3288"

#include <algorithm>

#include <android-base/logging.h>
#include <assert.h>
#include <chrono>
#include <dirent.h>
#include <pthread.h>
#include <regex>
#include <stdio.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>

#include <cutils/uevent.h>
#include <cutils/properties.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/eventfd.h>
#include <utils/Errors.h>
#include <utils/StrongPointer.h>
#include <poll.h>

#include <linux/cec.h>

#include "hdmi_cec.h"

#define CEC_VERSION		CEC_OP_CEC_VERSION_1_4
#define CEC_VENDOR_ID 1
#define CEC_PORT_ID 1

namespace android {
namespace hardware {
namespace tv {
namespace cec {
namespace V1_0 {
namespace implementation {

# define CEC_VENDOR_ID_LG				0xE091

#define SL_COMMAND_TYPE_HDDRECORDER_DISC  0x01
#define SL_COMMAND_TYPE_VCR               0x02
#define SL_COMMAND_TYPE_DVDPLAYER         0x03
#define SL_COMMAND_TYPE_HDDRECORDER_DISC2 0x04
#define SL_COMMAND_TYPE_HDDRECORDER       0x05

#define SL_COMMAND_INIT                 0x01
#define SL_COMMAND_ACK_INIT             0x02
#define SL_COMMAND_POWER_ON             0x03
#define SL_COMMAND_CONNECT_REQUEST      0x04
#define SL_COMMAND_SET_DEVICE_MODE      0x05
#define SL_COMMAND_REQUEST_RECONNECT    0x0b
#define SL_COMMAND_REQUEST_POWER_STATUS 0xa0

#define CEC_DECK_INFO_OTHER_STATUS_LG   0x20

void HdmiCec::dumpMessage(const CecMessage& msg, const char* prefix)
{
	IF_ALOGD() {
		if (msg.body.size() == 0) {
			ALOGD("%s: Msg %d->%d Poll Message", prefix, (int)msg.initiator, (int)msg.destination);
			return;
		}
		
		char	buffer[256];
		int 	pos = 0;
		
		buffer[0] = 0;
		for (size_t i = 0; i < msg.body.size(); ++i)
			pos += snprintf(buffer + pos, sizeof(buffer) -pos, ":%02X", msg.body[i]);
			
		ALOGD("%s: Msg %d->%d opcode [%02X]   %02X%s", prefix,
			(int)msg.initiator, (int)msg.destination,  msg.body[0],
			((int)msg.initiator <<4) | (int)msg.destination, 
			buffer
		);
	}
}

bool HdmiCec::processSimplink(const CecMessage& msg)
{
	if (msg.body.size() < 2)
		return false;

	switch (msg.body[1]) {
		case SL_COMMAND_INIT: {
			CecMessage reply {
				.initiator   = mLogAddr,
				.destination = msg.initiator,
				.body = { CEC_MSG_VENDOR_COMMAND, SL_COMMAND_ACK_INIT, SL_COMMAND_TYPE_HDDRECORDER }
			};
			return sendMessage(reply) == SendMessageResult::SUCCESS;
		}

		case SL_COMMAND_CONNECT_REQUEST: {
			CecMessage reply {
				.initiator   = mLogAddr,
				.destination = msg.initiator,
				.body = { CEC_MSG_VENDOR_COMMAND, SL_COMMAND_SET_DEVICE_MODE, (uint8_t)CecDeviceType::RECORDER }
			};
			return sendMessage(reply) == SendMessageResult::SUCCESS;
		}

		default: return false;
	}
}

bool HdmiCec::specialMessageHandling(const CecMessage& msg)
{
	switch (msg.body[0]) {
		case CEC_MSG_VENDOR_COMMAND:
			if (mSimplink)
				return processSimplink(msg);
			return false;

		case CEC_MSG_DEVICE_VENDOR_ID: {
			uint32_t	vendor_id = 0;

			for (size_t i = 1; i < msg.body.size(); i++) {
				vendor_id <<= 8;
				vendor_id |= msg.body[i];
			}

			if (vendor_id == CEC_VENDOR_ID_LG && !mSimplink) {
				mSimplink = true;
				ALOGI("%s: LG SimpLink detected", __func__);
			}

			return false;
		}

		case CEC_MSG_GIVE_DECK_STATUS: {
			CecMessage reply {
				.initiator   = mLogAddr,
				.destination = msg.initiator,
				.body = { CEC_MSG_DECK_STATUS, mSimplink ?
					 static_cast<uint8_t>(CEC_DECK_INFO_OTHER_STATUS_LG) :
					 static_cast<uint8_t>(CEC_OP_DECK_INFO_OTHER) }
			};
			return sendMessage(reply) == SendMessageResult::SUCCESS;
		}

		case CEC_MSG_REQUEST_ACTIVE_SOURCE: {
			uint16_t physAddr = getPhysicalAddress();
			
			CecMessage reply {
				.initiator   = mLogAddr,
				.destination = msg.initiator,
				.body = { CEC_MSG_ACTIVE_SOURCE, (uint8_t)(physAddr >> 8), (uint8_t)(physAddr & 0xff) }
			};
			return sendMessage(reply) == SendMessageResult::SUCCESS;
		}

		default: return false;
	}
}

void *HdmiCec::pollThread()
{
	ALOGI("%s startup", __func__);

	mExitFd = eventfd(0, EFD_NONBLOCK);
	if (mExitFd < 0) {
		ALOGE("%s could not create event fd: %s", __func__, strerror(errno));
		return nullptr;
	}

	struct pollfd ufds[3] = {
			{ mExitFd, POLLIN, 0 },
			{ mFd, POLLIN | POLLPRI | POLLRDNORM, 0 },
		};

	mPollThreadActive = true;

	while (1) {
		ufds[0].revents = 0;
		ufds[1].revents = 0;

		if (poll(ufds, 2, -1) <= 0)
			continue;

		if (ufds[0].revents & POLLIN)
			break;

		if (ufds[1].revents & POLLIN)
			receiveMessage();
		
		if (ufds[1].revents & POLLPRI)
			hotplugEvent();
	}

	close(mExitFd);
	mExitFd = -1;
	mPollThreadActive = false;

	ALOGD("%s shutdown", __func__);
	return nullptr;
}

void HdmiCec::hotplugEvent()
{
	struct cec_event	event;
	int					rc;

	HotplugEvent hotplugEvent { .portId = CEC_PORT_ID };

	rc = ioctl(mFd, CEC_DQEVENT, &event);
	if (rc) {
		ALOGE("%s: CEC_DQEVENT failed: %d (%s)", __func__, rc, strerror(errno));
		return;
	}

	if (event.event != CEC_EVENT_STATE_CHANGE)
		return;

	hotplugEvent.connected = event.state_change.phys_addr != CEC_PHYS_ADDR_INVALID;

	if (mLastConnectState == hotplugEvent.connected)
		return;

	mLastConnectState = hotplugEvent.connected;

	if (mCallback == nullptr)
		return;

	if (!hotplugEvent.connected) {
		CecMessage cecMessage {
			.initiator   = CecLogicalAddress::TV,
			.destination = mLogAddr,
			.body = { CEC_MSG_STANDBY }
		};

		mCallback->onCecMessage(cecMessage);	
	}

	mCallback->onHotplugEvent(hotplugEvent);
}

void HdmiCec::receiveMessage()
{
	struct cec_msg		msg;
	int					rc;

	rc = ioctl(mFd,  CEC_RECEIVE, &msg);
	if (rc) {
		ALOGE("%s: CEC_RECEIVE failed: %d (%s)", __func__, rc, strerror(errno));
		return;
	}

	size_t length = msg.len -1;
	
	if (length > (size_t)(MaxLength::MESSAGE_BODY))
		length = (size_t)(MaxLength::MESSAGE_BODY);

	CecMessage cecMessage {
		.initiator   = (CecLogicalAddress)cec_msg_initiator(&msg),
		.destination = (CecLogicalAddress)cec_msg_destination(&msg)
	};

	cecMessage.body.resize(length);
	for (size_t i = 0; i < length; ++i)
		cecMessage.body[i] = static_cast<uint8_t>(msg.msg[i+1]);
		
	dumpMessage(cecMessage, __func__);

	if (cecMessage.body.size() > 0 && specialMessageHandling(cecMessage))
		return;

	if (mCallback == nullptr) {
		ALOGD("%s: No callback registered", __func__);
		return;
	}

	mCallback->onCecMessage(cecMessage);	
}

Return<SendMessageResult> HdmiCec::sendMessage(const CecMessage& msg)
{
	if (!mEnabled) {
		ALOGI("%s: cec is disabled", __func__);
        return SendMessageResult::FAIL;
	}

	dumpMessage(msg, __func__);

	cec_msg cec;
	int		rc;

	cec_msg_init(&cec, static_cast<uint8_t>(msg.initiator), static_cast<uint8_t>(msg.destination));
	cec.msg[1] = msg.body[0];
	cec.len = msg.body.size() + 1; 

	if (cec.len >= CEC_MAX_MSG_SIZE)
		cec.len = 0;
	else 
		for (size_t i = 1; i < msg.body.size(); ++i)
			cec.msg[i+1] = msg.body[i];

	rc = ioctl(mFd, CEC_TRANSMIT, &cec);
	if (rc < 0) {
		if (msg.initiator == msg.destination && msg.body.size() == 0)
			/* Discovery poll message: Indicate usable LA with NACK */
			return SendMessageResult::NACK;
		
		ALOGE("ioctl err: %d %s", rc, strerror(errno));
		return SendMessageResult::FAIL;
	}

	if (cec.tx_status & CEC_TX_STATUS_OK)
		return SendMessageResult::SUCCESS;

	if (cec.tx_status & CEC_TX_STATUS_NACK)
		return SendMessageResult::NACK;

	return SendMessageResult::BUSY;
}

Return<Result> HdmiCec::addLogicalAddress(CecLogicalAddress addr)
{
	CecDeviceType	dev_type;
    __uint8_t		addr_type;
	__uint16_t		addr_mask = 1 << static_cast<uint8_t>(addr);

	if (0 != (addr_mask & CEC_LOG_ADDR_MASK_TV)) {
		dev_type = CecDeviceType::TV;
		addr_type = 0;
	} else if (0 != (addr_mask & CEC_LOG_ADDR_MASK_RECORD)) {
		dev_type = CecDeviceType::RECORDER;
		addr_type = 1;
	} else if (0 != (addr_mask & CEC_LOG_ADDR_MASK_TUNER)) {
		dev_type = CecDeviceType::TUNER;
		addr_type = 2;
	} else if (0 != (addr_mask & CEC_LOG_ADDR_MASK_PLAYBACK)) {
		dev_type = CecDeviceType::PLAYBACK;
		addr_type = 3;
	} else if (0 != (addr_mask & CEC_LOG_ADDR_MASK_AUDIOSYSTEM)) {
		dev_type = CecDeviceType::AUDIO_SYSTEM;
		addr_type = 4;
	} else {
		ALOGE("%s invalid logic addr", __func__);
		return Result::FAILURE_INVALID_ARGS;
	}

	int				rc;
	cec_log_addrs	log_addrs;

	memset(&log_addrs, 0, sizeof(log_addrs));

	rc = ioctl(mFd, CEC_ADAP_G_LOG_ADDRS, &log_addrs);
	if (0 == rc)
		for (int i=0; i < log_addrs.num_log_addrs; i++) 
			if (log_addrs.log_addr[i] == static_cast<uint8_t>(addr))
				return Result::SUCCESS;

	memset(&log_addrs, 0, sizeof(log_addrs));
	log_addrs.num_log_addrs = 1;
	log_addrs.log_addr[0] = (int)addr;
	log_addrs.primary_device_type[0] = (__u8)dev_type;
	log_addrs.log_addr_type[0] = addr_type;
	log_addrs.vendor_id = CEC_VENDOR_ID;
	log_addrs.cec_version = CEC_VERSION;
	strcpy(log_addrs.osd_name, "RK");

	rc = ioctl(mFd, CEC_ADAP_S_LOG_ADDRS, &log_addrs);
	if (rc) {
		ALOGE("%s: CEC_ADAP_S_LOG_ADDRS failed: %d (%s)", __func__, rc, strerror(errno));
		return Result::FAILURE_UNKNOWN;
	}

	mLogAddr = addr;
	return Result::SUCCESS;
}

Return<void> HdmiCec::clearLogicalAddress()
{
	ALOGD("%s", __func__);

	struct cec_log_addrs laddrs;
	int	rc;

	memset(&laddrs, 0, sizeof(laddrs));
	rc = ioctl(mFd, CEC_ADAP_S_LOG_ADDRS, &laddrs);

	if (rc)
		ALOGE("%s: CEC_ADAP_S_LOG_ADDRS failed: %d (%s)", __func__, rc, strerror(errno));

	mLogAddr = CecLogicalAddress::UNREGISTERED;
    return Void();
}

uint16_t HdmiCec::getPhysicalAddress()
{
	uint16_t	addr = 0;
	int			rc, i;

	for (i=0; i<4; i++) {
		rc = ioctl(mFd, CEC_ADAP_G_PHYS_ADDR, &addr);

		if (rc == 0) {
			ALOGD("get physical address returning %04x", addr);
			return addr;
		}

		ALOGD("read physical addr error! ret: %d\n", rc);
		usleep(200000);
	}

	ALOGE("read physical addr error! ret: %d\n", rc);
	return CEC_PHYS_ADDR_INVALID	;
}

Return<void> HdmiCec::getPhysicalAddress(getPhysicalAddress_cb _hidl_cb)
{
	uint16_t	addr = getPhysicalAddress();
	
	if (addr == CEC_PHYS_ADDR_INVALID)
		_hidl_cb(Result::FAILURE_UNKNOWN, addr);
	else
		_hidl_cb(Result::SUCCESS, addr);

    return Void();
}

Return<int32_t> HdmiCec::getCecVersion() {
	return CEC_VERSION;
}

Return<uint32_t> HdmiCec::getVendorId() {
	return CEC_VENDOR_ID;
}

Return<void> HdmiCec::getPortInfo(getPortInfo_cb _hidl_cb)
{
    hidl_vec<HdmiPortInfo> portInfos;
    uint16_t	addr;

	addr = getPhysicalAddress();

    portInfos.resize(1);
	portInfos[0] = {
		.type = HdmiPortType::OUTPUT,
		.portId = CEC_PORT_ID,
		.cecSupported = true,
		.arcSupported = false,
		.physicalAddress = addr
	};

    _hidl_cb(portInfos);	
	return Void();
}

Return<void> HdmiCec::setCallback(const sp<IHdmiCecCallback>& callback)
{
	stopPollThread();

    if (mCallback != nullptr) {
        mCallback->unlinkToDeath(this);
        mCallback = nullptr;
    }

    if (callback != nullptr) {
        mCallback = callback;
        mCallback->linkToDeath(this, 0 );
    }

	if (mEnabled && mCallback)
		startPollThread();

    return Void();
}

Return<void> HdmiCec::setOption(OptionKey key, bool value)
{
	switch (key) {
		case OptionKey::WAKEUP:
		break;

	case OptionKey::ENABLE_CEC:
		mEnabled = value ? 1 : 0;
		break;

	case OptionKey::SYSTEM_CEC_CONTROL:
		mSystemControl = value ? 1 : 0;
		break;
	}	

	if (mEnabled && mCallback)
		startPollThread();
	else
		stopPollThread();

	return Void();
}
Return<void> HdmiCec::setLanguage(const hidl_string& language)
{
    if (language.size() != 3) {
		ALOGE("%s: Wrong language code: expected 3 letters, but it was %zd", __func__, language.size());
        return Void();
    }
/*
	const char *languageStr = language.c_str();

	CecMessage msg {
		.initiator   = mLogAddr,
		.destination = CecLogicalAddress::BROADCAST,
		.body = { CEC_MSG_SET_MENU_LANGUAGE, languageStr[0], languageStr[1], languageStr[2] }
	};
	sendMessage(msg);
*/
	return Void();
}

Return<void> HdmiCec::enableAudioReturnChannel(int32_t portId, bool enable) {
	return Void();
}

Return<bool> HdmiCec::isConnected(int32_t portId) {
	(void)portId;
	
	return getPhysicalAddress() != CEC_PHYS_ADDR_INVALID;
}

void HdmiCec::startPollThread()
{
	if (mPollThreadActive)
		return;
		
	int rc = pthread_create(&mPollThreadId, 0, pollThreadWrapper, this);
	if (rc)
		ALOGE("Failed to create poll thread %s", strerror(errno));
}

void HdmiCec::stopPollThread()
{
	if (!mPollThreadActive)
		return;
	
	int			written;
	uint64_t	counter = 1;

	written = write(mExitFd, &counter, sizeof(counter));
	if (written != sizeof(counter))
		ALOGE("%s: Failed to write to event fd %s", __func__, strerror(errno));

	pthread_join(mPollThreadId, NULL);	
}

HdmiCec::HdmiCec() :
		mExitFd(-1),
		mEnabled(true),
		mSystemControl(true),
		mSimplink(false),
		mLastConnectState(false),
		mLogAddr(CecLogicalAddress::UNREGISTERED),
		mPollThreadActive(false),
		mCallback(nullptr)
{
	ALOGD("Using device dev/cec0");
	mFd = open("dev/cec0", O_RDWR);

	if (mFd < 0) {
		ALOGE("open error %s", strerror(errno));
		abort();
	}
	
	__u32 mode = CEC_MODE_INITIATOR | CEC_MODE_FOLLOWER;
	
	int rc = ioctl(mFd, CEC_S_MODE, &mode);
	if (rc)
		ALOGE("%s: CEC_S_MODE failed: %d (%s)", __func__, rc, strerror(errno));

	property_set("vendor.sys.hdmicec.version", "1.0");
}

}  // namespace implementation
}  // namespace V1_0
}  // namespace cec
}  // namespace tv
}  // namespace hardware
}  // namespace android
