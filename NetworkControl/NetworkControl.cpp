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

#include "NetworkControl.h"

namespace WPEFramework {

namespace Plugin
{

    SERVICE_REGISTRATION(NetworkControl, 1, 0);

    static Core::ProxyPoolType<Web::Response> responseFactory(4);
    static Core::ProxyPoolType<Web::JSONBodyType<NetworkControl::Entry>> jsonGetNetworkFactory(1);
    static Core::ProxyPoolType<Web::JSONBodyType<Core::JSON::ArrayType<NetworkControl::Entry>>> jsonGetNetworksFactory(1);
    static TCHAR NAMESERVER[] = "nameserver ";

    static bool ExternallyAccessible(const Core::AdapterIterator& index) {

        ASSERT (index.IsValid());

        bool accessible = index.IsRunning();

        if (accessible == true) {
            Core::IPV4AddressIterator checker4(index.IPV4Addresses());

            accessible = false;

            while ((accessible == false) && (checker4.Next() == true)) {
                accessible = (checker4.Address().IsLocalInterface() == false);
            }

            if (accessible == false) {
                Core::IPV6AddressIterator checker6(index.IPV6Addresses());

                while ((accessible == false) && (checker4.Next() == true)) {
                    accessible = (checker4.Address().IsLocalInterface() == false);
                }
            }
        }

        return (accessible);
    }

#ifdef __WINDOWS__
#pragma warning(disable : 4355)
#endif
    NetworkControl::NetworkControl()
        : _skipURL(0)
        , _service(nullptr)
        , _persistentStoragePath()
        , _responseTime(0)
        , _retries(0)
        , _dns()
        , _requiredSet()
        , _dhcpInterfaces()
        , _observer(*this)
        , _open(false)
    {
        RegisterAll();
    }
#ifdef __WINDOWS__
#pragma warning(default : 4355)
#endif

    /* virtual */ NetworkControl::~NetworkControl()
    {
        UnregisterAll();
    }

    /* virtual */ const string NetworkControl::Initialize(PluginHost::IShell * service)
    {
        string result;
        Config config;
        config.FromString(service->ConfigLine());

        _service = service;
        _skipURL = static_cast<uint8_t>(service->WebPrefix().length());
        _responseTime = config.TimeOut.Value();
        _retries = config.Retries.Value();
        _dnsFile = config.DNSFile.Value();

        // We will only "open" the DNS resolve file, so of ot does not exist yet, create an empty file.
        Core::File dnsFile(_dnsFile, true);
        if (dnsFile.Exists() == false) {
            if (dnsFile.Create() == false) {
                SYSLOG(Logging::Startup, (_T("Could not create DNS configuration file [%s]"), _dnsFile.c_str()));
            } else {
                dnsFile.Close();

                Core::JSON::ArrayType<Core::JSON::String>::Iterator entries(config.DNS.Elements());

                while (entries.Next() == true) {
                    Core::NodeId entry(entries.Current().Value().c_str());

                    if (entry.IsValid() == true) {
                        _dns.push_back(entry);
                    }
                }

                RefreshDNS();
            }
        }

        // Try to create persistent storage folder
        _persistentStoragePath = _service->PersistentPath();
        if (_persistentStoragePath.empty() == false) {
            if (Core::Directory(_persistentStoragePath.c_str()).CreatePath() == false) {
                _persistentStoragePath.clear();
                SYSLOG(Logging::Startup, (_T("Failed to create storage for network info: %s"), _persistentStoragePath.c_str()));
            }
            _persistentStoragePath += _T("Data.json");
        }

        Core::JSON::ArrayType<Core::JSON::String>::Iterator entries2(config.Set.Elements());
        while (entries2.Next() == true) {
            _requiredSet.push_back(entries2.Current().Value());
        }

        // Lets download leases for potential DHCP interfaces
        std::map<const string, const Entry> info;
        Load(_persistentStoragePath, info);

        Core::JSON::ArrayType<Entry>::Iterator index(config.Interfaces.Elements());

        // From now on we observer the states of the give interfaces.
        _observer.Open();

        // Load the configured interface with their settings..
        while (index.Next() == true) {
            if (index.Current().Interface.IsSet() == true) {
                string interfaceName(index.Current().Interface.Value());
                Core::AdapterIterator hardware(interfaceName);

                if (hardware.IsValid() == false) {
                    SYSLOG(Logging::Startup, (_T("Interface [%s], not available"), interfaceName.c_str()));
                } else {
                    JsonData::NetworkControl::NetworkData::ModeType how;
                    std::map<const string, const Entry>::const_iterator more (info.find(interfaceName));

                    if (more != info.end()) {
                        auto element = _dhcpInterfaces.emplace(std::piecewise_construct,
                            std::forward_as_tuple(interfaceName),
                            std::forward_as_tuple(*this, interfaceName, _responseTime, _retries, more->second));
                        how = (element.first->second.Info().Mode());
                    }
                    else {
                        auto element = _dhcpInterfaces.emplace(std::piecewise_construct,
                            std::forward_as_tuple(interfaceName),
                            std::forward_as_tuple(*this, interfaceName, _responseTime, _retries, index.Current()));
                        how = (element.first->second.Info().Mode());
                    }

                    if (hardware.IsUp() == false) {
                        hardware.Up(true);
                    }
                    else {
                        ClearIP(hardware);
                        Reload(interfaceName, (how == JsonData::NetworkControl::NetworkData::ModeType::DYNAMIC));
                    }
                }
            }
        }

        // If we are oke for loading all interfaces, load any interface not yet configured.
        if (config.Open.Value() == true) {
            // Find all adapters that are not listed.
            Core::AdapterIterator notListed;

            _open = true;

            while (notListed.Next() == true) {
                const string interfaceName(notListed.Name());

                if (_dhcpInterfaces.find(interfaceName) == _dhcpInterfaces.end()) {
                    _dhcpInterfaces.emplace(std::piecewise_construct,
                        std::forward_as_tuple(interfaceName),
                        std::forward_as_tuple(*this, interfaceName, _responseTime, _retries, Entry()));
                }
            }
        }

        // On success return empty, to indicate there is no error text.
        return (result);
    }

    /* virtual */ void NetworkControl::Deinitialize(PluginHost::IShell * service)
    {
        // Stop observing.
        _observer.Close();
        _dns.clear();
        _dhcpInterfaces.clear();
        _service = nullptr;
    }

    /* virtual */ string NetworkControl::Information() const
    {
        // No additional info to report.
        return (string());
    }

    /* virtual */ void NetworkControl::Inbound(Web::Request & /* request */)
    {
    }

    /* virtual */ Core::ProxyType<Web::Response>
    NetworkControl::Process(const Web::Request& request)
    {
        Core::ProxyType<Web::Response> result(PluginHost::IFactories::Instance().Response());
        Core::TextSegmentIterator index(Core::TextFragment(request.Path, _skipURL, static_cast<uint16_t>(request.Path.length()) - _skipURL), false, '/');

        // By default, we assume everything works..
        result->ErrorCode = Web::STATUS_BAD_REQUEST;
        result->Message = _T("Unsupported request for the [NetworkControl] service.");

        // By default, we skip the first slash
        index.Next();

        if (request.Verb == Web::Request::HTTP_GET) {

            _adminLock.Lock();

            if (index.Next() == true) {

                std::map<const string, DHCPEngine>::iterator entry(_dhcpInterfaces.find(index.Current().Text()));

                if (entry != _dhcpInterfaces.end()) {

                    Core::ProxyType<Web::JSONBodyType<NetworkControl::Entry>> data(jsonGetNetworkFactory.Element());

                    entry->second.Get(*data);

                    result->Body(data);
                    result->ErrorCode = Web::STATUS_OK;
                    result->Message = "OK";
                } else {
                    result->ErrorCode = Web::STATUS_NOT_FOUND;
                    result->Message = string(_T("Could not find interface: ")) + index.Current().Text();
                }
            } else {
                std::map<const string, DHCPEngine>::iterator entry(_dhcpInterfaces.begin());

                Core::ProxyType<Web::JSONBodyType<Core::JSON::ArrayType<NetworkControl::Entry>>> data(jsonGetNetworksFactory.Element());

                while (entry != _dhcpInterfaces.end()) {

                    Entry& newSlot = data->Add();

                    entry->second.Get(newSlot);
                    entry++;
                }
                result->Body(data);

                result->ErrorCode = Web::STATUS_OK;
                result->Message = "OK";
            }

            _adminLock.Unlock();
        } else if ((index.Next() == true) && (request.Verb == Web::Request::HTTP_PUT)) {

            _adminLock.Lock();

            std::map<const string, DHCPEngine>::iterator entry(_dhcpInterfaces.find(index.Current().Text()));

            if ((entry != _dhcpInterfaces.end()) && (index.Next() == true)) {

                bool reload = (index.Current() == _T("Reload"));

                if (((reload == true) && (entry->second.Info().Mode() != JsonData::NetworkControl::NetworkData::ModeType::STATIC)) || (index.Current() == _T("Request"))) {

                    if (Reload(entry->first, true) == Core::ERROR_NONE) {
                        result->ErrorCode = Web::STATUS_OK;
                        result->Message = "OK";
                    } else {
                        result->ErrorCode = Web::STATUS_INTERNAL_SERVER_ERROR;
                        result->Message = "Could not activate the server";
                    }
                } else if (((reload == true) && (entry->second.Info().Mode() == JsonData::NetworkControl::NetworkData::ModeType::STATIC)) || (index.Current() == _T("Assign"))) {

                    if (Reload(entry->first, false) == Core::ERROR_NONE) {
                        result->ErrorCode = Web::STATUS_OK;
                        result->Message = "OK";
                    } else {
                        result->ErrorCode = Web::STATUS_INTERNAL_SERVER_ERROR;
                        result->Message = "Could not activate the server";
                    }
                } else if (index.Current() == _T("Up")) {
                    string interfaceName(entry->first);
                    Core::AdapterIterator adapter(interfaceName);

                    if (adapter.IsValid() == false) {
                        result->ErrorCode = Web::STATUS_NOT_FOUND;
                        result->Message = string(_T("Interface: ")) + interfaceName + _T(" not found.");
                    } else {
                        adapter.Up(true);
                        result->ErrorCode = Web::STATUS_OK;
                        result->Message = string(_T("OK, ")) + interfaceName + _T(" set UP.");
                    }
                } else if (index.Current() == _T("Down")) {
                    string interfaceName(entry->first);
                    Core::AdapterIterator adapter(interfaceName);

                    if (adapter.IsValid() == false) {
                        result->ErrorCode = Web::STATUS_NOT_FOUND;
                        result->Message = string(_T("Interface: ")) + interfaceName + _T(" not found.");
                    } else {
                        adapter.Up(false);
                        result->ErrorCode = Web::STATUS_OK;
                        result->Message = string(_T("OK, ")) + interfaceName + _T(" set DOWN.");
                    }
                } else if (index.Current() == _T("Flush")) {
                    string interfaceName(entry->first);
                    Core::AdapterIterator adapter(interfaceName);

                    if (adapter.IsValid() == false) {
                        result->ErrorCode = Web::STATUS_NOT_FOUND;
                        result->Message = string(_T("Interface: ")) + interfaceName + _T(" not found.");
                    } else {
                        ClearIP(adapter);

                        result->ErrorCode = Web::STATUS_OK;
                        result->Message = string(_T("OK, ")) + interfaceName + _T(" set DOWN.");
                    }
                }
            }

            _adminLock.Unlock();
        }

        return result;
    }

    void NetworkControl::ClearIP(Core::AdapterIterator& adapter)
    {
        Core::IPV4AddressIterator checker4(adapter.IPV4Addresses());

        while (checker4.Next() == true) {
            adapter.Delete(checker4.Address());
        }

        Core::IPV6AddressIterator checker6(adapter.IPV6Addresses());

        while (checker6.Next() == true) {
            adapter.Delete(checker6.Address());
        }
 
        Core::AdapterIterator::Flush();
        ClearLease(adapter.Name());

        SubSystemValidation();
    }


    uint32_t NetworkControl::SetIP(Core::AdapterIterator& adapter, const Core::IPNode& ipAddress, const Core::NodeId& gateway, const Core::NodeId& broadcast, bool clearOld)
    {
        if (adapter.IsValid() == true) {
            bool addIt = true;

            if (ipAddress.Type() == Core::NodeId::TYPE_IPV4) {
                Core::IPV4AddressIterator checker(adapter.IPV4Addresses());
                while ((checker.Next() == true) && ((addIt == true) || (clearOld == true))) {   
                    if (checker.Address() == ipAddress) {
                        addIt = false;
                    }
                    else if (clearOld == true) {
                        adapter.Delete(checker.Address());
                    }
                    TRACE_L1("Validating IP: %s - %s [%s,%s]", checker.Address().HostAddress().c_str(), ipAddress.HostAddress().c_str(), addIt ? _T("true") : _T("false"), clearOld ? _T("true") : _T("false"));
                }
            } else if (ipAddress.Type() == Core::NodeId::TYPE_IPV6) {
                Core::IPV6AddressIterator checker(adapter.IPV6Addresses());
                while ((checker.Next() == true) && ((addIt == true) || (clearOld == true))) {   
                    if (checker.Address() == ipAddress) {
                        addIt = false;
                    }
                    else if (clearOld == true) {
                        adapter.Delete(checker.Address());
                    }
                }
            }

            if (addIt == true) {
                TRACE_L1("Setting IP: %s", ipAddress.HostAddress().c_str());
                adapter.Add(ipAddress);
            }
            if (broadcast.IsValid() == true) {
                adapter.Broadcast(broadcast);
            }
            if (gateway.IsValid() == true) {
                adapter.Gateway(Core::IPNode(Core::NodeId("0.0.0.0"), 0), gateway);
            }

            Core::AdapterIterator::Flush();

            SubSystemValidation();

            string message(string("{ \"interface\": \"") + adapter.Name() + string("\", \"status\":0, \"ip\":\"" + ipAddress.HostAddress() + "\" }"));
            TRACE(Trace::Information, (_T("Interface: [%s] set IP: [%s]"), adapter.Name().c_str(), ipAddress.HostAddress().c_str()));

            _service->Notify(message); 
        }

        return (Core::ERROR_NONE);
    }

    /* Saves leased offer to file */
    void NetworkControl::Save(const string& filename) 
    {
        if (filename.empty() == false) {
            Core::File leaseFile(filename);

            if (leaseFile.Create() == true) {
                Store storage;

                for (std::pair<const string, DHCPEngine>& entry : _dhcpInterfaces) {
                    Entry& result = storage.Add();
                    entry.second.Get(result);
                }

                if (storage.IElement::ToFile(leaseFile) == false) {
                    TRACE(Trace::Warning, ("Error occured while trying to save dhcp lease to file!"));
                } 

                leaseFile.Close();
            } else {
                TRACE(Trace::Warning, ("Failed to open leases file %s\n for saving", leaseFile.Name().c_str()));
            }
        }
    }
    
    /*
        Loads list of previously saved offers and adds it to unleased list 
        for requesting in future.
    */
    bool NetworkControl::Load(const string& filename, std::map<const string, const Entry>& info) 
    {
        bool result = false;

        if (filename.empty() == false) {

            Core::File leaseFile(filename);

            if (leaseFile.Size() != 0 && leaseFile.Open(true) == true) {

                Store storage;
                Core::OptionalType<Core::JSON::Error> error;
                if (storage.IElement::FromFile(leaseFile, error) == true) {
                    
                    Core::JSON::ArrayType<Entry>::Iterator loop(storage.Elements());

                    while (loop.Next() == true) {
                        info.emplace(std::piecewise_construct,
                            std::forward_as_tuple(loop.Current().Interface.Value()),
                            std::forward_as_tuple(loop.Current()));
                    }
                }

                if (error.IsSet() == true) {
                    SYSLOG(Logging::ParsingError, (_T("Parsing failed with %s"), ErrorDisplayMessage(error.Value()).c_str()));
                } else {
                    result = true;
                }

                leaseFile.Close();
            }
        }

        return result;
    }

    uint32_t NetworkControl::Reload(const string& interfaceName, const bool dynamic)
    {

        uint32_t result = Core::ERROR_UNKNOWN_KEY;

        std::map<const string, DHCPEngine>::iterator index(_dhcpInterfaces.find(interfaceName));

        if (index != _dhcpInterfaces.end()) {

            Core::AdapterIterator adapter(interfaceName);

            if ( (adapter.IsValid() == false) || (adapter.IsRunning() == false) || (adapter.HasMAC() == false) ) {
                SYSLOG(Logging::Notification, (_T("Adapter [%s] not available or in the wrong state."), interfaceName.c_str()));
            }
            else if (dynamic == false) {
                if (index->second.Info().Address().IsValid() == true) {
                    SYSLOG(Logging::Notification, (_T("Invalid static IP address, for interfaces: %s"), interfaceName.c_str()));
                }
                else { 
                    result = SetIP(adapter, index->second.Info().Address(), index->second.Info().Gateway(), index->second.Info().Broadcast());
                }
            } else {
                uint8_t mac[6];
                adapter.MACAddress(mac, sizeof(mac));
                index->second.UpdateMAC(mac, sizeof(mac));
                index->second.Discover(index->second.Info().Address());
                result = Core::ERROR_NONE;
            }
        }

        return (result);
    }

    void NetworkControl::SubSystemValidation() {

        uint16_t count = 0;
        bool fullSet = true;
        bool validIP = false;

        Core::AdapterIterator adapter;

        while (adapter.Next() == true) {
            const string name (adapter.Name());
            std::map<const string, DHCPEngine>::iterator index (_dhcpInterfaces.find(name));
            
            if (index != _dhcpInterfaces.end()) {
                bool hasValidIP = ExternallyAccessible(adapter);
                TRACE_L1("Interface [%s] has a public IP: [%s]", name.c_str(), hasValidIP ? _T("true") : _T("false"));

                if (std::find(_requiredSet.cbegin(), _requiredSet.cend(), name) != _requiredSet.cend()) {
                    count++;
                    fullSet &= hasValidIP;
                }
                validIP |= hasValidIP;
            }
        }

        ASSERT (count <= _requiredSet.size());

        fullSet &= ( (count == _requiredSet.size()) && validIP );

        PluginHost::ISubSystem* subSystem = _service->SubSystems();
        ASSERT(subSystem != nullptr);

        if (subSystem != nullptr) {
            if (subSystem->IsActive(PluginHost::ISubSystem::NETWORK) ^ fullSet) {
                subSystem->Set(fullSet ? PluginHost::ISubSystem::NETWORK : PluginHost::ISubSystem::NOT_NETWORK, nullptr);
            }

            subSystem->Release();
        }
    }

    void NetworkControl::Accepted(const string& interfaceName, const DHCPClient::Offer& offer)
    {
        std::map<const string, DHCPEngine>::iterator entry(_dhcpInterfaces.find(interfaceName));

        if (entry != _dhcpInterfaces.end()) {

            Core::AdapterIterator adapter(interfaceName);

            ASSERT(adapter.IsValid() == true);

            if (adapter.IsValid() == true) {

                TRACE(Trace::Information, ("DHCP Request for interface %s accepted, %s offered IP!\n", interfaceName.c_str(), offer.Address().HostAddress().c_str()));

                SetIP(adapter, Core::IPNode(offer.Address(), offer.Netmask()), offer.Gateway(), offer.Broadcast(), true);
                
                RefreshDNS();

                TRACE_L1("New IP Granted for %s:", interfaceName.c_str());
                TRACE_L1("     Source:    %s", offer.Source().HostAddress().c_str());
                TRACE_L1("     Address:   %s", offer.Address().HostAddress().c_str());
                TRACE_L1("     Broadcast: %s", offer.Broadcast().HostAddress().c_str());
                TRACE_L1("     Gateway:   %s", offer.Gateway().HostAddress().c_str());
                TRACE_L1("     DNS:       %d", offer.DNS().size());
                TRACE_L1("     Netmask:   %d", offer.Netmask());

                Save(_persistentStoragePath);
            }
        } else {
            TRACE_L1("Request accepted for nonexisting network interface!");
        }
    }

    void NetworkControl::Failed(const string& interfaceName) 
    {
        _adminLock.Lock();

        std::map<const string, DHCPEngine>::iterator index(_dhcpInterfaces.find(interfaceName));

        if (index != _dhcpInterfaces.end()) {

            string message(string("{ \"interface\": \"") + index->first + string("\", \"status\":11 }"));
            TRACE(Trace::Information, (_T("DHCP Request timed out on: %s"), index->first.c_str()));

            _service->Notify(message);
        }

        _adminLock.Unlock();
    }

    void NetworkControl::RefreshDNS()
    {
        Core::DataElementFile file(_dnsFile, Core::File::SHAREABLE|Core::File::USER_READ|Core::File::USER_WRITE|Core::File::USER_EXECUTE|Core::File::GROUP_READ|Core::File::GROUP_WRITE, 0);

        if (file.IsValid() == false) {
            SYSLOG(Logging::Notification, (_T("DNS functionality could NOT be updated [%s]"), _dnsFile.c_str()));
        } else {
            string data((_T("#++SECTION: ")) + _service->Callsign() + '\n');
            string endMarker((_T("#--SECTION: ")) + _service->Callsign() + '\n');
            uint16_t start = 0;
            uint16_t end = 0;
            uint16_t offset = 1;
            uint8_t index = 0;

            do {
                index += offset - 1;
                offset = 0;

                // Find the first comparible character && check the remainder in the next step...
                while ((index < file.Size()) && (file[index] != data[offset])) {
                    index++;
                }
                while ((index < file.Size()) && (offset < data.length()) && (file[index] == data[offset])) {
                    index++;
                    offset++;
                }

            } while ((index < file.Size()) && (offset != data.length()));

            start = index - offset;
            offset = 1;

            do {
                index += offset - 1;
                offset = 0;

                // Find the first comparible character && check the remainder in the next step...
                while ((index < file.Size()) && (file[index] != endMarker[offset])) {
                    index++;
                }
                while ((index < file.Size()) && (offset < endMarker.length()) && (file[index] == endMarker[offset])) {
                    index++;
                    offset++;
                }

            } while ((index < file.Size()) && (offset != endMarker.length()));

            end = index;
            uint16_t reduction = end - start;

            if (reduction != 0) {
                // move everything after de marker, over the marker sections.
                ::memcpy(&(file[start]), &file[end], static_cast<uint16_t>(file.Size()) - end);
            }

            offset = (static_cast<uint16_t>(file.Size()) - reduction);

            _adminLock.Lock();

            std::list<Core::NodeId> servers(_dns);

            for (const std::pair<const string, DHCPEngine>& entry : _dhcpInterfaces) {
                
                if ( (entry.second.Info().Mode() == JsonData::NetworkControl::NetworkData::ModeType::DYNAMIC) && (entry.second.HasActiveLease() == true) ) {
                  
                    for (const Core::NodeId& node : entry.second.DNS()) {
                        if (std::find(servers.begin(), servers.end(), node) == servers.end()) {
                            servers.push_back(node);
                        }
                    }
                }
            }

            _adminLock.Unlock();

            for (const Core::NodeId& entry : servers) {
                data += string(NAMESERVER, sizeof(NAMESERVER) - 1) + entry.HostAddress() + '\n';
            }

            data += endMarker;
            file.Size(offset + data.length());
            ::memcpy(&(file[offset]), data.c_str(), data.length());
            file.Sync();

            SYSLOG(Logging::Startup, (_T("DNS functionality updated [%s]"), _dnsFile.c_str()));
        }
    }

    void NetworkControl::Activity(const string& interfaceName)
    {
        Core::AdapterIterator adapter(interfaceName);

        if (adapter.IsValid() == true) {

            std::map<const string, DHCPEngine>::iterator index(_dhcpInterfaces.find(interfaceName));
            JsonData::NetworkControl::ConnectionchangeParamsData::StatusType status;

            // Send a message with the state of the adapter.
            string message = string(_T("{ \"interface\": \"")) + interfaceName + string(_T("\", \"running\": \"")) + string(adapter.IsRunning() ? _T("true") : _T("false")) + string(_T("\", \"up\": \"")) + string(adapter.IsUp() ? _T("true") : _T("false")) + _T("\", \"event\": \"");
            TRACE(Trace::Information, (_T("Adapter change report on: %s"), interfaceName.c_str()));

            _adminLock.Lock();

           if (index != _dhcpInterfaces.end()) {
                message += _T("Update\" }");
                status = JsonData::NetworkControl::ConnectionchangeParamsData::StatusType::UPDATED;
                TRACE(Trace::Information, (_T("Statechange on interface: [%s], running: [%s]"), interfaceName.c_str(), adapter.IsRunning() ? _T("true") : _T("false")));

                if (adapter.IsRunning() == true) {

                    JsonData::NetworkControl::NetworkData::ModeType how(index->second.Info().Mode());
                    Reload(interfaceName, how == JsonData::NetworkControl::NetworkData::ModeType::DYNAMIC);

                } else {
                    ClearIP(adapter);
                }

                _service->Notify(message);
                event_connectionchange(interfaceName.c_str(), string(), status);
            }
            else if (_open == true) {
                _dhcpInterfaces.emplace(std::piecewise_construct,
                    std::forward_as_tuple(interfaceName),
                    std::forward_as_tuple(*this, interfaceName, _responseTime, _retries, Entry()));

                message += _T("Create\" }");

                TRACE(Trace::Information, (_T("Added interface: %s"), interfaceName.c_str()));
                status = JsonData::NetworkControl::ConnectionchangeParamsData::StatusType::CREATED;

                _service->Notify(message);
                event_connectionchange(interfaceName.c_str(), string(), status);
            } 
 
            _adminLock.Unlock();
        }
    }

} // namespace Plugin
} // namespace WPEFramework

