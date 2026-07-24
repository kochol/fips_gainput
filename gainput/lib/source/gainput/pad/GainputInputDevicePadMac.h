
#ifndef GAINPUTINPUTDEVICEPADMAC_H_
#define GAINPUTINPUTDEVICEPADMAC_H_


namespace gainput
{

class InputDevicePadImplMac : public InputDevicePadImpl
{
public:
	InputDevicePadImplMac(InputManager& manager, InputDevice& device, unsigned index, InputState& state, InputState& previousState);
	~InputDevicePadImplMac();

	InputDevice::DeviceVariant GetVariant() const
	{
		return InputDevice::DV_STANDARD;
	}

	void Update(InputDeltaState* delta);

	InputDevice::DeviceState GetState() const
	{
		return deviceState_;
	}

	bool IsValidButton(DeviceButtonId deviceButton) const;

	bool Vibrate(float leftMotor, float rightMotor);

	// XInput-over-USB backend.
	//
	// Controllers in XInput mode (Logitech F310/F710 with the switch on X, Xbox 360 pads,
	// most third-party "X mode" pads) are vendor-specific USB devices, not HID devices.
	// macOS ships no driver for them, so no IOHIDDevice is ever created and the
	// IOHIDManager path above cannot see them at all. We claim the device directly from
	// user space via IOUSBLib and read its interrupt endpoint ourselves.
	void XInputSetup();
	void XInputTeardown();
	bool XInputTryClaim(unsigned service);
	void XInputHandleReport(const unsigned char* report, unsigned length);
	void XInputPump();

	HashMap<unsigned, DeviceButtonId> buttonDialect_;
	HashMap<unsigned, DeviceButtonId> axisDialect_;
	float minAxis_;
	float maxAxis_;
	float minTriggerAxis_;
	float maxTriggerAxis_;
	InputManager& manager_;
	InputDevice& device_;
	unsigned index_;
	InputState& state_;
	InputState& previousState_;
	InputState nextState_;
	InputDeltaState* delta_;
	InputDevice::DeviceState deviceState_;

	void* ioManager_;

	void* xiNotifyPort_;
	void* xiDevice_;
	void* xiInterface_;
	void* xiSource_;
	unsigned long long xiRegistryId_;
	unsigned xiAddedIter_;
	unsigned xiRemovedIter_;
	unsigned xiPipe_;
	unsigned xiOutPipe_;
	bool xiActive_;
	bool xiHavePrev_;
	unsigned char xiBuf_[32];
	unsigned char xiPrev_[20];

private:
};

}

#endif

