/********************************************************************************
 * Copyright (C) 2012-2023 GSI Helmholtzzentrum fuer Schwerionenforschung GmbH  *
 *                                                                              *
 *              This software is distributed under the terms of the             *
 *              GNU Lesser General Public Licence (LGPL) version 3,             *
 *                  copied verbatim in the file "LICENSE"                       *
 ********************************************************************************/

// FairMQ
#include <fairmq/Device.h>
#include <fairmq/Tools.h>

// boost
#include <boost/algorithm/string.hpp>   // join/split

// std
#include <algorithm>   // std::max, std::any_of
#include <chrono>
#include <iomanip>
#include <list>
#include <memory>   // std::make_unique
#include <mutex>
#include <thread>

namespace fair::mq {

using namespace std;

constexpr const char* Device::DefaultId;
constexpr int Device::DefaultIOThreads;
constexpr const char* Device::DefaultTransportName;
constexpr mq::Transport Device::DefaultTransportType;
constexpr const char* Device::DefaultNetworkInterface;
constexpr int Device::DefaultInitTimeout;
constexpr float Device::DefaultRate;
constexpr const char* Device::DefaultSession;

struct StateSubscription
{
    StateMachine& fStateMachine;
    StateQueue& fStateQueue;
    string fId;

    explicit StateSubscription(string id, StateMachine& stateMachine, StateQueue& stateQueue)
        : fStateMachine(stateMachine)
        , fStateQueue(stateQueue)
        , fId(std::move(id))
    {
        fStateMachine.SubscribeToStateChange(fId, [&](State state) {
            fStateQueue.Push(state);
        });
    }

    StateSubscription(const StateSubscription&) = delete;
    StateSubscription(StateSubscription&&) = delete;
    StateSubscription& operator=(const StateSubscription&) = delete;
    StateSubscription& operator=(StateSubscription&&) = delete;

    ~StateSubscription() {
        fStateMachine.UnsubscribeFromStateChange(fId);
    }
};

Device::Device()
    : Device(nullptr, {0, 0, 0})
{}

Device::Device(ProgOptions& config)
    : Device(&config, {0, 0, 0})
{}

Device::Device(tools::Version version)
    : Device(nullptr, version)
{}

Device::Device(ProgOptions& config, tools::Version version)
    : Device(&config, version)
{}

/// TODO: Remove this once Device::fChannels is no longer public
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
Device::Device(ProgOptions* config, tools::Version version)
    : fTransportFactory(nullptr)
    , fInternalConfig(config ? nullptr : make_unique<ProgOptions>())
    , fConfig(config ? config : fInternalConfig.get())
    , fId(DefaultId)
    , fDefaultTransportType(DefaultTransportType)
    , fDataCallbacks(false)
    , fMultitransportProceed(false)
    , fVersion(version)
    , fRate(DefaultRate)
    , fInitializationTimeoutInS(DefaultInitTimeout)
{
    SubscribeToNewTransition("device", [&](Transition transition) {
        LOG(trace) << "device notified on new transition: " << transition;
        InterruptTransports();
    });

    fStateMachine.PrepareState([&](State state) {
        LOG(trace) << "Resuming transports for " << state << " state";
        ResumeTransports();
    });

    fStateMachine.HandleStates([&](State state) {
        LOG(trace) << "device notified on new state: " << state;

        fStateQueue.Push(state);

        switch (state) {
            case State::InitializingDevice:
                InitWrapper();
                break;
            case State::Binding:
                BindWrapper();
                break;
            case State::Connecting:
                ConnectWrapper();
                break;
            case State::InitializingTask:
                InitTaskWrapper();
                break;
            case State::Running:
                RunWrapper();
                break;
            case State::ResettingTask:
                ResetTaskWrapper();
                break;
            case State::ResettingDevice:
                ResetWrapper();
                break;
            case State::Exiting:
                Exit();
                break;
            default:
                LOG(trace) << "device notified on new state without a matching handler: " << state;
                break;
        }
    });

    fStateMachine.Start();
}
#pragma GCC diagnostic pop

void Device::InitWrapper()
{
    // run initialization once CompleteInit transition is requested
    fStateMachine.WaitForPendingState();

    fId = fConfig->GetProperty<string>("id", DefaultId);

    Init();

    fRate = fConfig->GetProperty<float>("rate", DefaultRate);
    fInitializationTimeoutInS = fConfig->GetProperty<int>("init-timeout", DefaultInitTimeout);

    try {
        fDefaultTransportType = TransportTypes.at(fConfig->GetProperty<string>("transport", DefaultTransportName));
    } catch (const exception& e) {
        LOG(error) << "exception: " << e.what();
        LOG(error) << "invalid transport type provided: " << fConfig->GetProperty<string>("transport", "not provided");
        throw;
    }

    unordered_map<string, int> infos = fConfig->GetChannelInfo();
    for (const auto& info : infos) {
        for (int i = 0; i < info.second; ++i) {
            GetChannels()[info.first].emplace_back(info.first, i, fConfig->GetPropertiesStartingWith(tools::ToString("chans.", info.first, ".", i, ".")));
        }
    }

    LOG(debug) << "Setting '" << TransportNames.at(fDefaultTransportType) << "' as default transport for the device";
    fTransportFactory = AddTransport(fDefaultTransportType);

    string networkInterface = fConfig->GetProperty<string>("network-interface", DefaultNetworkInterface);

    // Fill the uninitialized channel containers
    for (auto& channel : GetChannels()) {
        int subChannelIndex = 0;
        for (auto& subChannel : channel.second) {
            // set channel transport
            LOG(debug) << "Initializing transport for channel " << subChannel.fName << ": " << TransportNames.at(subChannel.fTransportType);
            subChannel.InitTransport(AddTransport(subChannel.fTransportType));

            if (subChannel.fMethod == "bind") {
                // if binding address is not specified, try getting it from the configured network interface
                if (subChannel.fAddress == "unspecified" || subChannel.fAddress.empty()) {
                    // if the configured network interface is default, get its name from the default route
                    try {
                        if (networkInterface == "default") {
                            networkInterface = tools::getDefaultRouteNetworkInterface();
                        }
                        subChannel.fAddress = "tcp://" + tools::getInterfaceIP(networkInterface) + ":1";
                    } catch(const tools::DefaultRouteDetectionError& e) {
                        LOG(debug) << "binding on tcp://*:1";
                        subChannel.fAddress = "tcp://*:1";
                    }
                }
                // fill the uninitialized list
                fUninitializedBindingChannels.push_back(&subChannel);
            } else if (subChannel.fMethod == "connect") {
                // fill the uninitialized list
                fUninitializedConnectingChannels.push_back(&subChannel);
            } else if (subChannel.fAddress.find_first_of("@+>") != string::npos) {
                // fill the uninitialized list
                fUninitializedConnectingChannels.push_back(&subChannel);
            } else {
                LOG(error) << "Cannot update configuration. Socket method (bind/connect) for channel '" << subChannel.fName << "' not specified.";
                throw runtime_error(tools::ToString("Cannot update configuration. Socket method (bind/connect) for channel ", subChannel.fName, " not specified."));
            }

            subChannelIndex++;
        }
    }

    // ChangeStateOrThrow(Transition::Auto);
}

void Device::BindWrapper()
{
    // Bind channels. Here one run is enough, because bind settings should be available locally
    // If necessary this could be handled in the same way as the connecting channels
    AttachChannels(fUninitializedBindingChannels);

    if (!fUninitializedBindingChannels.empty()) {
        LOG(error) << fUninitializedBindingChannels.size() << " of the binding channels could not initialize. Initial configuration incomplete.";
        throw runtime_error(tools::ToString(fUninitializedBindingChannels.size(), " of the binding channels could not initialize. Initial configuration incomplete."));
    }

    Bind();

    if (!NewStatePending()) {
        ChangeStateOrThrow(Transition::Auto);
    }
}

void Device::ConnectWrapper()
{
    // go over the list of channels until all are initialized (and removed from the uninitialized list)
    int numAttempts = 1;
    auto sleepTimeInMS = 50;
    auto maxAttempts = fInitializationTimeoutInS * 1000 / sleepTimeInMS;
    // first attempt
    AttachChannels(fUninitializedConnectingChannels);
    // if not all channels could be connected, update their address values from config and retry
    while (!fUninitializedConnectingChannels.empty() && !NewStatePending()) {
        this_thread::sleep_for(chrono::milliseconds(sleepTimeInMS));

        for (auto& chan : fUninitializedConnectingChannels) {
            string key{"chans." + chan->GetPrefix() + "." + chan->GetIndex() + ".address"};
            string newAddress = fConfig->GetProperty<string>(key);
            if (newAddress != chan->GetAddress()) {
                chan->UpdateAddress(newAddress);
            }
        }

        if (numAttempts++ > maxAttempts) {
            LOG(error) << "could not connect all channels after " << fInitializationTimeoutInS << " attempts";
            LOG(error) << "following channels are still invalid:";
            for (auto& chan : fUninitializedConnectingChannels) {
                LOG(error) << "channel: " << *chan;
            }
            throw runtime_error(tools::ToString("could not connect all channels after ", fInitializationTimeoutInS, " attempts"));
        }

        AttachChannels(fUninitializedConnectingChannels);
    }

    if (GetChannels().empty()) {
        LOG(warn) << "No channels created after finishing initialization";
    }

    Connect();

    if (!NewStatePending()) {
        ChangeStateOrThrow(Transition::Auto);
    }
}

void Device::AttachChannels(vector<Channel*>& chans)
{
    auto itr = chans.begin();

    while (itr != chans.end()) {
        if ((*itr)->Validate()) {
            (*itr)->Init();
            if (AttachChannel(**itr)) {
                // remove the channel from the uninitialized container
                itr = chans.erase(itr);
            } else {
                LOG(error) << "failed to attach channel " << (*itr)->fName << " (" << (*itr)->fMethod << ")";
                ++itr;
            }
        } else {
            ++itr;
        }
    }
}

bool Device::AttachChannel(Channel& chan)
{
    vector<string> endpoints;
    string chanAddress = chan.GetAddress();
    boost::algorithm::split(endpoints, chanAddress, boost::algorithm::is_any_of(","));

    for (auto& endpoint : endpoints) {
        // attach
        bool bind = (chan.GetMethod() == "bind");
        bool connectionModifier = false;
        string address = endpoint;

        // check if the default fMethod is overridden by a modifier
        if (endpoint[0] == '+' || endpoint[0] == '>') {
            connectionModifier = true;
            bind = false;
            address = endpoint.substr(1);
        } else if (endpoint[0] == '@') {
            connectionModifier = true;
            bind = true;
            address = endpoint.substr(1);
        }

        if (address.compare(0, 6, "tcp://") == 0) {
            string addressString = address.substr(6);
            auto pos = addressString.find(':');
            string hostPart = addressString.substr(0, pos);
            if (!(bind && hostPart == "*")) {
                string portPart = addressString.substr(pos + 1);
                string resolvedHost = tools::getIpFromHostname(hostPart);
                if (resolvedHost.empty()) {
                    return false;
                }
                address.assign("tcp://" + resolvedHost + ":" + portPart);
            }
        }

        bool success = true;
        // make the connection
        if (bind) {
            success = chan.BindEndpoint(address);
        } else {
            success = chan.ConnectEndpoint(address);
        }

        // bind might bind to an address different than requested,
        // put the actual address back in the config
        endpoint.clear();
        if (connectionModifier) {
            endpoint.push_back(bind?'@':'+');
        }
        endpoint += address;

        // after the book keeping is done, exit in case of errors
        if (!success) {
            return success;
        } else {
            LOG(debug) << "Attached channel " << chan.GetName() << " to " << endpoint << (bind ? " (bind) " : " (connect) ") << "(" << chan.GetType() << ")";
        }
    }

    // put the (possibly) modified address back in the channel object and config
    string newAddress(boost::algorithm::join(endpoints, ","));
    if (newAddress != chanAddress) {
        chan.UpdateAddress(newAddress);

        // update address in the config, it could have been modified during binding
        fConfig->SetProperty({"chans." + chan.GetPrefix() + "." + chan.GetIndex() + ".address"}, newAddress);
    }

    return true;
}

void Device::InitTaskWrapper()
{
    InitTask();

    if (!NewStatePending()) {
        ChangeStateOrThrow(Transition::Auto);
    }
}

void Device::RunWrapper()
{
    LOG(info) << "fair::mq::Device running...";

    unique_ptr<thread> rateLogger;
    // Check if rate logging thread is needed
    const bool rateLogging = any_of(GetChannels().cbegin(), GetChannels().cend(), [](auto ch) {
        return any_of(ch.second.cbegin(), ch.second.cend(), [](auto sub) { return sub.fRateLogging > 0; });
    });

    if (rateLogging) {
        rateLogger = make_unique<thread>(&Device::LogSocketRates, this);
    }
    tools::CallOnDestruction joinRateLogger([&](){
        if (rateLogging && rateLogger->joinable()) { rateLogger->join(); }
    });

    // change to Error state in case of an exception, to release LogSocketRates
    tools::CallOnDestruction cod([&](){
        ChangeStateOrThrow(Transition::ErrorFound);
    });

    PreRun();

    // process either data callbacks or ConditionalRun/Run
    if (fDataCallbacks) {
        // if only one input channel, do lightweight handling without additional polling.
        if (fInputChannelKeys.size() == 1 && GetChannels().at(fInputChannelKeys.at(0)).size() == 1) {
            HandleSingleChannelInput();
        } else {// otherwise do full handling with polling
            HandleMultipleChannelInput();
        }
    } else {
        tools::RateLimiter rateLimiter(fRate);

        while (!NewStatePending() && ConditionalRun()) {
            if (fRate > 0.001) {
                rateLimiter.maybe_sleep();
            }
        }

        Run();
    }

    // if Run() exited and the state is still RUNNING, transition to READY.
    if (!NewStatePending()) {
        ChangeStateOrThrow(Transition::Stop);
    }

    PostRun();

    cod.disable();
}

void Device::HandleSingleChannelInput()
{
    bool proceed = true;

    if (!fMsgInputs.empty()) {
        while (!NewStatePending() && proceed) {
            proceed = HandleMsgInput(fInputChannelKeys.at(0), fMsgInputs.begin()->second, 0);
        }
    } else if (!fMultipartInputs.empty()) {
        while (!NewStatePending() && proceed) {
            proceed = HandleMultipartInput(fInputChannelKeys.at(0), fMultipartInputs.begin()->second, 0);
        }
    }
}

void Device::HandleMultipleChannelInput()
{
    // check if more than one transport is used
    fMultitransportInputs.clear();
    for (const auto& k : fInputChannelKeys) {
        mq::Transport t = GetChannel(k, 0).fTransportType;
        if (fMultitransportInputs.find(t) == fMultitransportInputs.end()) {
            fMultitransportInputs.insert(pair<mq::Transport, vector<string>>(t, vector<string>()));
            fMultitransportInputs.at(t).push_back(k);
        } else {
            fMultitransportInputs.at(t).push_back(k);
        }
    }

    for (const auto& mi : fMsgInputs) {
        for (auto& i : GetChannels().at(mi.first)) {
            i.fMultipart = false;
        }
    }

    for (const auto& mi : fMultipartInputs) {
        for (auto& i : GetChannels().at(mi.first)) {
            i.fMultipart = true;
        }
    }

    // if more than one transport is used, handle poll of each in a separate thread
    if (fMultitransportInputs.size() > 1) {
        HandleMultipleTransportInput();
    } else { // otherwise poll directly
        bool proceed = true;

        PollerPtr poller(GetChannel(fInputChannelKeys.at(0), 0).fTransportFactory->CreatePoller(GetChannels(), fInputChannelKeys));

        while (!NewStatePending() && proceed) {
            poller->Poll(200);

            // check which inputs are ready and call their data handlers if they are.
            for (const auto& ch : fInputChannelKeys) {
                for (unsigned int i = 0; i < GetChannels().at(ch).size(); ++i) {
                    if (poller->CheckInput(ch, i)) {
                        if (GetChannel(ch, i).fMultipart) {
                            proceed = HandleMultipartInput(ch, fMultipartInputs.at(ch), i);
                        } else {
                            proceed = HandleMsgInput(ch, fMsgInputs.at(ch), i);
                        }

                        if (!proceed) {
                            break;
                        }
                    }
                }
                if (!proceed) {
                    break;
                }
            }
        }
    }
}

void Device::HandleMultipleTransportInput()
{
    vector<thread> threads;

    fMultitransportProceed = true;

    for (const auto& i : fMultitransportInputs) {
        threads.emplace_back(thread(&Device::PollForTransport, this, fTransports.at(i.first).get(), i.second));
    }

    for (thread& t : threads) {
        t.join();
    }
}

void Device::PollForTransport(const TransportFactory* factory, const vector<string>& channelKeys)
{
    try {
        PollerPtr poller(factory->CreatePoller(GetChannels(), channelKeys));

        while (!NewStatePending() && fMultitransportProceed) {
            poller->Poll(500);

            for (const auto& ch : channelKeys) {
                for (unsigned int i = 0; i < GetChannels().at(ch).size(); ++i) {
                    if (poller->CheckInput(ch, i)) {
                        lock_guard<mutex> lock(fMultitransportMutex);

                        if (!fMultitransportProceed) {
                            break;
                        }

                        if (GetChannel(ch, i).fMultipart) {
                            fMultitransportProceed = HandleMultipartInput(ch, fMultipartInputs.at(ch), i);
                        } else {
                            fMultitransportProceed = HandleMsgInput(ch, fMsgInputs.at(ch), i);
                        }

                        if (!fMultitransportProceed) {
                            break;
                        }
                    }
                }
                if (!fMultitransportProceed) {
                    break;
                }
            }
        }
    } catch (exception& e) {
        LOG(error) << "fair::mq::Device::PollForTransport() failed: " << e.what() << ", going to ERROR state.";
        throw runtime_error(tools::ToString("fair::mq::Device::PollForTransport() failed: ", e.what(), ", going to ERROR state."));
    }
}

bool Device::HandleMsgInput(const string& chName, const InputMsgCallback& callback, int i)
{
    unique_ptr<Message> input(GetChannel(chName, i).fTransportFactory->CreateMessage());

    if (Receive(input, chName, i) >= 0) {
        return callback(input, i);
    } else {
        return false;
    }
}

bool Device::HandleMultipartInput(const string& chName, const InputMultipartCallback& callback, int i)
{
    Parts input;

    if (Receive(input, chName, i) >= 0) {
        return callback(input, i);
    } else {
        return false;
    }
}

shared_ptr<TransportFactory> Device::AddTransport(mq::Transport transport)
{
    lock_guard<mutex> lock(fTransportMtx);

    if (transport == mq::Transport::DEFAULT) {
        transport = fDefaultTransportType;
    }

    auto i = fTransports.find(transport);

    if (i == fTransports.end()) {
        LOG(debug) << "Adding '" << TransportNames.at(transport) << "' transport";
        auto tr = TransportFactory::CreateTransportFactory(TransportNames.at(transport), fId, fConfig);
        fTransports.insert({transport, tr});
        return tr;
    } else {
        LOG(debug) << "Reusing existing '" << TransportNames.at(transport) << "' transport";
        return i->second;
    }
}

void Device::SetConfig(ProgOptions& config)
{
    fInternalConfig.reset();
    fConfig = &config;
}

void Device::LogSocketRates()
{
    vector<Channel*> filteredChannels;
    vector<string> filteredChannelNames;
    vector<int> logIntervals;
    vector<int> intervalCounters;

    size_t chanNameLen = 0;

    // iterate over the channels map
    for (auto& channel : GetChannels()) {
        // iterate over the channels vector
        for (auto& subChannel : channel.second) {
            if (subChannel.fRateLogging > 0) {
                filteredChannels.push_back(&subChannel);
                logIntervals.push_back(subChannel.fRateLogging);
                intervalCounters.push_back(0);
                filteredChannelNames.push_back(subChannel.GetName());
                chanNameLen = max(chanNameLen, filteredChannelNames.back().length());
            }
        }
    }

    vector<unsigned long> bytesIn(filteredChannels.size());
    vector<unsigned long> msgIn(filteredChannels.size());
    vector<unsigned long> bytesOut(filteredChannels.size());
    vector<unsigned long> msgOut(filteredChannels.size());

    vector<unsigned long> bytesInNew(filteredChannels.size());
    vector<unsigned long> msgInNew(filteredChannels.size());
    vector<unsigned long> bytesOutNew(filteredChannels.size());
    vector<unsigned long> msgOutNew(filteredChannels.size());

    vector<double> mbPerSecIn(filteredChannels.size());
    vector<double> msgPerSecIn(filteredChannels.size());
    vector<double> mbPerSecOut(filteredChannels.size());
    vector<double> msgPerSecOut(filteredChannels.size());

    int i = 0;
    for (const auto& channel : filteredChannels) {
        bytesIn.at(i) = channel->GetBytesRx();
        bytesOut.at(i) = channel->GetBytesTx();
        msgIn.at(i) = channel->GetMessagesRx();
        msgOut.at(i) = channel->GetMessagesTx();
        ++i;
    }

    chrono::time_point<chrono::high_resolution_clock> t0(chrono::high_resolution_clock::now());
    chrono::time_point<chrono::high_resolution_clock> t1;

    while (!NewStatePending()) {
        WaitFor(chrono::seconds(1));

        t1 = chrono::high_resolution_clock::now();

        uint64_t msSinceLastLog = chrono::duration_cast<chrono::milliseconds>(t1 - t0).count();

        i = 0;

        for (const auto& channel : filteredChannels) {
            intervalCounters.at(i)++;

            if (intervalCounters.at(i) == logIntervals.at(i)) {
                intervalCounters.at(i) = 0;

                if (msSinceLastLog > 0) {
                    bytesInNew.at(i) = channel->GetBytesRx();
                    msgInNew.at(i) = channel->GetMessagesRx();
                    bytesOutNew.at(i) = channel->GetBytesTx();
                    msgOutNew.at(i) = channel->GetMessagesTx();

                    mbPerSecIn.at(i) = (static_cast<double>(bytesInNew.at(i) - bytesIn.at(i)) / (1000. * 1000.)) / static_cast<double>(msSinceLastLog) * 1000.;
                    msgPerSecIn.at(i) = static_cast<double>(msgInNew.at(i) - msgIn.at(i)) / static_cast<double>(msSinceLastLog) * 1000.;
                    mbPerSecOut.at(i) = (static_cast<double>(bytesOutNew.at(i) - bytesOut.at(i)) / (1000. * 1000.)) / static_cast<double>(msSinceLastLog) * 1000.;
                    msgPerSecOut.at(i) = static_cast<double>(msgOutNew.at(i) - msgOut.at(i)) / static_cast<double>(msSinceLastLog) * 1000.;

                    bytesIn.at(i) = bytesInNew.at(i);
                    msgIn.at(i) = msgInNew.at(i);
                    bytesOut.at(i) = bytesOutNew.at(i);
                    msgOut.at(i) = msgOutNew.at(i);

                    LOG(info) << setw(static_cast<int>(chanNameLen)) << filteredChannelNames.at(i) << ": "
                              << "in: " << msgPerSecIn.at(i) << " (" << mbPerSecIn.at(i) << " MB) "
                              << "out: " << msgPerSecOut.at(i) << " (" << mbPerSecOut.at(i) << " MB)";
                }
            }

            ++i;
        }

        t0 = t1;
    }
}

void Device::InterruptTransports()
{
    lock_guard<mutex> lock(fTransportMtx);
    for (auto& [transportType, transport] : fTransports) {
        transport->Interrupt();
    }
}

void Device::ResumeTransports()
{
    lock_guard<mutex> lock(fTransportMtx);
    for (auto& [transportType, transport] : fTransports) {
        transport->Resume();
    }
}

void Device::ResetTaskWrapper()
{
    ResetTask();

    if (!NewStatePending()) {
        ChangeStateOrThrow(Transition::Auto);
    }
}

void Device::ResetWrapper()
{
    {
        lock_guard<mutex> lock(fTransportMtx);
        for (auto& [transportType, transport] : fTransports) {
            transport->Reset();
        }
        fTransports.clear();
    }

    Reset();

    GetChannels().clear();
    fTransportFactory.reset();
    if (!NewStatePending()) {
        ChangeStateOrThrow(Transition::Auto);
    }
}

/// TODO: Remove this once Device::fChannels is no longer public
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
Device::~Device()
{
    UnsubscribeFromNewTransition("device");
    fStateMachine.StopHandlingStates();
    LOG(debug) << "Shutting down device " << fId;
}
#pragma GCC diagnostic pop

} // namespace fair::mq
