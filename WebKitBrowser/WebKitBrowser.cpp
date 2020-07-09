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

#include "WebKitBrowser.h"

namespace WPEFramework {

namespace WebKitBrowser {

    // An implementation file needs to implement this method to return an operational browser, wherever that would be :-)
    extern Exchange::IMemory* MemoryObserver(const RPC::IRemoteConnection* connection);
}

namespace Plugin {

    SERVICE_REGISTRATION(WebKitBrowser, 1, 0);

    /* virtual */ const string WebKitBrowser::Initialize(PluginHost::IShell* service)
    {
        Config config;
        string message;

        ASSERT(_service == nullptr);
        ASSERT(_browser == nullptr);
        ASSERT(_memory == nullptr);

        _connectionId = 0;
        _service = service;
        _skipURL = _service->WebPrefix().length();

        config.FromString(_service->ConfigLine());
        _persistentStoragePath = _service->PersistentPath();

        // Register the Connection::Notification stuff. The Remote process might die before we get a
        // change to "register" the sink for these events !!! So do it ahead of instantiation.
        _service->Register(&_notification);

        _browser = service->Root<Exchange::IWebKitBrowser>(_connectionId, 2000, _T("WebKitImplementation"));

        if (_browser != nullptr) {
            PluginHost::IStateControl* stateControl(_browser->QueryInterface<PluginHost::IStateControl>());

            // We see that sometimes the WPE implementation crashes before it reaches this point, than there is
            // no StateControl. Cope with this situation.
            if (stateControl == nullptr) {
                _browser->Release();
                _browser = nullptr;

                stateControl->Release();

            } else {
                _browser->Register(&_notification);

                const RPC::IRemoteConnection *connection = _service->RemoteConnection(_connectionId);
                _memory = WPEFramework::WebKitBrowser::MemoryObserver(connection);
                ASSERT(_memory != nullptr);
                if (connection != nullptr)
                    connection->Release();

                stateControl->Configure(_service);
                stateControl->Register(&_notification);
                stateControl->Release();
            }
        }

        if (_browser == nullptr) {
            message = _T("WebKitBrowser could not be instantiated.");
            _service->Unregister(&_notification);
            _service = nullptr;
        } else {
            RegisterAll();
        }

        return message;
    }

    /* virtual */ void WebKitBrowser::Deinitialize(PluginHost::IShell* service)
    {
        ASSERT(_service == service);
        ASSERT(_browser != nullptr);
        ASSERT(_memory != nullptr);

        if (_browser == nullptr)
            return;

        // Make sure we get no longer get any notifications, we are deactivating..
        _service->Unregister(&_notification);
        _browser->Unregister(&_notification);
        _memory->Release();

        PluginHost::IStateControl* stateControl(_browser->QueryInterface<PluginHost::IStateControl>());

        // In case WPE rpcprocess crashed, there is no access to the statecontrol interface, check it !!
        if (stateControl != nullptr) {
            stateControl->Unregister(&_notification);
            stateControl->Release();
        }

        // Stop processing of the browser:
        if (_browser->Release() != Core::ERROR_DESTRUCTION_SUCCEEDED) {
            ASSERT(_connectionId != 0);

            TRACE_L1("Browser Plugin is not properly destructed. %d", _connectionId);

            RPC::IRemoteConnection* connection(_service->RemoteConnection(_connectionId));
            // The process can disappear in the meantime...
            if (connection != nullptr) {
                // But if it did not dissapear in the meantime, forcefully terminate it. Shoot to kill :-)
                connection->Terminate();
                connection->Release();
            }
        }

        _service = nullptr;
        _browser = nullptr;
        _memory = nullptr;
    }

    /* virtual */ string WebKitBrowser::Information() const
    {
        // No additional info to report.
        return { };
    }

    /* virtual */ void WebKitBrowser::Inbound(Web::Request& request)
    {
        if (request.Verb == Web::Request::HTTP_POST) {
            request.Body(_jsonBodyDataFactory.Element());
        }
    }

    /* virtual */ Core::ProxyType<Web::Response> WebKitBrowser::Process(const Web::Request& request)
    {
        ASSERT(_skipURL <= request.Path.length());

        TRACE(Trace::Information, (string(_T("Received request"))));

        Core::ProxyType<Web::Response> result(PluginHost::IFactories::Instance().Response());
        Core::TextSegmentIterator index(
            Core::TextFragment(request.Path, _skipURL, request.Path.length() - _skipURL), false, '/');

        result->ErrorCode = Web::STATUS_BAD_REQUEST;
        result->Message = "Unknown error";

        if (_browser != nullptr) {

            PluginHost::IStateControl* stateControl(_browser->QueryInterface<PluginHost::IStateControl>());

            ASSERT(stateControl != nullptr);

            if (request.Verb == Web::Request::HTTP_GET) {
                auto visibilityState = _browser->GetVisibility();
                PluginHost::IStateControl::state currentState = stateControl->State();
                Core::ProxyType<Web::JSONBodyType<WebKitBrowser::Data>> body(_jsonBodyDataFactory.Element());
                body->URL = _browser->GetURL();
                body->FPS = _browser->GetFPS();
                body->Suspended = (currentState == PluginHost::IStateControl::SUSPENDED);
                body->Hidden = (visibilityState != Exchange::IWebKitBrowser::Visibility::VISIBLE);
                result->ErrorCode = Web::STATUS_OK;
                result->Message = "OK";
                result->Body<Web::JSONBodyType<WebKitBrowser::Data>>(body);
            } else if ((request.Verb == Web::Request::HTTP_POST) && (index.Next() == true) && (index.Next() == true)) {
                result->ErrorCode = Web::STATUS_OK;
                result->Message = "OK";

                // We might be receiving a plugin download request.
                if (index.Remainder() == _T("Suspend")) {
                    stateControl->Request(PluginHost::IStateControl::SUSPEND);
                } else if (index.Remainder() == _T("Resume")) {
                    stateControl->Request(PluginHost::IStateControl::RESUME);
                } else if (index.Remainder() == _T("Hide")) {
                    _browser->SetVisibility(Exchange::IWebKitBrowser::Visibility::HIDDEN);
                } else if (index.Remainder() == _T("Show")) {
                    _browser->SetVisibility(Exchange::IWebKitBrowser::Visibility::VISIBLE);
                } else if ((index.Remainder() == _T("URL")) && (request.HasBody() == true) && (request.Body<const Data>()->URL.Value().empty() == false)) {
                    _browser->SetURL(request.Body<const Data>()->URL.Value());

                } else if ((index.Remainder() == _T("Delete")) && (request.HasBody() == true) && (request.Body<const Data>()->Path.Value().empty() == false)) {
                    if (delete_dir(request.Body<const Data>()->Path.Value()) != Core::ERROR_NONE) {
                        result->ErrorCode = Web::STATUS_BAD_REQUEST;
                        result->Message = "Unknown error";
                    }
                } else {
                    result->ErrorCode = Web::STATUS_BAD_REQUEST;
                    result->Message = "Unknown error";
                }
            }
            stateControl->Release();
        }

        return result;
    }

    void WebKitBrowser::LoadFinished(const string& URL, int32_t code)
    {
        TRACE(Trace::Information, (_T("LoadFinished: { \"url\": \"%s\", \"httpstatus\": %d}"), URL.c_str(), code));
        event_loadfinished(URL, code);
        URLChange(URL, true);
    }

    void WebKitBrowser::LoadFailed(const string& URL)
    {
        TRACE(Trace::Information, (_T("LoadFailed: { \"url\": \"%s\" }"), URL.c_str()));
        event_loadfailed(URL);
    }

    void WebKitBrowser::URLChange(const string& URL, bool loaded)
    {
        TRACE(Trace::Information, (_T("URLChange: { \"url\": \"%s\", \"loaded\": \"%s\" }"), URL.c_str(), (loaded ? "true" : "false")));
        event_urlchange(URL, loaded);
    }

    void WebKitBrowser::VisibilityChange(const bool hidden)
    {
        TRACE(Trace::Information, (_T("VisibilityChange: { \"hidden\": \"%s\"}"), (hidden ? "true" : "false")));
        event_visibilitychange(hidden);
    }

    void WebKitBrowser::PageClosure()
    {
        TRACE(Trace::Information, (_T("PageClosure")));
        event_pageclosure();
    }

    void WebKitBrowser::BridgeQuery(const string& message)
    {
        TRACE(Trace::Information, (_T("BridgeQuery: %s"), message.c_str()));
        event_bridgequery(message);
    }

    void WebKitBrowser::StateChange(const PluginHost::IStateControl::state state)
    {
        TRACE(Trace::Information, (_T("StateChange: { \"State\": %d }"), state));
        event_statechange(state == PluginHost::IStateControl::SUSPENDED);
    }

    void WebKitBrowser::Deactivated(RPC::IRemoteConnection* connection)
    {
        if (connection->Id() == _connectionId) {

            ASSERT(_service != nullptr);

            Core::IWorkerPool::Instance().Submit(PluginHost::IShell::Job::Create(_service, PluginHost::IShell::DEACTIVATED, PluginHost::IShell::FAILURE));
        }
    }
}  // namespace Plugin

}  // WPEFramework
