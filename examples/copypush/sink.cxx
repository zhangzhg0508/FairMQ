/********************************************************************************
 *    Copyright (C) 2014 GSI Helmholtzzentrum fuer Schwerionenforschung GmbH    *
 *                                                                              *
 *              This software is distributed under the terms of the             *
 *              GNU Lesser General Public Licence (LGPL) version 3,             *
 *                  copied verbatim in the file "LICENSE"                       *
 ********************************************************************************/

#include <fairmq/Device.h>
#include <fairmq/runDevice.h>

#include <cstdint> // uint64_t

namespace bpo = boost::program_options;

struct Sink : fair::mq::Device
{
    Sink()
    {
        OnData("data", &Sink::HandleData);
    }

  protected:
    void InitTask() override
    {
        // Get the fMaxIterations value from the command line options (via fConfig)
        fMaxIterations = fConfig->GetProperty<uint64_t>("max-iterations");
    }

    bool HandleData(fair::mq::MessagePtr& msg, int)
    {
        LOG(info) << "Received message: \"" << *(static_cast<uint64_t*>(msg->GetData())) << "\"";

        if (fMaxIterations > 0 && ++fNumIterations >= fMaxIterations) {
            LOG(info) << "Configured maximum number of iterations reached. Leaving RUNNING state.";
            return false;
        }

        // return true if want to be called again (otherwise go to IDLE state)
        return true;
    }

  private:
    uint64_t fMaxIterations = 0;
    uint64_t fNumIterations = 0;
};

void addCustomOptions(bpo::options_description& options)
{
    options.add_options()
        ("max-iterations", bpo::value<uint64_t>()->default_value(0), "Maximum number of iterations of Run/ConditionalRun/OnData (0 - infinite)");
}

std::unique_ptr<fair::mq::Device> getDevice(fair::mq::ProgOptions& /*config*/)
{
    return std::make_unique<Sink>();
}