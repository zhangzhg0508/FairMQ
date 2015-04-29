/* 
 * File:   GenericMerger.h
 * Author: winckler
 *
 * Created on April 9, 2015, 1:37 PM
 */

#ifndef GENERICMERGER_H
#define	GENERICMERGER_H


#include <boost/thread.hpp>
#include <boost/bind.hpp>

#include "FairMQDevice.h"
#include "FairMQLogger.h"
#include "FairMQPoller.h"


template <typename MergerPolicy, typename InputPolicy, typename OutputPolicy>
class GenericMerger : public FairMQDevice, public MergerPolicy, public InputPolicy, public OutputPolicy
{
  public:
    GenericMerger()
        : fBlockingTime(100)
    {}

    virtual ~GenericMerger()
    {}

    void SetTransport(FairMQTransportFactory* transport)
    {
        FairMQDevice::SetTransport(transport);
    }

  protected:
    int fBlockingTime;

    virtual void Run()
    {
        FairMQPoller* poller = fTransportFactory->CreatePoller(fChannels["data-in"]);

        int received = 0;

        while (GetCurrentState() == RUNNING)
        {
            FairMQMessage* msg = fTransportFactory->CreateMessage();
            // MergerPolicy::
            poller->Poll(fBlockingTime);

            for (int i = 0; i < fChannels["datain"].size(); i++)
            {
                if (poller->CheckInput(i))
                {
                    received = fChannels["data-in"].at(i).Receive(msg)
                    MergerPolicy::Merge(InputPolicy::DeSerializeMsg(msg));
                }

                OutputPolicy::SetMessage(msg);

                if (received > 0 && MergerPolicy::ReadyToSend())
                {
                    fChannels["data-out"].at(0).Send(OutputPolicy::SerializeMsg(MergerPolicy::GetOutputData()));
                    received = 0;
                }
            }

            delete msg;
        }

        delete poller;
    }
};

#endif /* GENERICMERGER_H */

