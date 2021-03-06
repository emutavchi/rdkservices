/**
* If not stated otherwise in this file or this component's LICENSE
* file the following copyright and licenses apply:
*
* Copyright 2019 RDK Management
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
**/

#include "HdmiCec.h"


#include "ccec/Connection.hpp"
#include "ccec/CECFrame.hpp"
#include "host.hpp"
#include "ccec/host/RDK.hpp"

#include "ccec/drivers/iarmbus/CecIARMBusMgr.h"


#include "dsMgr.h"
#include "dsDisplay.h"
#include "videoOutputPort.hpp"

#include "websocket/URL.h"

#include "utils.h"

#define HDMICEC_METHOD_SET_ENABLED "setEnabled"
#define HDMICEC_METHOD_GET_ENABLED "getEnabled"
#define HDMICEC_METHOD_GET_CEC_ADDRESSES "getCECAddresses"
#define HDMICEC_METHOD_SEND_MESSAGE "sendMessage"

#define HDMICEC_EVENT_ON_DEVICES_CHANGED "onDevicesChanged"
#define HDMICEC_EVENT_ON_MESSAGE "onMessage"
#define HDMICEC_EVENT_ON_HDMI_HOT_PLUG "onHdmiHotPlug"
#define HDMICEC_EVENT_ON_CEC_ADDRESS_CHANGE "cecAddressesChanged"

#define PHYSICAL_ADDR_CHANGED 1
#define LOGICAL_ADDR_CHANGED 2
#define DEV_TYPE_TUNER 1

#define HDMI_HOT_PLUG_EVENT_CONNECTED 0

#if defined(HAS_PERSISTENT_IN_HDD)
#define CEC_SETTING_ENABLED_FILE "/tmp/mnt/diska3/persistent/ds/cecData.json"
#elif defined(HAS_PERSISTENT_IN_FLASH)
#define CEC_SETTING_ENABLED_FILE "/opt/persistent/ds/cecData.json"
#else
#define CEC_SETTING_ENABLED_FILE "/opt/ds/cecData.json"
#endif

#define CEC_SETTING_ENABLED "cecEnabled"

namespace WPEFramework
{
    namespace Plugin
    {
        SERVICE_REGISTRATION(HdmiCec, 1, 0);

        HdmiCec* HdmiCec::_instance = nullptr;

        static int libcecInitStatus = 0;

        HdmiCec::HdmiCec()
        : AbstractPlugin()
        {
            LOGINFO();
            HdmiCec::_instance = this;

            InitializeIARM();

            registerMethod(HDMICEC_METHOD_SET_ENABLED, &HdmiCec::setEnabledWrapper, this);
            registerMethod(HDMICEC_METHOD_GET_ENABLED, &HdmiCec::getEnabledWrapper, this);
            registerMethod(HDMICEC_METHOD_GET_CEC_ADDRESSES, &HdmiCec::getCECAddressesWrapper, this);
            registerMethod(HDMICEC_METHOD_SEND_MESSAGE, &HdmiCec::sendMessageWrapper, this);

            physicalAddress = 0x0F0F0F0F;

            logicalAddressDeviceType = "None";
            logicalAddress = 0xFF;

            loadSettings();
            if (cecSettingEnabled)
            {
                setEnabled(cecSettingEnabled);
            }
            else
            {
                setEnabled(false);
                persistSettings(false);
            }
        }

        HdmiCec::~HdmiCec()
        {
            LOGINFO();
            HdmiCec::_instance = nullptr;

            DeinitializeIARM();

        }

        const void HdmiCec::InitializeIARM()
        {
            LOGINFO();

            int isRegistered;
            IARM_Result_t res = IARM_Bus_IsConnected("HdmiCec" , &isRegistered);
            if(res != IARM_RESULT_SUCCESS)
            {
                IARM_CHECK( IARM_Bus_Init("HdmiCec") );
                IARM_CHECK( IARM_Bus_Connect() );
                m_iarmConnected = true;
            }

            //IARM_CHECK( IARM_Bus_RegisterEventHandler(IARM_BUS_CECHOST_NAME, IARM_BUS_CECHost_EVENT_DEVICESTATUSCHANGE,cecDeviceStatusEventHandler) ); // It didn't do anything in original service
            IARM_CHECK( IARM_Bus_RegisterEventHandler(IARM_BUS_CECMGR_NAME, IARM_BUS_CECMGR_EVENT_DAEMON_INITIALIZED,cecMgrEventHandler) );
            IARM_CHECK( IARM_Bus_RegisterEventHandler(IARM_BUS_CECMGR_NAME, IARM_BUS_CECMGR_EVENT_STATUS_UPDATED,cecMgrEventHandler) );
            IARM_CHECK( IARM_Bus_RegisterEventHandler(IARM_BUS_DSMGR_NAME,IARM_BUS_DSMGR_EVENT_HDMI_HOTPLUG, dsHdmiEventHandler) );
        }

        //TODO(MROLLINS) - we need to install crash handler to ensure DeinitializeIARM gets called
        void HdmiCec::DeinitializeIARM()
        {
            LOGINFO();

            if (m_iarmConnected)
            {
                m_iarmConnected = false;
                IARM_Result_t res;

                //IARM_CHECK( IARM_Bus_UnRegisterEventHandler(IARM_BUS_CECHOST_NAME, IARM_BUS_CECHost_EVENT_DEVICESTATUSCHANGE) );
                IARM_CHECK( IARM_Bus_UnRegisterEventHandler(IARM_BUS_CECMGR_NAME, IARM_BUS_CECMGR_EVENT_DAEMON_INITIALIZED) );
                IARM_CHECK( IARM_Bus_UnRegisterEventHandler(IARM_BUS_CECMGR_NAME, IARM_BUS_CECMGR_EVENT_STATUS_UPDATED) );
                IARM_CHECK( IARM_Bus_UnRegisterEventHandler(IARM_BUS_DSMGR_NAME,IARM_BUS_DSMGR_EVENT_HDMI_HOTPLUG) );

                IARM_CHECK( IARM_Bus_Disconnect() );
                IARM_CHECK( IARM_Bus_Term() );
            }
        }

        void HdmiCec::cecMgrEventHandler(const char *owner, IARM_EventId_t eventId, void *data, size_t len)
        {
            LOGINFO();

            if(!HdmiCec::_instance)
                return;

            if( !strcmp(owner, IARM_BUS_CECMGR_NAME))
            {
                switch (eventId)
                {
                    case IARM_BUS_CECMGR_EVENT_DAEMON_INITIALIZED:
                    {
                        HdmiCec::_instance->onCECDaemonInit();
                    }
                    break;
                    case IARM_BUS_CECMGR_EVENT_STATUS_UPDATED:
                    {
                        IARM_Bus_CECMgr_Status_Updated_Param_t *evtData = new IARM_Bus_CECMgr_Status_Updated_Param_t;
                        if(evtData)
                        {
                            memcpy(evtData,data,sizeof(IARM_Bus_CECMgr_Status_Updated_Param_t));
                            HdmiCec::_instance->cecStatusUpdated(evtData);
                        }
                    }
                    break;
                    default:
                    /*Do nothing*/
                    break;
                }
            }
        }

        void HdmiCec::dsHdmiEventHandler(const char *owner, IARM_EventId_t eventId, void *data, size_t len)
        {
            LOGINFO();

            if(!HdmiCec::_instance)
                return;

            if (IARM_BUS_DSMGR_EVENT_HDMI_HOTPLUG == eventId)
            {
                IARM_Bus_DSMgr_EventData_t *eventData = (IARM_Bus_DSMgr_EventData_t *)data;
                int hdmi_hotplug_event = eventData->data.hdmi_hpd.event;
                LOGINFO("Received IARM_BUS_DSMGR_EVENT_HDMI_HOTPLUG  event data:%d \r\n", hdmi_hotplug_event);

                HdmiCec::_instance->onHdmiHotPlug(hdmi_hotplug_event);
            }
        }

        void HdmiCec::onCECDaemonInit()
        {
            LOGINFO();

            if(true == getEnabled())
            {
                setEnabled(false);
                setEnabled(true);
            }
            else
            {
                /*Do nothing as CEC is not already enabled*/
            }
        }

        void HdmiCec::cecStatusUpdated(void *evtStatus)
        {
            LOGINFO();

            IARM_Bus_CECMgr_Status_Updated_Param_t *evtData = (IARM_Bus_CECMgr_Status_Updated_Param_t *)evtStatus;
            if(evtData)
            {
               try{
                    getPhysicalAddress();

                    unsigned int logicalAddr = evtData->logicalAddress;
                    std::string logicalAddrDeviceType = DeviceType(LogicalAddress(evtData->logicalAddress).getType()).toString().c_str();

                    LOGWARN("cecLogicalAddressUpdated: logical address updated: %d , saved : %d ", logicalAddr, logicalAddress);
                    if (logicalAddr != logicalAddress || logicalAddrDeviceType != logicalAddressDeviceType)
                    {
                        logicalAddress = logicalAddr;
                        logicalAddressDeviceType = logicalAddrDeviceType;
                        cecAddressesChanged(LOGICAL_ADDR_CHANGED);
                    }
                }
                catch (const std::exception e)
                {
                    LOGWARN("CEC exception caught from cecStatusUpdated");
                }

                delete evtData;
            }
           return;
        }

        void HdmiCec::onHdmiHotPlug(int connectStatus)
        {
            LOGINFO();

            if (HDMI_HOT_PLUG_EVENT_CONNECTED == connectStatus)
            {
                LOGWARN("onHdmiHotPlug Status : %d ", connectStatus);
                getPhysicalAddress();
                getLogicalAddress();
            }
            return;
        }

        uint32_t HdmiCec::setEnabledWrapper(const JsonObject& parameters, JsonObject& response)
        {
            LOGINFO();

            bool enabled = false;

            if (parameters.HasLabel("enabled"))
            {
                getBoolParameter("enabled", enabled);
            }
            else
            {
                returnResponse(false);
            }

            setEnabled(enabled);
            returnResponse(true);
        }

        uint32_t HdmiCec::getEnabledWrapper(const JsonObject& parameters, JsonObject& response)
        {
            LOGINFO();

            response["enabled"] = getEnabled();
            returnResponse(true);
        }

        uint32_t HdmiCec::getCECAddressesWrapper(const JsonObject& parameters, JsonObject& response)
        {
            LOGINFO();

            response["CECAddresses"] = getCECAddresses();

            returnResponse(true);
        }

        uint32_t HdmiCec::sendMessageWrapper(const JsonObject& parameters, JsonObject& response)
        {
            LOGINFO();

            std::string message;

            if (parameters.HasLabel("message"))
            {
                message = parameters["message"].String();
            }
            else
            {
                returnResponse(false);
            }

            sendMessage(message);
            returnResponse(true);
        }

        bool HdmiCec::loadSettings()
        {
            Core::File file;
            file = CEC_SETTING_ENABLED_FILE;

            file.Open();
            JsonObject parameters;
            parameters.IElement::FromFile(file);

            file.Close();

            getBoolParameter(CEC_SETTING_ENABLED, cecSettingEnabled);

            return cecSettingEnabled;
        }

        void HdmiCec::persistSettings(bool enableStatus)
        {
            Core::File file;
            file = CEC_SETTING_ENABLED_FILE;

            file.Open(false);
            if (!file.IsOpen())
                file.Create();

            JsonObject cecSetting;
            cecSetting[CEC_SETTING_ENABLED] = enableStatus;

            cecSetting.IElement::ToFile(file);

            file.Close();

            return;
        }

        void HdmiCec::setEnabled(bool enabled)
        {
           LOGWARN("Entered setEnabled ");

           if (cecSettingEnabled != enabled)
           {
               persistSettings(enabled);
           }
           if(true == enabled)
           {
               CECEnable();
           }
           else
           {
               CECDisable();
           }
           return;
        }

        void HdmiCec::CECEnable(void)
        {
            LOGWARN("Entered CECEnable");
            if (cecEnableStatus)
            {
                LOGWARN("CEC Already Enabled");
                return;
            }

            if(0 == libcecInitStatus)
            {
                try
                {
                    LibCCEC::getInstance().init();
                }
                catch (const std::exception e)
                {
                    LOGWARN("CEC exception caught from CECEnable");
                }
            }
            libcecInitStatus++;

            smConnection = new Connection(LogicalAddress::UNREGISTERED,false,"ServiceManager::Connection::");
            smConnection->open();
            smConnection->addFrameListener(this);

            //Acquire CEC Addresses
            getPhysicalAddress();
            getLogicalAddress();

            cecEnableStatus = true;
            return;
        }

        void HdmiCec::CECDisable(void)
        {
            LOGWARN("Entered CECDisable ");

            if(!cecEnableStatus)
            {
                LOGWARN("CEC Already Disabled ");
                return;
            }

            if (smConnection != NULL)
            {
                smConnection->close();
                delete smConnection;
                smConnection = NULL;
            }
            cecEnableStatus = false;

            if(1 == libcecInitStatus)
            {
                LibCCEC::getInstance().term();
            }

            libcecInitStatus--;

            return;
        }


        void HdmiCec::getPhysicalAddress()
        {
            LOGINFO("Entered getPhysicalAddress ");

            uint32_t physAddress = 0x0F0F0F0F;

            try {
                    LibCCEC::getInstance().getPhysicalAddress(&physAddress);

                    LOGINFO("getPhysicalAddress: physicalAddress: %x %x %x %x ", (physAddress >> 24) & 0xFF, (physAddress >> 16) & 0xFF, (physAddress >> 8)  & 0xFF, (physAddress) & 0xFF);
                    if (physAddress != physicalAddress)
                    {
                        physicalAddress = physAddress;
                        cecAddressesChanged(PHYSICAL_ADDR_CHANGED);
                    }
            }
            catch (const std::exception e)
            {
                LOGWARN("DS exception caught from getPhysicalAddress");
            }
            return;
        }

        void HdmiCec::getLogicalAddress()
        {
            LOGINFO("Entered getLogicalAddress ");

            try{
                int addr = LibCCEC::getInstance().getLogicalAddress(DEV_TYPE_TUNER);

                std::string logicalAddrDeviceType = DeviceType(LogicalAddress(addr).getType()).toString().c_str();

                LOGWARN("logical address obtained is %d , saved logical address is %d ", addr, logicalAddress);

                if ((int)logicalAddress != addr || logicalAddressDeviceType != logicalAddrDeviceType)

                {
                    logicalAddress = addr;
                    logicalAddressDeviceType = logicalAddrDeviceType;
                    cecAddressesChanged(LOGICAL_ADDR_CHANGED);
                }
            }
            catch (const std::exception e)
            {
                LOGWARN("CEC exception caught from getLogicalAddress ");
            }

            return;
        }

        bool HdmiCec::getEnabled()
        {
            LOGWARN("Entered getEnabled ");
            if(true == cecEnableStatus)
                return true;
            else
                return false;
        }

        void HdmiCec::setName(std::string name)
        {
            //SVCLOG_WARN("%s \r\n",__FUNCTION__);
            return;
        }

        std::string HdmiCec::getName()
        {
            //SVCLOG_WARN("%s \r\n",__FUNCTION__);
            IARM_Result_t ret = IARM_RESULT_INVALID_STATE;
            if (ret != IARM_RESULT_SUCCESS)
            {
                LOGWARN("getName :: IARM_BUS_CEC_HOST_GetOSDName failed ");
                return "STB";
            }

            return "STB";
        }

        JsonObject HdmiCec::getCECAddresses()
        {
            JsonObject CECAddress;
            LOGINFO("Entered getCECAddresses ");

            JsonArray pa;
            pa.Add((physicalAddress >> 24) & 0xff);
            pa.Add((physicalAddress >> 16) & 0xff);
            pa.Add((physicalAddress >> 8)  & 0xff);
            pa.Add( physicalAddress        & 0xff);

            CECAddress["physicalAddress"] = pa;

            JsonObject logical;
            logical["deviceType"] = logicalAddressDeviceType;
            logical["logicalAddress"] = logicalAddress;

            JsonArray logicalArray;
            logicalArray.Add(logical);

            CECAddress["logicalAddresses"] = logicalArray;
            LOGWARN("getCECAddresses: physicalAddress from QByteArray : %x %x %x %x ", (physicalAddress >> 24) & 0xFF, (physicalAddress >> 16) & 0xFF, (physicalAddress >> 8)  & 0xFF, (physicalAddress) & 0xFF);
            LOGWARN("getCECAddresses: logical address: %x  ", logicalAddress);

            return CECAddress;
        }

        void HdmiCec::sendMessage(std::string message)
        {
            LOGINFO("sendMessage ");

            if(true == cecEnableStatus)
            {
                std::vector <unsigned char> buf;
                buf.resize(message.size());

                uint16_t decodedLen = Core::URL::Base64Decode(message.c_str(), message.size(), (uint8_t*)buf.data(), buf.size());
                CECFrame frame = CECFrame((const uint8_t *)buf.data(), decodedLen);
        //      SVCLOG_WARN("Frame to be sent from servicemanager in %s \n",__FUNCTION__);
        //      frame.hexDump();
                smConnection->sendAsync(frame);
            }
            else
                LOGWARN("cecEnableStatus=false");
            return;
        }

        void HdmiCec::cecAddressesChanged(int changeStatus)
        {
            LOGINFO();

            JsonObject params;
            JsonObject CECAddresses;

            LOGWARN(" cecAddressesChanged Change Status : %d ", changeStatus);
            if(PHYSICAL_ADDR_CHANGED == changeStatus)
            {
                CECAddresses["physicalAddress"] = physicalAddress;
            }
            else if(LOGICAL_ADDR_CHANGED == changeStatus)
            {
                CECAddresses["logicalAddresses"] = logicalAddress;
            }
            else
            {
                //Do Nothing
            }

            params["CECAddresses"] = CECAddresses;
            LOGWARN(" cecAddressesChanged  send : %s ", HDMICEC_EVENT_ON_CEC_ADDRESS_CHANGE);

            sendNotify(HDMICEC_EVENT_ON_CEC_ADDRESS_CHANGE, params);

            return;
        }

        void HdmiCec::notify(const CECFrame &in) const
        {
            LOGINFO("Inside notify ");
            size_t length;
            const uint8_t *input_frameBuf = NULL;
            CECFrame Frame = in;
        //  SVCLOG_WARN("Frame received by servicemanager is \n");
        //  Frame.hexDump();
            Frame.getBuffer(&input_frameBuf,&length);

            std::vector <char> buf;
            buf.resize(length * 2);

            uint16_t encodedLen = Core::URL::Base64Encode(input_frameBuf, length, buf.data(), buf.size());
            buf[encodedLen] = 0;

            (const_cast<HdmiCec*>(this))->onMessage(buf.data());
            return;
        }

        void HdmiCec::onMessage( const char *message )
        {
            JsonObject params;
            params["message"] = message;
            sendNotify(HDMICEC_EVENT_ON_MESSAGE, params);
        }

    } // namespace Plugin
} // namespace WPEFramework



