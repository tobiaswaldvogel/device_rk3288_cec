#ifndef ANDROID_HARDWARE_TV_CEC_V1_0_H
#define ANDROID_HARDWARE_TV_CEC_V1_0_H

#include <android/hardware/tv/cec/1.0/IHdmiCec.h>
#include <linux/cec.h>
#include <hidl/Status.h>
#include <hardware/hdmi_cec.h>
#include <hidl/MQDescriptor.h>
#include <utils/Log.h>

#ifndef cec_msg_initiator
/**
 * cec_msg_initiator - return the initiator's logical address.
 * @msg:	the message structure
 */
static inline __u8 cec_msg_initiator(const struct cec_msg *msg)
{
	return msg->msg[0] >> 4;
}
#endif

#ifndef cec_msg_destination
/**
 * cec_msg_destination - return the destination's logical address.
 * @msg:	the message structure
 */
static inline __u8 cec_msg_destination(const struct cec_msg *msg)
{
	return msg->msg[0] & 0xf;
}
#endif

/**
 * cec_msg_init - initialize the message structure.
 * @msg:	the message structure
 * @initiator:	the logical address of the initiator
 * @destination:the logical address of the destination (0xf for broadcast)
 *
 * The whole structure is zeroed, the len field is set to 1 (i.e. a poll
 * message) and the initiator and destination are filled in.
 */
static inline void cec_msg_init(struct cec_msg *msg,
				__u8 initiator, __u8 destination)
{
	memset(msg, 0, sizeof(*msg));
	msg->msg[0] = (initiator << 4) | destination;
	msg->len = 1;
}

using ::android::sp;
using namespace android::hardware;
using namespace android::hardware::tv::cec::V1_0;

namespace android {
namespace hardware {
namespace tv {
namespace cec {
namespace V1_0 {
namespace implementation {

struct HdmiCec : public IHdmiCec, public hidl_death_recipient {
    HdmiCec();

	// Methods from ::android::hardware::tv::cec::V1_0::IHdmiCec follow.
	Return<Result> addLogicalAddress(CecLogicalAddress addr)  override;
	Return<void> clearLogicalAddress()  override;
	Return<void> getPhysicalAddress(getPhysicalAddress_cb _hidl_cb)  override;
	Return<SendMessageResult> sendMessage(const CecMessage& message)  override;
	Return<void> setCallback(const sp<IHdmiCecCallback>& callback)  override;
	Return<int32_t> getCecVersion()  override;
	Return<uint32_t> getVendorId()  override;
	Return<void> getPortInfo(getPortInfo_cb _hidl_cb)  override;
	Return<void> setOption(OptionKey key, bool value)  override;
	Return<void> setLanguage(const hidl_string& language)  override;
	Return<void> enableAudioReturnChannel(int32_t portId, bool enable)  override;
	Return<bool> isConnected(int32_t portId)  override;
   
	virtual void serviceDied(uint64_t /*cookie*/, const wp<::android::hidl::base::V1_0::IBase>& /*who*/) {
        setCallback(nullptr);
    }

private:
	int	 mFd;
	int  mExitFd;
	bool mEnabled;
	bool mSystemControl;
	bool mSimplink;
	
	bool mLastConnectState;
	CecLogicalAddress	mLogAddr;
	
	pthread_t mPollThreadId;
	bool		mPollThreadActive;
	sp<IHdmiCecCallback> mCallback;

	void dumpMessage(const CecMessage& msg, const char* prefix);

	void* pollThread();
	void startPollThread();
	void stopPollThread();
		
	uint16_t getPhysicalAddress();
	bool specialMessageHandling(const CecMessage& msg);
	bool processSimplink(const CecMessage& msg);

	void receiveMessage();
	void hotplugEvent();

	static void* pollThreadWrapper(void *ctx) {
		return (((struct HdmiCec*)ctx)->pollThread());
	};
};

}  // namespace implementation
}  // namespace V1_0
}  // namespace cec
}  // namespace tv
}  // namespace hardware
}  // namespace android

#endif  // ANDROID_HARDWARE_TV_CEC_V1_0_H
