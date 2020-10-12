/*
 * If not stated otherwise in this file or this component's LICENSE file the
 * following copyright and licenses apply:
 *
 * Copyright 2020 RDK Management
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "../Module.h"
#include "../DisplayInfoTracing.h"
#include "../ExtendedDisplayIdentification.h"

#include <interfaces/IDisplayInfo.h>

#include <nexus_config.h>
#include <nexus_platform.h>
#include <nxclient.h>
#include <nexus_core_utils.h>

#if ( (NEXUS_PLATFORM_VERSION_MAJOR > 18) ||  \
      ( (NEXUS_PLATFORM_VERSION_MAJOR == 18) && (NEXUS_PLATFORM_VERSION_MINOR >= 2) ) \
    )

#define NEXUS_HDCPVERSION_SUPPORTED
#define NEXUS_HDR_SUPPORTED

#endif


namespace WPEFramework {
namespace Plugin {

class DisplayInfoImplementation : public Exchange::IHDRProperties, public Exchange::IGraphicsProperties, public Exchange::IConnectionProperties {
public:
    DisplayInfoImplementation()
       : _width(0)
       , _height(0)
       , _verticalFreq(0)
       , _connected(false)
       , _hdcpprotection(HDCPProtectionType::HDCP_Unencrypted)
       , _type(HDR_OFF)
       , _totalGpuRam(0)
       , _audioPassthrough(false)
       , _EDID()
       , _adminLock()
       , _activity(*this) {

        VARIABLE_IS_NOT_USED NEXUS_Error rc = NxClient_Join(NULL);
        ASSERT(!rc);
        NxClient_UnregisterAcknowledgeStandby(NxClient_RegisterAcknowledgeStandby());
        NEXUS_Platform_GetConfiguration(&_platformConfig);

        UpdateTotalGpuRam(_totalGpuRam);

        NexusHdmiOutput hdmihandle;
        if( hdmihandle ) {
            UpdateDisplayInfoConnected(hdmihandle, _connected, _width, _height, _verticalFreq, _type);
            UpdateDisplayInfoHDCP(hdmihandle, _hdcpprotection);
            RetrieveEDID(hdmihandle, _EDID);
        }

        UpdateAudioPassthrough(_audioPassthrough);

        RegisterCallback();
    }

    DisplayInfoImplementation(const DisplayInfoImplementation&) = delete;
    DisplayInfoImplementation& operator= (const DisplayInfoImplementation&) = delete;
    virtual ~DisplayInfoImplementation()
    {
        NxClient_StopCallbackThread();
        NxClient_Uninit();
    }

public:
    // Graphics Properties interface
    uint32_t TotalGpuRam(uint64_t& total) const override
    {
        total = _totalGpuRam;
        return (Core::ERROR_NONE);
    }
    uint32_t FreeGpuRam(uint64_t& free) const override
    {
        uint64_t freeRam = 0;
        NEXUS_MemoryStatus status;

        NEXUS_Error rc = NEXUS_UNKNOWN;
#if NEXUS_MEMC0_GRAPHICS_HEAP
        if (_platformConfig.heap[NEXUS_MEMC0_GRAPHICS_HEAP]) {
            rc = NEXUS_Heap_GetStatus(_platformConfig.heap[NEXUS_MEMC0_GRAPHICS_HEAP], &status);
            if (rc == NEXUS_SUCCESS) {
                freeRam = static_cast<uint64_t>(status.free);
            }
        }
#endif
#if NEXUS_MEMC1_GRAPHICS_HEAP
        if (_platformConfig.heap[NEXUS_MEMC1_GRAPHICS_HEAP]) {
            rc = NEXUS_Heap_GetStatus(_platformConfig.heap[NEXUS_MEMC1_GRAPHICS_HEAP], &status);
            if (rc == NEXUS_SUCCESS) {
                freeRam += static_cast<uint64_t>(status.free);
            }
        }
#endif
#if NEXUS_MEMC2_GRAPHICS_HEAP
        if (_platformConfig.heap[NEXUS_MEMC2_GRAPHICS_HEAP]) {
            rc = NEXUS_Heap_GetStatus(_platformConfig.heap[NEXUS_MEMC2_GRAPHICS_HEAP], &status);
            if (rc == NEXUS_SUCCESS) {
                freeRam  += static_cast<uint64_t>(status.free);
            }
        }
#endif
        free = freeRam;
        return (Core::ERROR_NONE);
    }

    // Connection Properties interface
    uint32_t Register(INotification* notification) override
    {
        _adminLock.Lock();

        // Make sure a sink is not registered multiple times.
        ASSERT(std::find(_observers.begin(), _observers.end(), notification) == _observers.end());

        _observers.push_back(notification);
        notification->AddRef();

        _adminLock.Unlock();

        return (Core::ERROR_NONE);
    }
    uint32_t Unregister(INotification* notification) override
    {
        _adminLock.Lock();

        std::list<IConnectionProperties::INotification*>::iterator index(std::find(_observers.begin(), _observers.end(), notification));

        // Make sure you do not unregister something you did not register !!!
        ASSERT(index != _observers.end());

        if (index != _observers.end()) {
            (*index)->Release();
            _observers.erase(index);
        }

        _adminLock.Unlock();

        return (Core::ERROR_NONE);
    }
    uint32_t IsAudioPassthrough (bool& passthru) const override
    {
        passthru = _audioPassthrough;
        return (Core::ERROR_NONE);
    }
    uint32_t Connected(bool& isconnected) const override
    {
        isconnected = _connected;
        return (Core::ERROR_NONE);
    }
    uint32_t Width(uint32_t& width) const override
    {
        width = _width;
        return (Core::ERROR_NONE);
    }
    uint32_t Height(uint32_t& height) const override
    {
        height = _height;
        return (Core::ERROR_NONE);
    }
    uint32_t VerticalFreq(uint32_t& vf) const override
    {
        vf = _verticalFreq;
        return (Core::ERROR_NONE);
    }
    uint32_t HDCPProtection(HDCPProtectionType& value) const override
    {
        value = _hdcpprotection;
        return (Core::ERROR_NONE);
    }
    uint32_t HDCPProtection(const HDCPProtectionType value) override
    {
        _hdcpprotection = value;
        return (Core::ERROR_NONE);
    }
    uint32_t EDID (uint16_t& length, uint8_t data[]) const override
    {
        length = _EDID.Raw(length, data);
        return length ? (Core::ERROR_NONE) : Core::ERROR_UNAVAILABLE;
    }
    uint32_t WidthInCentimeters(uint8_t& width) const override
    {
        width = _EDID.WidthInCentimeters();
        return width ? (Core::ERROR_NONE) : Core::ERROR_UNAVAILABLE;
    }
    uint32_t HeightInCentimeters(uint8_t& height) const override
    {
        height = _EDID.HeightInCentimeters();
        return height ? (Core::ERROR_NONE) : Core::ERROR_UNAVAILABLE;
    }
    uint32_t PortName(string& name) const override
    {
        return (Core::ERROR_UNAVAILABLE);
    }
    uint32_t TVCapabilities(IHDRIterator*& type) const override
    {
        return (Core::ERROR_UNAVAILABLE);
    }
    uint32_t STBCapabilities(IHDRIterator*& type) const override
    {
        return (Core::ERROR_UNAVAILABLE);
    }
    uint32_t HDRSetting(HDRType& type) const override
    {
        type = _type;
        return (Core::ERROR_NONE);
    }

    void Dispatch() const
    {
        // To be handled based on events
    }

    BEGIN_INTERFACE_MAP(DisplayInfoImplementation)
        INTERFACE_ENTRY(Exchange::IHDRProperties)
        INTERFACE_ENTRY(Exchange::IGraphicsProperties)
        INTERFACE_ENTRY(Exchange::IConnectionProperties)
    END_INTERFACE_MAP

private:
    inline void UpdateTotalGpuRam(uint64_t& totalRam) const
    {
        NEXUS_MemoryStatus status;
        NEXUS_Error rc = NEXUS_UNKNOWN;

#if NEXUS_MEMC0_GRAPHICS_HEAP
        if (_platformConfig.heap[NEXUS_MEMC0_GRAPHICS_HEAP]) {
            rc = NEXUS_Heap_GetStatus(_platformConfig.heap[NEXUS_MEMC0_GRAPHICS_HEAP], &status);
            if (rc == NEXUS_SUCCESS) {
                totalRam = static_cast<uint64_t>(status.size);
            }
        }
#endif
#if NEXUS_MEMC1_GRAPHICS_HEAP
        if (_platformConfig.heap[NEXUS_MEMC1_GRAPHICS_HEAP]) {
            rc = NEXUS_Heap_GetStatus(_platformConfig.heap[NEXUS_MEMC1_GRAPHICS_HEAP], &status);
            if (rc == NEXUS_SUCCESS) {
                totalRam += static_cast<uint64_t>(status.size);
            }
        }
#endif
#if NEXUS_MEMC2_GRAPHICS_HEAP
        if (_platformConfig.heap[NEXUS_MEMC2_GRAPHICS_HEAP]) {
            rc = NEXUS_Heap_GetStatus(_platformConfig.heap[NEXUS_MEMC2_GRAPHICS_HEAP], &status);
            if (rc == NEXUS_SUCCESS) {
                totalRam += static_cast<uint64_t>(status.size);
            }
        }
#endif
    }

    inline void UpdateAudioPassthrough(bool& audioPassthrough)
    {
        NxClient_AudioStatus status;
        NEXUS_Error rc = NEXUS_UNKNOWN;
        rc = NxClient_GetAudioStatus(&status);
        if (rc == NEXUS_SUCCESS) {
            if ((status.hdmi.outputMode != NxClient_AudioOutputMode_eNone) && (status.hdmi.outputMode < NxClient_AudioOutputMode_eMax)) {
                audioPassthrough = true;
            }
        }
    }

    static string NEXUSHdmiOutputHdcpStateToString(const NEXUS_HdmiOutputHdcpState state) {

        struct HdmiOutputHdcpStateStrings {
            NEXUS_HdmiOutputHdcpState state;
            const char* strValue;
        };

        static const HdmiOutputHdcpStateStrings StateToStringTable[] = {
                                                {NEXUS_HdmiOutputHdcpState_eUnpowered, _T("Unpowered")},
                                                {NEXUS_HdmiOutputHdcpState_eUnauthenticated, _T("Unauthenticated")},
                                                {NEXUS_HdmiOutputHdcpState_eWaitForValidVideo, _T("WaitForValidVideo")},
                                                {NEXUS_HdmiOutputHdcpState_eInitializedAuthentication, _T("InitializedAuthentication")},
                                                {NEXUS_HdmiOutputHdcpState_eWaitForReceiverAuthentication, _T("WaitForReceiverAuthentication")},
                                                {NEXUS_HdmiOutputHdcpState_eReceiverR0Ready, _T("ReceiverR0Ready")},
                                                {NEXUS_HdmiOutputHdcpState_eR0LinkFailure, _T("R0LinkFailure")},
                                                {NEXUS_HdmiOutputHdcpState_eReceiverAuthenticated, _T("ReceiverAuthenticated")},
                                                {NEXUS_HdmiOutputHdcpState_eWaitForRepeaterReady, _T("WaitForRepeaterReady")},
                                                {NEXUS_HdmiOutputHdcpState_eCheckForRepeaterReady, _T("CheckForRepeaterReady")},
                                                {NEXUS_HdmiOutputHdcpState_eRepeaterReady, _T("RepeaterReady")},
                                                {NEXUS_HdmiOutputHdcpState_eLinkAuthenticated, _T("LinkAuthenticated")},
                                                {NEXUS_HdmiOutputHdcpState_eEncryptionEnabled, _T("EncryptionEnabled")},
                                                {NEXUS_HdmiOutputHdcpState_eRepeaterAuthenticationFailure, _T("RepeaterAuthenticationFailure")},
                                                {NEXUS_HdmiOutputHdcpState_eRiLinkIntegrityFailure, _T("RiLinkIntegrityFailure")},
                                                {NEXUS_HdmiOutputHdcpState_ePjLinkIntegrityFailure, _T("PjLinkIntegrityFailure")},
                                              };
        static const HdmiOutputHdcpStateStrings* end = {StateToStringTable + (sizeof(StateToStringTable)/sizeof(HdmiOutputHdcpStateStrings))};
        const HdmiOutputHdcpStateStrings * it = std::find_if(StateToStringTable, end,
                                                           [state](const HdmiOutputHdcpStateStrings& item){
                                                                return item.state == state;
                                                            });

        string result;

        if( it != end ) {
            result = it->strValue;
        } else {
            result = _T("Unknown(") + Core::NumberType<std::underlying_type<NEXUS_HdmiOutputHdcpState>::type>(state).Text() + _T(")");
        }

        return result;
    }

    static string NEXUSHdmiOutputHdcpErrorToString(const NEXUS_HdmiOutputHdcpError error) {

        struct HdmiOutputHdcpErrorStrings {
            NEXUS_HdmiOutputHdcpError error;
            const char* strValue;
        };

        static const HdmiOutputHdcpErrorStrings ErrorToStringTable[] = {
                                                {NEXUS_HdmiOutputHdcpError_eSuccess, _T("Success")},
                                                {NEXUS_HdmiOutputHdcpError_eRxBksvError, _T("RxBksvError")},
                                                {NEXUS_HdmiOutputHdcpError_eRxBksvRevoked, _T("RxBksvRevoked")},
                                                {NEXUS_HdmiOutputHdcpError_eRxBksvI2cReadError, _T("RxBksvI2cReadError")},
                                                {NEXUS_HdmiOutputHdcpError_eTxAksvError, _T("TxAksvError")},
                                                {NEXUS_HdmiOutputHdcpError_eTxAksvI2cWriteError, _T("TxAksvI2cWriteError")},
                                                {NEXUS_HdmiOutputHdcpError_eReceiverAuthenticationError, _T("ReceiverAuthenticationError")},
                                                {NEXUS_HdmiOutputHdcpError_eRepeaterAuthenticationError, _T("RepeaterAuthenticationError")},
                                                {NEXUS_HdmiOutputHdcpError_eRxDevicesExceeded, _T("RxDevicesExceeded")},
                                                {NEXUS_HdmiOutputHdcpError_eRepeaterDepthExceeded, _T("RepeaterDepthExceeded")},
                                                {NEXUS_HdmiOutputHdcpError_eRepeaterFifoNotReady, _T("RepeaterFifoNotReady")},
                                                {NEXUS_HdmiOutputHdcpError_eRepeaterDeviceCount0, _T("RepeaterDeviceCount")},
                                                {NEXUS_HdmiOutputHdcpError_eRepeaterLinkFailure, _T("RepeaterLinkFailure")},
                                                {NEXUS_HdmiOutputHdcpError_eLinkRiFailure, _T("LinkRiFailure")},
                                                {NEXUS_HdmiOutputHdcpError_eLinkPjFailure, _T("LinkPjFailure")},
                                                {NEXUS_HdmiOutputHdcpError_eFifoUnderflow, _T("FifoUnderflow")},
                                                {NEXUS_HdmiOutputHdcpError_eFifoOverflow, _T("FifoOverflow")},
                                                {NEXUS_HdmiOutputHdcpError_eMultipleAnRequest, _T("MultipleAnRequest")},
                                              };
        static const HdmiOutputHdcpErrorStrings* end = {ErrorToStringTable + (sizeof(ErrorToStringTable)/sizeof(HdmiOutputHdcpErrorStrings))};
        const HdmiOutputHdcpErrorStrings * it = std::find_if(ErrorToStringTable, end,
                                                           [error](const HdmiOutputHdcpErrorStrings& item){
                                                                return item.error == error;
                                                            });

        string result;

        if( it != end ) {
            result = it->strValue;
        } else {
            result = _T("Unknown(") + Core::NumberType<std::underlying_type<NEXUS_HdmiOutputHdcpError>::type>(error).Text() + _T(")");
        }

        return result;
    }

#ifdef NEXUS_HDCPVERSION_SUPPORTED

    static string NEXUSHdcpVersionToString(const NEXUS_HdcpVersion version) {
        string result;
        if( version == NEXUS_HdcpVersion_e1x ) {
            result = _T("1x");
        }
        else if( version == NEXUS_HdcpVersion_e2x ) {
            result = _T("2x");
        }
        else {
            result = _T("Unknown(") + Core::NumberType<std::underlying_type<NEXUS_HdcpVersion>::type>(version).Text() + _T(")");
        }
        return result;
    }

#endif

    class NexusHdmiOutput {
    public:
        NexusHdmiOutput(const NexusHdmiOutput&) = delete;
        NexusHdmiOutput& operator=(const NexusHdmiOutput&) = delete;

        NexusHdmiOutput(const uint8_t hdmiPort = 0) : _hdmiOutput(nullptr) {

            _hdmiOutput = NEXUS_HdmiOutput_Open(NEXUS_ALIAS_ID + hdmiPort, NULL);

            if( _hdmiOutput == nullptr ) {
                TRACE(Trace::Error, (_T("Error opening Nexus HDMI ouput")));
            }
        }

        ~NexusHdmiOutput() {
            if( _hdmiOutput != nullptr ) {
                NEXUS_HdmiOutput_Close(_hdmiOutput);
            }
        }

        operator bool() const {
            return (_hdmiOutput != nullptr);
        }

        operator NEXUS_HdmiOutputHandle() const {
            return _hdmiOutput;
        }

        private:
            NEXUS_HdmiOutputHandle _hdmiOutput;
    };

    void UpdateDisplayInfoConnected(const NEXUS_HdmiOutputHandle& hdmiOutput,
                                    bool& connected, uint32_t& width, uint32_t& height,
                                    uint32_t& verticalFreq, HDRType& hdr) const
    {
        NEXUS_HdmiOutputStatus status;
        NEXUS_Error rc = NEXUS_HdmiOutput_GetStatus(hdmiOutput, &status);
        if (rc == NEXUS_SUCCESS) {
            connected = status.connected;
            if (connected == true) {
                NxClient_DisplaySettings displaySettings;
                NxClient_GetDisplaySettings(&displaySettings);

                NxClient_DisplayStatus status;
                NEXUS_Error rcStatus = NxClient_GetDisplayStatus(&status);
                if(rcStatus != NEXUS_SUCCESS){
                    TRACE_L1(_T("Failed to get display status with rc=%d", rcStatus));
                }

#ifdef NEXUS_HDR_SUPPORTED
                // Read HDR status
                switch (status.hdmi.dynamicRangeMode) {
                case NEXUS_VideoDynamicRangeMode_eHdr10:
                    hdr = HDR_10;
                    break;
                case NEXUS_VideoDynamicRangeMode_eHdr10Plus:
                    hdr = HDR_10PLUS;
                    break;
                default:
                    hdr = HDR_OFF;
                    break;
                }
#else
                switch (displaySettings.hdmiPreferences.drmInfoFrame.eotf) {
                case NEXUS_VideoEotf_eHdr10:
                    hdr = HDR_10;
                    break;
                default:
                    hdr = HDR_OFF;
                    break;
                }
#endif

                NEXUS_VideoFormatInfo videoFormatInfo;
                NEXUS_VideoFormat_GetInfo(displaySettings.format, &videoFormatInfo);
                width = videoFormatInfo.width;
                height = videoFormatInfo.height;
                verticalFreq = videoFormatInfo.verticalFreq;
            } else {
                width = 0;
                height = 0;
                verticalFreq = 0;
                hdr = HDR_OFF;
            }
        }

        TRACE(Trace::Information, (_T("Display change: %s - %ix%i@%ihz, HDR:%i\n"),
                                  (connected? _T("connected"):_T("disconnected")),
                                  width, height, verticalFreq, hdr));
    }

    void UpdateDisplayInfoHDCP(const NEXUS_HdmiOutputHandle& hdmiOutput, HDCPProtectionType& hdcpprotection) const
    {
        // Check HDCP version
        NEXUS_HdmiOutputHdcpStatus hdcpStatus;
        NEXUS_Error rc = NEXUS_HdmiOutput_GetHdcpStatus(hdmiOutput, &hdcpStatus);

        if (rc  == NEXUS_SUCCESS) {
             TRACE(Trace::Information, (_T(" HDCP Error=[%s]")
                                        _T(" TransmittingEncrypted=[%s]")
                                        _T(" HDCP2.2Features=[%s]")
#ifdef NEXUS_HDCPVERSION_SUPPORTED
                                        _T(" SelectedHDCPVersion=[%s]")
#endif
                                        , NEXUSHdmiOutputHdcpErrorToString(hdcpStatus.hdcpError).c_str()
                                        , hdcpStatus.transmittingEncrypted ? _T("true") : _T("false")
                                        , hdcpStatus.hdcp2_2Features ? _T("true") : _T("false")
#ifdef NEXUS_HDCPVERSION_SUPPORTED
                                        , NEXUSHdcpVersionToString(hdcpStatus.selectedHdcpVersion).c_str()
#endif
                                    ) );

            TRACE(HDCPDetailedInfo,
                                       (_T("HDCP State=[%s]")
                                        _T(" ReadyForEncryption=[%s]")
                                        _T(" HDCP1.1Features=[%s]")
                                        _T(" 1.xDeviceDownstream=[%s]")
#ifdef NEXUS_HDCPVERSION_SUPPORTED
                                        _T(" MaxHDCPVersion=[%s]")
#endif
                                        , NEXUSHdmiOutputHdcpStateToString(hdcpStatus.hdcpState).c_str()
                                        , hdcpStatus.linkReadyForEncryption ? _T("true") : _T("false")
                                        , hdcpStatus.hdcp1_1Features ? _T("true") : _T("false")
                                        , hdcpStatus.hdcp2_2RxInfo.hdcp1_xDeviceDownstream ? _T("true") : _T("false")
#ifdef NEXUS_HDCPVERSION_SUPPORTED
                                        , NEXUSHdcpVersionToString(hdcpStatus.rxMaxHdcpVersion).c_str()
#endif
                                    ) );


            if(  hdcpStatus.transmittingEncrypted == false ) {
                hdcpprotection = HDCPProtectionType::HDCP_Unencrypted;
            }  else {

#ifdef NEXUS_HDCPVERSION_SUPPORTED
                if (hdcpStatus.selectedHdcpVersion == NEXUS_HdcpVersion_e2x) {
#else
                if (hdcpStatus.hdcp2_2Features == true) {
#endif
                    hdcpprotection = HDCPProtectionType::HDCP_2X;

                } else {
                    hdcpprotection = HDCPProtectionType::HDCP_1X;
                }
            }
        } else {
            TRACE(Trace::Error, (_T("Error retrieving HDCP status")));
        }
    }

    enum class CallbackType : int { HotPlug, DisplaySettings, HDCP };

    void RegisterCallback()
    {
        NxClient_CallbackThreadSettings settings;
        NxClient_GetDefaultCallbackThreadSettings(&settings);

        settings.hdmiOutputHotplug.callback = Callback;
        settings.hdmiOutputHotplug.context = reinterpret_cast<void*>(this);
        settings.hdmiOutputHotplug.param = static_cast<int>(CallbackType::HotPlug);

        settings.displaySettingsChanged.callback = Callback;
        settings.displaySettingsChanged.context = reinterpret_cast<void*>(this);
        settings.displaySettingsChanged.param = static_cast<int>(CallbackType::DisplaySettings);

        settings.hdmiOutputHdcpChanged.callback = Callback;
        settings.hdmiOutputHdcpChanged.context = reinterpret_cast<void*>(this);
        settings.hdmiOutputHdcpChanged.param = static_cast<int>(CallbackType::HDCP);

        if (NxClient_StartCallbackThread(&settings) != NEXUS_SUCCESS) {
            TRACE_L1(_T("Error in starting nexus callback thread"));
        }
    }

    static void Callback(void *cbData, int param)
    {
        DisplayInfoImplementation* platform = static_cast<DisplayInfoImplementation*>(cbData);

        ASSERT(platform != nullptr);

        if( platform != nullptr ) {
            platform->UpdateDisplayInfo(static_cast<CallbackType>(param));
        }
    }

    void UpdateDisplayInfo(const CallbackType callbacktype)
    {
        bool notify = false;
        bool connected = false;

        _adminLock.Lock();
        NexusHdmiOutput hdmiHandle;
        if (hdmiHandle) {
            switch ( callbacktype ) {
            case CallbackType::HDCP:  // HDCP settings changed
                if (_connected == true) {
                    UpdateDisplayInfoHDCP(hdmiHandle, _hdcpprotection);
                    notify = true;
                }
                /* fall-through! */
            case CallbackType::HotPlug:
            case CallbackType::DisplaySettings:
                UpdateDisplayInfoConnected(hdmiHandle, connected, _width, _height, _verticalFreq, _type);
                if (connected == false) {
                    _hdcpprotection = HDCP_Unencrypted;
                    notify = _connected; // don't bother with notifying disconnected->disconnected
                } else {
                    notify = true;
                }
                _connected = connected;
                break;
            default:
                break;
            }
            if (connected == true) {
                RetrieveEDID(hdmiHandle, _EDID);
            } else {
                _EDID.Clear();
            }
        }
        _adminLock.Unlock();

        if (notify == true) {
            _activity.Submit();
        }
    }

    // rc = BHDM_EDID_GetHdrStaticMetadatadb(hdmiOutput->hdmHandle, &_hdrdb);
    void RetrieveEDID(NEXUS_HdmiOutputHandle handle, ExtendedDisplayIdentification& info) {
        // typedef struct NEXUS_HdmiOutputEdidBlock
        // {
        //     uint8_t data[128];
        // } NEXUS_HdmiOutputEdidBlock;

        // NEXUS_Error NEXUS_HdmiOutput_GetEdidBlock(
        //      NEXUS_HdmiOutputHandle output,
        //      unsigned blockNum,
        //      NEXUS_HdmiOutputEdidBlock *pBlock    /* [out] Block of raw EDID data */
        //      );
        NEXUS_Error error;
        uint8_t index = 0;

        do {
            error = NEXUS_HdmiOutput_GetEdidBlock(handle, index, reinterpret_cast<NEXUS_HdmiOutputEdidBlock*>(info.Segment(index)));

            index++;

        } while ( (index <= info.Segments()) && (error == 0) );
    }


private:
    uint32_t _width;
    uint32_t _height;
    uint32_t _verticalFreq;
    bool _connected;

    HDCPProtectionType _hdcpprotection;
    HDRType _type;

    uint64_t _totalGpuRam;
    bool _audioPassthrough;

    std::list<IConnectionProperties::INotification*> _observers;

    NEXUS_PlatformConfiguration _platformConfig;
    ExtendedDisplayIdentification _EDID;

    mutable Core::CriticalSection _adminLock;

    Core::WorkerPool::JobType<DisplayInfoImplementation&> _activity;
};

    SERVICE_REGISTRATION(DisplayInfoImplementation, 1, 0);
}
}
