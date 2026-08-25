
#include <gainput/gainput.h>

#ifdef GAINPUT_PLATFORM_MAC

#include "GainputInputDevicePadImpl.h"
#include <gainput/GainputInputDeltaState.h>
#include <gainput/GainputHelpers.h>
#include <gainput/GainputLog.h>

#include "GainputInputDevicePadMac.h"

#import <CoreFoundation/CoreFoundation.h>
#import <IOKit/hid/IOHIDManager.h>
#import <IOKit/hid/IOHIDUsageTables.h>
#import <IOKit/IOKitLib.h>
#import <IOKit/IOCFPlugIn.h>
#import <IOKit/usb/IOUSBLib.h>

#include <string.h>

// kIOMainPortDefault replaced kIOMasterPortDefault in the macOS 12 SDK.
#if defined(MAC_OS_VERSION_12_0) && (MAC_OS_X_VERSION_MAX_ALLOWED >= MAC_OS_VERSION_12_0)
	#define GAINPUT_IO_MAIN_PORT kIOMainPortDefault
#else
	#define GAINPUT_IO_MAIN_PORT kIOMasterPortDefault
#endif

// The USB sources live in a private run loop mode so that pumping them from Update()
// cannot run the application's own timers/sources re-entrantly.
#define GAINPUT_XINPUT_MODE CFSTR("GainputXInputMode")


namespace gainput
{

extern bool MacIsApplicationKey();

namespace {

static const unsigned kMaxPads = 16;
static bool usedPadIndices_[kMaxPads] = { false };

// Registry IDs of USB pads already claimed, so that two InputDevicePad instances never
// bind to the same physical controller.
static unsigned long long usedXInputIds_[kMaxPads] = { 0 };

static inline float FixUpAnalog(float analog, const float minAxis, const float maxAxis, bool symmetric)
{
	analog = analog < minAxis ? minAxis : analog > maxAxis ? maxAxis : analog; // clamp
	analog -= minAxis;
	analog /= (Abs(minAxis) + Abs(maxAxis))*(symmetric ? 0.5f : 1.0f);
	if (symmetric)
	{
		analog -= 1.0f;
	}
	return analog;
}

static void OnDeviceInput(void* inContext, IOReturn inResult, void* inSender, IOHIDValueRef value)
{
	if (!MacIsApplicationKey())
	{
		return;
	}

	IOHIDElementRef elem = IOHIDValueGetElement(value);

	InputDevicePadImplMac* device = reinterpret_cast<InputDevicePadImplMac*>(inContext);
	GAINPUT_ASSERT(device);

	uint32_t usagePage = IOHIDElementGetUsagePage(elem);
	uint32_t usage = IOHIDElementGetUsage(elem);

	if (IOHIDElementGetReportCount(elem) > 1
			|| (usagePage == kHIDPage_GenericDesktop && usage == kHIDUsage_GD_Pointer) )
	{
		return;
	}

    if (usagePage >= kHIDPage_VendorDefinedStart)
    {
        return;
    }

    InputManager& manager = device->manager_;
	CFIndex state = (int)IOHIDValueGetIntegerValue(value);
	float analog = IOHIDValueGetScaledValue(value, kIOHIDValueScaleTypePhysical);

	if (usagePage == kHIDPage_Button && device->buttonDialect_.count(usage))
	{
		const DeviceButtonId buttonId = device->buttonDialect_[usage];
		manager.EnqueueConcurrentChange(device->device_, device->nextState_, device->delta_, buttonId, state != 0);
	}
	else if (usagePage == kHIDPage_GenericDesktop)
	{
		if (usage == kHIDUsage_GD_Hatswitch)
		{
			int dpadX = 0;
			int dpadY = 0;
			switch(state)
			{
				case  0: dpadX =  0; dpadY =  1; break;
				case  1: dpadX =  1; dpadY =  1; break;
				case  2: dpadX =  1; dpadY =  0; break;
				case  3: dpadX =  1; dpadY = -1; break;
				case  4: dpadX =  0; dpadY = -1; break;
				case  5: dpadX = -1; dpadY = -1; break;
				case  6: dpadX = -1; dpadY =  0; break;
				case  7: dpadX = -1; dpadY =  1; break;
				default: dpadX =  0; dpadY =  0; break;
			}
			manager.EnqueueConcurrentChange(device->device_, device->nextState_, device->delta_, PadButtonLeft, dpadX < 0);
			manager.EnqueueConcurrentChange(device->device_, device->nextState_, device->delta_, PadButtonRight, dpadX > 0);
			manager.EnqueueConcurrentChange(device->device_, device->nextState_, device->delta_, PadButtonUp, dpadY > 0);
			manager.EnqueueConcurrentChange(device->device_, device->nextState_, device->delta_, PadButtonDown, dpadY < 0);
		}
		else if (device->axisDialect_.count(usage))
		{
			const DeviceButtonId buttonId = device->axisDialect_[usage];
			if (buttonId == PadButtonAxis4 || buttonId == PadButtonAxis5)
			{
				analog = FixUpAnalog(analog, device->minTriggerAxis_, device->maxTriggerAxis_, false);
			}
			else if (buttonId == PadButtonLeftStickY || buttonId == PadButtonRightStickY)
			{
				analog = -FixUpAnalog(analog, device->minAxis_, device->maxAxis_, true);
			}
			else
			{
				analog = FixUpAnalog(analog, device->minAxis_, device->maxAxis_, true);
			}
			manager.EnqueueConcurrentChange(device->device_, device->nextState_, device->delta_, buttonId, analog);
		}
		else if (device->buttonDialect_.count(usage))
		{
			const DeviceButtonId buttonId = device->buttonDialect_[usage];
			manager.EnqueueConcurrentChange(device->device_, device->nextState_, device->delta_, buttonId, state != 0);
		}
#ifdef GAINPUT_DEBUG
		else
		{
			GAINPUT_LOG("Unmapped button (generic): %d\n", usage);
		}
#endif
	}
#ifdef GAINPUT_DEBUG
	else
	{
		GAINPUT_LOG("Unmapped button: %d\n", usage);
	}
#endif
}

static void OnDeviceConnected(void* inContext, IOReturn inResult, void* inSender, IOHIDDeviceRef inIOHIDDeviceRef)
{
	InputDevicePadImplMac* device = reinterpret_cast<InputDevicePadImplMac*>(inContext);
	GAINPUT_ASSERT(device);

	if (device->deviceState_ != InputDevice::DS_UNAVAILABLE)
	{
		return;
	}

	for (unsigned i = 0; i < device->index_ && i < kMaxPads; ++i)
	{
		if (!usedPadIndices_[i])
		{
			return;
		}
	}

	if (device->index_ < kMaxPads)
	{
		usedPadIndices_[device->index_] = true;
	}
	device->deviceState_ = InputDevice::DS_OK;

	long vendorId = 0;
	long productId = 0;

	if (CFTypeRef tCFTypeRef = IOHIDDeviceGetProperty(inIOHIDDeviceRef, CFSTR(kIOHIDVendorIDKey) ))
	{
		if (CFNumberGetTypeID() == CFGetTypeID(tCFTypeRef))
		{
			CFNumberGetValue((CFNumberRef)tCFTypeRef, kCFNumberSInt32Type, &vendorId);
		}
	}
	if (CFTypeRef tCFTypeRef = IOHIDDeviceGetProperty(inIOHIDDeviceRef, CFSTR(kIOHIDProductIDKey) ))
	{
		if (CFNumberGetTypeID() == CFGetTypeID(tCFTypeRef))
		{
			CFNumberGetValue((CFNumberRef)tCFTypeRef, kCFNumberSInt32Type, &productId);
		}
	}

	if (vendorId == 0x46D && productId == 0xC21F) // Logitech wireless gamepad F710 controller
	{
		printf("Found Logitech controller.\n");
		device->minAxis_ = -(1<<15);
		device->maxAxis_ = 1<<15;
		device->minTriggerAxis_ = 0;
		device->maxTriggerAxis_ = 255;
		device->axisDialect_[kHIDUsage_GD_X] = PadButtonLeftStickX;
		device->axisDialect_[kHIDUsage_GD_Y] = PadButtonLeftStickY;
		device->axisDialect_[kHIDUsage_GD_Rx] = PadButtonRightStickX;
		device->axisDialect_[kHIDUsage_GD_Ry] = PadButtonRightStickY;
		device->axisDialect_[kHIDUsage_GD_Z] = PadButtonAxis4;
		device->axisDialect_[kHIDUsage_GD_Rz] = PadButtonAxis5;
		device->buttonDialect_[0x0a] = PadButtonSelect;
		device->buttonDialect_[0x07] = PadButtonL3;
		device->buttonDialect_[0x08] = PadButtonR3;
		device->buttonDialect_[0x09] = PadButtonStart;
		device->buttonDialect_[0x0c] = PadButtonUp;
		device->buttonDialect_[0x0f] = PadButtonRight;
		device->buttonDialect_[0x0d] = PadButtonDown;
		device->buttonDialect_[0x0e] = PadButtonLeft;
		device->buttonDialect_[0x05] = PadButtonL1;
		device->buttonDialect_[0x06] = PadButtonR1;
		device->buttonDialect_[0x04] = PadButtonY;
		device->buttonDialect_[0x02] = PadButtonB;
		device->buttonDialect_[0x01] = PadButtonA;
		device->buttonDialect_[0x03] = PadButtonX;
		device->buttonDialect_[0x0b] = PadButtonHome;
	}
	else if (vendorId == 0x2563 && productId == 0x575)   // XCSource ZM-X6 Bluetooth gamepad https://www.amazon.com.au/gp/product/B078H5PVJR/ref=oh_aui_detailpage_o00_s00?ie=UTF8&psc=1
	{
		printf("Found XCSource ZM-X6 Bluetooth controller.\n");
		device->minAxis_ = 0;
		device->maxAxis_ = 256;
		device->minTriggerAxis_ = device->minAxis_;
		device->maxTriggerAxis_ = device->maxAxis_;
		device->axisDialect_[kHIDUsage_GD_X] = PadButtonLeftStickX;
		device->axisDialect_[kHIDUsage_GD_Y] = PadButtonLeftStickY;
		device->axisDialect_[kHIDUsage_GD_Z] = PadButtonRightStickX;
		device->axisDialect_[kHIDUsage_GD_Rz] = PadButtonRightStickY;
		device->buttonDialect_[0x05] = PadButtonL1;
		device->buttonDialect_[0x03] = PadButtonA;
	}
	else if (vendorId == 0x054c && (productId == 0x5c4 || productId == 0x9cc)) // Sony DualShock 4
	{
		device->minAxis_ = 0;
		device->maxAxis_ = 256;
		device->minTriggerAxis_ = device->minAxis_;
		device->maxTriggerAxis_ = device->maxAxis_;
		device->axisDialect_[kHIDUsage_GD_X] = PadButtonLeftStickX;
		device->axisDialect_[kHIDUsage_GD_Y] = PadButtonLeftStickY;
		device->axisDialect_[kHIDUsage_GD_Z] = PadButtonRightStickX;
		device->axisDialect_[kHIDUsage_GD_Rz] = PadButtonRightStickY;
		device->axisDialect_[kHIDUsage_GD_Rx] = PadButtonAxis4;
		device->axisDialect_[kHIDUsage_GD_Ry] = PadButtonAxis5;
		device->buttonDialect_[0x09] = PadButtonSelect;
		device->buttonDialect_[0x0b] = PadButtonL3;
		device->buttonDialect_[0x0c] = PadButtonR3;
		device->buttonDialect_[0x0A] = PadButtonStart;
		device->buttonDialect_[0xfffffff0] = PadButtonUp;
		device->buttonDialect_[0xfffffff1] = PadButtonRight;
		device->buttonDialect_[0xfffffff2] = PadButtonDown;
		device->buttonDialect_[0xfffffff3] = PadButtonLeft;
		device->buttonDialect_[0x05] = PadButtonL1;
        device->buttonDialect_[0x07] = PadButtonL2;
		device->buttonDialect_[0x06] = PadButtonR1;
		device->buttonDialect_[0x08] = PadButtonR2;
		device->buttonDialect_[0x04] = PadButtonY;
		device->buttonDialect_[0x03] = PadButtonB;
		device->buttonDialect_[0x02] = PadButtonA;
		device->buttonDialect_[0x01] = PadButtonX;
		device->buttonDialect_[0x0d] = PadButtonHome;
        device->buttonDialect_[0x0e] = PadButton17; // Touch pad
	}
	else if (vendorId == 0x054c && productId == 0x268) // Sony DualShock 3
	{
		device->minAxis_ = 0;
		device->maxAxis_ = 256;
		device->minTriggerAxis_ = device->minAxis_;
		device->maxTriggerAxis_ = device->maxAxis_;
		device->axisDialect_[kHIDUsage_GD_X] = PadButtonLeftStickX;
		device->axisDialect_[kHIDUsage_GD_Y] = PadButtonLeftStickY;
		device->axisDialect_[kHIDUsage_GD_Z] = PadButtonRightStickX;
		device->axisDialect_[kHIDUsage_GD_Rz] = PadButtonRightStickY;
		device->axisDialect_[kHIDUsage_GD_Rx] = PadButtonAxis4;
		device->axisDialect_[kHIDUsage_GD_Ry] = PadButtonAxis5;
		//device->buttonDialect_[0] = PadButtonSelect;
		device->buttonDialect_[2] = PadButtonL3;
		device->buttonDialect_[3] = PadButtonR3;
		device->buttonDialect_[4] = PadButtonStart;
		device->buttonDialect_[5] = PadButtonUp;
		device->buttonDialect_[6] = PadButtonRight;
		device->buttonDialect_[7] = PadButtonDown;
		device->buttonDialect_[8] = PadButtonLeft;
		device->buttonDialect_[11] = PadButtonL1;
		device->buttonDialect_[9] = PadButtonL2;
		device->buttonDialect_[12] = PadButtonR1;
		device->buttonDialect_[10] = PadButtonR2;
		device->buttonDialect_[13] = PadButtonY;
		device->buttonDialect_[14] = PadButtonB;
		device->buttonDialect_[15] = PadButtonA;
		device->buttonDialect_[16] = PadButtonX;
		device->buttonDialect_[17] = PadButtonHome;
	}
	else if (vendorId == 0x045e && (productId == 0x028E || productId == 0x028F || productId == 0x02D1 || productId == 0x02FD)) // Microsoft 360 Controller wired/wireless, Xbox One Controller
	{
		device->minAxis_ = -(1<<15);
		device->maxAxis_ = 1<<15;
		device->minTriggerAxis_ = 0;
		device->maxTriggerAxis_ = 255;
		device->axisDialect_[kHIDUsage_GD_X] = PadButtonLeftStickX;
		device->axisDialect_[kHIDUsage_GD_Y] = PadButtonLeftStickY;
		device->axisDialect_[kHIDUsage_GD_Rx] = PadButtonRightStickX;
		device->axisDialect_[kHIDUsage_GD_Ry] = PadButtonRightStickY;
		device->axisDialect_[kHIDUsage_GD_Z] = PadButtonAxis4;
		device->axisDialect_[kHIDUsage_GD_Rz] = PadButtonAxis5;
		device->buttonDialect_[0x0a] = PadButtonSelect;
		device->buttonDialect_[0x07] = PadButtonL3;
		device->buttonDialect_[0x08] = PadButtonR3;
		device->buttonDialect_[0x09] = PadButtonStart;
		device->buttonDialect_[0x0c] = PadButtonUp;
		device->buttonDialect_[0x0f] = PadButtonRight;
		device->buttonDialect_[0x0d] = PadButtonDown;
		device->buttonDialect_[0x0e] = PadButtonLeft;
		device->buttonDialect_[0x05] = PadButtonL1;
		device->buttonDialect_[0x06] = PadButtonR1;
		device->buttonDialect_[0x04] = PadButtonY;
		device->buttonDialect_[0x02] = PadButtonB;
		device->buttonDialect_[0x01] = PadButtonA;
		device->buttonDialect_[0x03] = PadButtonX;
		device->buttonDialect_[0x0b] = PadButtonHome;
	}
	else if (vendorId == 0x0810 && productId == 0xE501) // Classic USB NES Controller
	{
		device->minAxis_ = 0;
		device->maxAxis_ = 256;
		device->minTriggerAxis_ = device->minAxis_;
		device->maxTriggerAxis_ = device->maxAxis_;
		device->axisDialect_[kHIDUsage_GD_X] = PadButtonLeftStickX;
		device->axisDialect_[kHIDUsage_GD_Y] = PadButtonLeftStickY;
		device->buttonDialect_[0x01] = PadButtonB;
		device->buttonDialect_[0x02] = PadButtonA;
		device->buttonDialect_[0x09] = PadButtonSelect;
		device->buttonDialect_[0x0a] = PadButtonStart;
	}
	else {
		printf("Found unknown pad, disabled. VendorID: 0x%lX ProductID: 0x%lX.\n", vendorId, productId);
	}

}

static void OnDeviceRemoved(void* inContext, IOReturn inResult, void* inSender, IOHIDDeviceRef inIOHIDDeviceRef)
{
	InputDevicePadImplMac* device = reinterpret_cast<InputDevicePadImplMac*>(inContext);
	GAINPUT_ASSERT(device);
	device->deviceState_ = InputDevice::DS_UNAVAILABLE;
	if (device->index_ < kMaxPads)
	{
		usedPadIndices_[device->index_] = true;
	}
}


// ---------------------------------------------------------------------------
// XInput over USB
// ---------------------------------------------------------------------------

// Byte layout of the 20-byte XInput input report, confirmed against a Logitech F310
// (VID 0x046D PID 0xC21D) in X mode:
//   0    report id  (0x00)
//   1    length     (0x14)
//   2    dpad / start / back / thumbs
//   3    shoulders / guide / face buttons
//   4    left trigger  0..255
//   5    right trigger 0..255
//   6..13  LX, LY, RX, RY as little-endian int16
enum { kXInputReportSize = 20 };

// Triggers are analog only in XInput; gainput also exposes digital L2/R2, so we
// synthesise those from the standard XInput trigger threshold.
enum { kXInputTriggerThreshold = 30 };

struct XInputButtonMap
{
	unsigned byteIndex;
	unsigned char mask;
	DeviceButtonId button;
};

static const XInputButtonMap kXInputButtons[] = {
	{ 2, 0x01, PadButtonUp },
	{ 2, 0x02, PadButtonDown },
	{ 2, 0x04, PadButtonLeft },
	{ 2, 0x08, PadButtonRight },
	{ 2, 0x10, PadButtonStart },
	{ 2, 0x20, PadButtonSelect },
	{ 2, 0x40, PadButtonL3 },
	{ 2, 0x80, PadButtonR3 },
	{ 3, 0x01, PadButtonL1 },
	{ 3, 0x02, PadButtonR1 },
	{ 3, 0x04, PadButtonHome },
	{ 3, 0x10, PadButtonA },
	{ 3, 0x20, PadButtonB },
	{ 3, 0x40, PadButtonX },
	{ 3, 0x80, PadButtonY }
};
static const unsigned kXInputButtonCount = sizeof(kXInputButtons) / sizeof(kXInputButtons[0]);

struct XInputAxisMap
{
	unsigned offset;
	DeviceButtonId button;
};

static const XInputAxisMap kXInputAxes[] = {
	{ 6,  PadButtonLeftStickX },
	{ 8,  PadButtonLeftStickY },
	{ 10, PadButtonRightStickX },
	{ 12, PadButtonRightStickY }
};
static const unsigned kXInputAxisCount = sizeof(kXInputAxes) / sizeof(kXInputAxes[0]);

static float XInputStick(const unsigned char* p)
{
	const int raw = (int)(short)((unsigned short)p[0] | ((unsigned short)p[1] << 8));
	float v = (float)raw / 32767.0f;
	if (v > 1.0f)
	{
		v = 1.0f;
	}
	else if (v < -1.0f)
	{
		v = -1.0f;
	}
	// Pads with 8-bit sticks (the F310 among them) pad the value into the high byte of a
	// 16-bit field, so they rest at 128 rather than 0. Snap that residual away.
	if (v > -0.01f && v < 0.01f)
	{
		v = 0.0f;
	}
	return v;
}

static void XInputReadComplete(void* refcon, IOReturn result, void* arg0)
{
	InputDevicePadImplMac* device = reinterpret_cast<InputDevicePadImplMac*>(refcon);
	GAINPUT_ASSERT(device);

	if (result == kIOReturnSuccess)
	{
		device->XInputHandleReport(device->xiBuf_, (unsigned)(uintptr_t)arg0);
	}
	else if (result == kIOReturnNoDevice || result == kIOReturnNotOpen || result == kIOReturnAborted)
	{
		device->XInputTeardown();
		return;
	}

	IOUSBInterfaceInterface500** intf = reinterpret_cast<IOUSBInterfaceInterface500**>(device->xiInterface_);
	if (!intf)
	{
		return;
	}
	if ((*intf)->ReadPipeAsync(intf, device->xiPipe_, device->xiBuf_, sizeof(device->xiBuf_),
			XInputReadComplete, device) != kIOReturnSuccess)
	{
		device->XInputTeardown();
	}
}

static void XInputDeviceAdded(void* refcon, io_iterator_t iterator)
{
	InputDevicePadImplMac* device = reinterpret_cast<InputDevicePadImplMac*>(refcon);
	GAINPUT_ASSERT(device);
	io_service_t service;
	while ((service = IOIteratorNext(iterator)))
	{
		device->XInputTryClaim(service);
		IOObjectRelease(service);
	}
}

static void XInputDeviceRemoved(void* refcon, io_iterator_t iterator)
{
	InputDevicePadImplMac* device = reinterpret_cast<InputDevicePadImplMac*>(refcon);
	GAINPUT_ASSERT(device);
	bool ourDeviceWent = false;
	io_service_t service;
	while ((service = IOIteratorNext(iterator)))
	{
		unsigned long long id = 0;
		if (IORegistryEntryGetRegistryEntryID(service, &id) == KERN_SUCCESS
				&& device->xiActive_ && id == device->xiRegistryId_)
		{
			ourDeviceWent = true;
		}
		IOObjectRelease(service);
	}
	if (ourDeviceWent)
	{
		device->XInputTeardown();
	}
}

}

InputDevicePadImplMac::InputDevicePadImplMac(InputManager& manager, InputDevice& device, unsigned index, InputState& state, InputState& previousState) :
	buttonDialect_(manager.GetAllocator()),
	axisDialect_(manager.GetAllocator()),
	minAxis_(-FLT_MAX),
	maxAxis_(FLT_MAX),
	minTriggerAxis_(-FLT_MAX),
	maxTriggerAxis_(FLT_MAX),
	manager_(manager),
	device_(device),
	index_(index),
	state_(state),
	previousState_(previousState),
	nextState_(manager.GetAllocator(), PadButtonCount_ + PadButtonAxisCount_),
	delta_(0),
	deviceState_(InputDevice::DS_UNAVAILABLE),
    ioManager_(0),
	xiNotifyPort_(0),
	xiDevice_(0),
	xiInterface_(0),
	xiSource_(0),
	xiRegistryId_(0),
	xiAddedIter_(0),
	xiRemovedIter_(0),
	xiPipe_(0),
	xiOutPipe_(0),
	xiActive_(false),
	xiHavePrev_(false)
{
	memset(xiBuf_, 0, sizeof(xiBuf_));
	memset(xiPrev_, 0, sizeof(xiPrev_));

	XInputSetup();

	IOHIDManagerRef ioManager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDManagerOptionNone);

	if (CFGetTypeID(ioManager) != IOHIDManagerGetTypeID())
	{
		return;
	}

	ioManager_ = ioManager;

	static const unsigned kKeyCount = 2;

	CFStringRef keys[kKeyCount] = {
		CFSTR(kIOHIDDeviceUsagePageKey),
		CFSTR(kIOHIDDeviceUsageKey),
	};

	int usagePage = kHIDPage_GenericDesktop;
	int usage = kHIDUsage_GD_GamePad;
	CFNumberRef values[kKeyCount] = {
		CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &usagePage),
		CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &usage),
	};

	CFMutableArrayRef matchingArray = CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);

	CFDictionaryRef matchingDict = CFDictionaryCreate(kCFAllocatorDefault,
			(const void **) keys, (const void **) values, kKeyCount,
			&kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	CFArrayAppendValue(matchingArray, matchingDict);
	CFRelease(matchingDict);
	CFRelease(values[1]);

	usage = kHIDUsage_GD_MultiAxisController;
	values[1] = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &usage);
	
	matchingDict = CFDictionaryCreate(kCFAllocatorDefault,
			(const void **) keys, (const void **) values, kKeyCount,
			&kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	CFArrayAppendValue(matchingArray, matchingDict);
	CFRelease(matchingDict);
	CFRelease(values[1]);

	usage = kHIDUsage_GD_Joystick;
	values[1] = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &usage);
	
	matchingDict = CFDictionaryCreate(kCFAllocatorDefault,
			(const void **) keys, (const void **) values, kKeyCount,
			&kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	CFArrayAppendValue(matchingArray, matchingDict);
	CFRelease(matchingDict);

	for (unsigned i = 0; i < kKeyCount; ++i)
	{
		CFRelease(keys[i]);
		CFRelease(values[i]);
	}

	IOHIDManagerSetDeviceMatchingMultiple(ioManager, matchingArray);
	CFRelease(matchingArray);

	IOHIDManagerRegisterDeviceMatchingCallback(ioManager, OnDeviceConnected, this);
	IOHIDManagerRegisterDeviceRemovalCallback(ioManager, OnDeviceRemoved, this);
	IOHIDManagerRegisterInputValueCallback(ioManager, OnDeviceInput, this);

	IOHIDManagerOpen(ioManager, kIOHIDOptionsTypeNone);

	IOHIDManagerScheduleWithRunLoop(ioManager, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
}

InputDevicePadImplMac::~InputDevicePadImplMac()
{
	XInputTeardown();

	if (xiAddedIter_)
	{
		IOObjectRelease((io_iterator_t)xiAddedIter_);
		xiAddedIter_ = 0;
	}
	if (xiRemovedIter_)
	{
		IOObjectRelease((io_iterator_t)xiRemovedIter_);
		xiRemovedIter_ = 0;
	}
	if (xiNotifyPort_)
	{
		IONotificationPortRef port = reinterpret_cast<IONotificationPortRef>(xiNotifyPort_);
		CFRunLoopRemoveSource(CFRunLoopGetCurrent(), IONotificationPortGetRunLoopSource(port), GAINPUT_XINPUT_MODE);
		IONotificationPortDestroy(port);
		xiNotifyPort_ = 0;
	}

	// The constructor bails out before assigning ioManager_ if IOHIDManagerCreate misbehaves.
	if (!ioManager_)
	{
		return;
	}

	IOHIDManagerRef ioManager = reinterpret_cast<IOHIDManagerRef>(ioManager_);
	IOHIDManagerUnscheduleFromRunLoop(ioManager, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
	IOHIDManagerClose(ioManager, 0);
	CFRelease(ioManager);
}

void InputDevicePadImplMac::Update(InputDeltaState* delta)
{
	delta_ = delta;
	// Must run before the state swap so this frame's packets land in nextState_.
	XInputPump();
	state_ = nextState_;
}

void InputDevicePadImplMac::XInputPump()
{
	if (!xiNotifyPort_)
	{
		return;
	}
	// Drain everything pending in our private mode without blocking. The bound is a
	// safety net against a source that re-signals itself indefinitely.
	for (int i = 0; i < 64; ++i)
	{
		if (CFRunLoopRunInMode(GAINPUT_XINPUT_MODE, 0.0, true) != kCFRunLoopRunHandledSource)
		{
			break;
		}
	}
}

void InputDevicePadImplMac::XInputSetup()
{
	IONotificationPortRef port = IONotificationPortCreate(GAINPUT_IO_MAIN_PORT);
	if (!port)
	{
		return;
	}
	xiNotifyPort_ = port;
	CFRunLoopAddSource(CFRunLoopGetCurrent(), IONotificationPortGetRunLoopSource(port), GAINPUT_XINPUT_MODE);

	// Matching on idVendor/idProduct does not work against IOUSBHostDevice, so match the
	// class and filter on the descriptors ourselves once the device shows up.
	CFMutableDictionaryRef matching = IOServiceMatching("IOUSBHostDevice");
	if (!matching)
	{
		return;
	}
	// IOServiceAddMatchingNotification consumes a reference per registration.
	CFRetain(matching);

	io_iterator_t addedIter = 0;
	if (IOServiceAddMatchingNotification(port, kIOFirstMatchNotification, matching,
			XInputDeviceAdded, this, &addedIter) == KERN_SUCCESS)
	{
		xiAddedIter_ = addedIter;
		// Draining arms the notification and delivers already-connected devices.
		XInputDeviceAdded(this, addedIter);
	}

	io_iterator_t removedIter = 0;
	if (IOServiceAddMatchingNotification(port, kIOTerminatedNotification, matching,
			XInputDeviceRemoved, this, &removedIter) == KERN_SUCCESS)
	{
		xiRemovedIter_ = removedIter;
		XInputDeviceRemoved(this, removedIter);
	}
}

bool InputDevicePadImplMac::XInputTryClaim(unsigned service)
{
	if (xiActive_)
	{
		return false;
	}

	io_service_t usbService = (io_service_t)service;

	unsigned long long regId = 0;
	if (IORegistryEntryGetRegistryEntryID(usbService, &regId) != KERN_SUCCESS)
	{
		return false;
	}
	for (unsigned i = 0; i < kMaxPads; ++i)
	{
		if (usedXInputIds_[i] == regId)
		{
			return false;
		}
	}

	IOCFPlugInInterface** plugin = 0;
	SInt32 score = 0;
	if (IOCreatePlugInInterfaceForService(usbService, kIOUSBDeviceUserClientTypeID,
			kIOCFPlugInInterfaceID, &plugin, &score) != KERN_SUCCESS || !plugin)
	{
		return false;
	}

	IOUSBDeviceInterface500** dev = 0;
	HRESULT hr = (*plugin)->QueryInterface(plugin, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID500), (LPVOID*)&dev);
	IODestroyPlugInInterface(plugin);
	if (hr || !dev)
	{
		return false;
	}

	// XInput controllers declare themselves entirely vendor-specific at the device level.
	// Anything else is left alone: we must open and configure a device to see its
	// interfaces, and we do not want to disturb unrelated hardware.
	UInt8 devClass = 0, devSubClass = 0, devProtocol = 0;
	(*dev)->GetDeviceClass(dev, &devClass);
	(*dev)->GetDeviceSubClass(dev, &devSubClass);
	(*dev)->GetDeviceProtocol(dev, &devProtocol);
	if (devClass != 0xFF || devSubClass != 0xFF || devProtocol != 0xFF)
	{
		(*dev)->Release(dev);
		return false;
	}

	if ((*dev)->USBDeviceOpen(dev) != kIOReturnSuccess)
	{
		// Another process already owns it exclusively.
		(*dev)->Release(dev);
		return false;
	}

	IOUSBConfigurationDescriptorPtr conf = 0;
	if ((*dev)->GetConfigurationDescriptorPtr(dev, 0, &conf) != kIOReturnSuccess || !conf)
	{
		(*dev)->USBDeviceClose(dev);
		(*dev)->Release(dev);
		return false;
	}
	// No kernel driver claims these devices, so nothing has set a configuration and no
	// interface nubs exist until we do it ourselves.
	(*dev)->SetConfiguration(dev, conf->bConfigurationValue);

	IOUSBFindInterfaceRequest req;
	req.bInterfaceClass = kIOUSBFindInterfaceDontCare;
	req.bInterfaceSubClass = kIOUSBFindInterfaceDontCare;
	req.bInterfaceProtocol = kIOUSBFindInterfaceDontCare;
	req.bAlternateSetting = kIOUSBFindInterfaceDontCare;

	io_iterator_t interfaceIter = 0;
	if ((*dev)->CreateInterfaceIterator(dev, &req, &interfaceIter) != kIOReturnSuccess)
	{
		(*dev)->USBDeviceClose(dev);
		(*dev)->Release(dev);
		return false;
	}

	IOUSBInterfaceInterface500** found = 0;
	UInt8 inPipe = 0;
	UInt8 outPipe = 0;
	io_service_t usbInterface;
	while (!found && (usbInterface = IOIteratorNext(interfaceIter)))
	{
		IOCFPlugInInterface** iplugin = 0;
		if (IOCreatePlugInInterfaceForService(usbInterface, kIOUSBInterfaceUserClientTypeID,
				kIOCFPlugInInterfaceID, &iplugin, &score) == KERN_SUCCESS && iplugin)
		{
			IOUSBInterfaceInterface500** intf = 0;
			HRESULT ihr = (*iplugin)->QueryInterface(iplugin,
					CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID500), (LPVOID*)&intf);
			IODestroyPlugInInterface(iplugin);

			if (!ihr && intf)
			{
				UInt8 ic = 0, isc = 0, ip = 0;
				(*intf)->GetInterfaceClass(intf, &ic);
				(*intf)->GetInterfaceSubClass(intf, &isc);
				(*intf)->GetInterfaceProtocol(intf, &ip);

				// 0xFF/0x5D/0x01 is the XInput control interface.
				if (ic == 0xFF && isc == 0x5D && ip == 0x01
						&& (*intf)->USBInterfaceOpen(intf) == kIOReturnSuccess)
				{
					UInt8 endpoints = 0;
					(*intf)->GetNumEndpoints(intf, &endpoints);
					for (UInt8 pipe = 1; pipe <= endpoints; ++pipe)
					{
						UInt8 dir = 0, num = 0, transferType = 0, interval = 0;
						UInt16 maxPacket = 0;
						if ((*intf)->GetPipeProperties(intf, pipe, &dir, &num, &transferType,
								&maxPacket, &interval) != kIOReturnSuccess
								|| transferType != kUSBInterrupt)
						{
							continue;
						}
						if (dir == kUSBIn && !inPipe)
						{
							inPipe = pipe;
						}
						else if (dir == kUSBOut && !outPipe)
						{
							outPipe = pipe;
						}
					}

					if (inPipe)
					{
						found = intf;
					}
					else
					{
						(*intf)->USBInterfaceClose(intf);
						(*intf)->Release(intf);
					}
				}
				else
				{
					(*intf)->Release(intf);
				}
			}
		}
		IOObjectRelease(usbInterface);
	}
	IOObjectRelease(interfaceIter);

	if (!found)
	{
		(*dev)->USBDeviceClose(dev);
		(*dev)->Release(dev);
		return false;
	}

	CFRunLoopSourceRef source = 0;
	if ((*found)->CreateInterfaceAsyncEventSource(found, &source) != kIOReturnSuccess || !source)
	{
		(*found)->USBInterfaceClose(found);
		(*found)->Release(found);
		(*dev)->USBDeviceClose(dev);
		(*dev)->Release(dev);
		return false;
	}
	CFRunLoopAddSource(CFRunLoopGetCurrent(), source, GAINPUT_XINPUT_MODE);

	xiDevice_ = dev;
	xiInterface_ = found;
	xiSource_ = source;
	xiPipe_ = inPipe;
	xiOutPipe_ = outPipe;
	xiRegistryId_ = regId;
	xiActive_ = true;
	xiHavePrev_ = false;
	deviceState_ = InputDevice::DS_OK;
	if (index_ < kMaxPads)
	{
		usedXInputIds_[index_] = regId;
	}

	// Interrupt pipes reject ReadPipeTO's timeouts, so reads must be asynchronous.
	if ((*found)->ReadPipeAsync(found, inPipe, xiBuf_, sizeof(xiBuf_), XInputReadComplete, this) != kIOReturnSuccess)
	{
		XInputTeardown();
		return false;
	}

	return true;
}

void InputDevicePadImplMac::XInputTeardown()
{
	if (xiSource_)
	{
		CFRunLoopRemoveSource(CFRunLoopGetCurrent(), reinterpret_cast<CFRunLoopSourceRef>(xiSource_), GAINPUT_XINPUT_MODE);
		xiSource_ = 0;
	}
	if (xiInterface_)
	{
		IOUSBInterfaceInterface500** intf = reinterpret_cast<IOUSBInterfaceInterface500**>(xiInterface_);
		(*intf)->USBInterfaceClose(intf);
		(*intf)->Release(intf);
		xiInterface_ = 0;
	}
	if (xiDevice_)
	{
		IOUSBDeviceInterface500** dev = reinterpret_cast<IOUSBDeviceInterface500**>(xiDevice_);
		(*dev)->USBDeviceClose(dev);
		(*dev)->Release(dev);
		xiDevice_ = 0;
	}
	if (index_ < kMaxPads && xiRegistryId_ && usedXInputIds_[index_] == xiRegistryId_)
	{
		usedXInputIds_[index_] = 0;
	}

	const bool wasActive = xiActive_;
	xiRegistryId_ = 0;
	xiPipe_ = 0;
	xiOutPipe_ = 0;
	xiActive_ = false;
	xiHavePrev_ = false;
	if (wasActive)
	{
		deviceState_ = InputDevice::DS_UNAVAILABLE;
	}
}

void InputDevicePadImplMac::XInputHandleReport(const unsigned char* report, unsigned length)
{
	if (length < kXInputReportSize || report[0] != 0x00 || report[1] != kXInputReportSize)
	{
		return;
	}

	const bool first = !xiHavePrev_;

	for (unsigned i = 0; i < kXInputButtonCount; ++i)
	{
		const XInputButtonMap& m = kXInputButtons[i];
		const bool down = (report[m.byteIndex] & m.mask) != 0;
		if (first || down != ((xiPrev_[m.byteIndex] & m.mask) != 0))
		{
			manager_.EnqueueConcurrentChange(device_, nextState_, delta_, m.button, down);
		}
	}

	if (first || report[4] != xiPrev_[4])
	{
		manager_.EnqueueConcurrentChange(device_, nextState_, delta_, PadButtonAxis4, (float)report[4] / 255.0f);
		const bool down = report[4] > kXInputTriggerThreshold;
		if (first || down != (xiPrev_[4] > kXInputTriggerThreshold))
		{
			manager_.EnqueueConcurrentChange(device_, nextState_, delta_, PadButtonL2, down);
		}
	}
	if (first || report[5] != xiPrev_[5])
	{
		manager_.EnqueueConcurrentChange(device_, nextState_, delta_, PadButtonAxis5, (float)report[5] / 255.0f);
		const bool down = report[5] > kXInputTriggerThreshold;
		if (first || down != (xiPrev_[5] > kXInputTriggerThreshold))
		{
			manager_.EnqueueConcurrentChange(device_, nextState_, delta_, PadButtonR2, down);
		}
	}

	for (unsigned i = 0; i < kXInputAxisCount; ++i)
	{
		const XInputAxisMap& a = kXInputAxes[i];
		if (first || report[a.offset] != xiPrev_[a.offset] || report[a.offset + 1] != xiPrev_[a.offset + 1])
		{
			manager_.EnqueueConcurrentChange(device_, nextState_, delta_, a.button, XInputStick(report + a.offset));
		}
	}

	memcpy(xiPrev_, report, kXInputReportSize);
	xiHavePrev_ = true;
}

bool InputDevicePadImplMac::Vibrate(float leftMotor, float rightMotor)
{
	if (!xiActive_ || !xiInterface_ || !xiOutPipe_)
	{
		return false;
	}

	if (leftMotor < 0.0f) { leftMotor = 0.0f; } else if (leftMotor > 1.0f) { leftMotor = 1.0f; }
	if (rightMotor < 0.0f) { rightMotor = 0.0f; } else if (rightMotor > 1.0f) { rightMotor = 1.0f; }

	unsigned char cmd[8] = { 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
	cmd[3] = (unsigned char)(leftMotor * 255.0f);
	cmd[4] = (unsigned char)(rightMotor * 255.0f);

	IOUSBInterfaceInterface500** intf = reinterpret_cast<IOUSBInterfaceInterface500**>(xiInterface_);
	return (*intf)->WritePipe(intf, (UInt8)xiOutPipe_, cmd, sizeof(cmd)) == kIOReturnSuccess;
}

bool InputDevicePadImplMac::IsValidButton(DeviceButtonId deviceButton) const
{
	if (xiActive_)
	{
		// XInput has a fixed layout, so the HID dialects do not apply.
		switch (deviceButton)
		{
		case PadButtonLeftStickX:
		case PadButtonLeftStickY:
		case PadButtonRightStickX:
		case PadButtonRightStickY:
		case PadButtonAxis4:
		case PadButtonAxis5:
		case PadButtonUp:
		case PadButtonDown:
		case PadButtonLeft:
		case PadButtonRight:
		case PadButtonStart:
		case PadButtonSelect:
		case PadButtonL1:
		case PadButtonR1:
		case PadButtonL2:
		case PadButtonR2:
		case PadButtonL3:
		case PadButtonR3:
		case PadButtonA:
		case PadButtonB:
		case PadButtonX:
		case PadButtonY:
		case PadButtonHome:
			return true;
		default:
			return false;
		}
	}

	if (buttonDialect_.empty())
	{
		return deviceButton < PadButtonMax_;
	}

	for (HashMap<unsigned, DeviceButtonId>::const_iterator it = buttonDialect_.begin();
			it != buttonDialect_.end();
			++it)
	{
		if (it->second == deviceButton)
		{
			return true;
		}
	}

	for (HashMap<unsigned, DeviceButtonId>::const_iterator it = axisDialect_.begin();
			it != axisDialect_.end();
			++it)
	{
		if (it->second == deviceButton)
		{
			return true;
		}
	}

	return false;
}

}

#endif

