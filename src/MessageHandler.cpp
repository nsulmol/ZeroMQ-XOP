#include "ZeroMQ.h"
#include "MessageHandler.h"
#include "RequestInterface.h"

#include <chrono>
#include <thread>

// This file is part of the `ZeroMQ-XOP` project and licensed under
// BSD-3-Clause.

namespace
{

using namespace std::chrono_literals;
std::recursive_mutex threadMutex;
ConcurrentQueue<RequestInterfacePtr> reqQueue;
bool threadShouldFinish;
std::recursive_mutex threadShouldFinishMutex;

void WorkerThread()
{
  DebugOutput(fmt::format("{}: Begin WorkerThread().\r", __func__));

  {
    // initialize to false
    LockGuard lock(threadShouldFinishMutex);
    threadShouldFinish = false;
  }

  zmq_msg_t identityMsg;
  zmq_msg_t payloadMsg;

  int rc = zmq_msg_init(&identityMsg);
  ZEROMQ_ASSERT(rc == 0);

  rc = zmq_msg_init(&payloadMsg);
  ZEROMQ_ASSERT(rc == 0);

  for(;;)
  {
    try
    {
      // check if stop is requested
      {
        LockGuard lock(threadShouldFinishMutex);
        if(threadShouldFinish)
        {
          DebugOutput(fmt::format("{}: Exiting.\r", __func__));
          break;
        }
      }

      auto numBytes = ZeroMQServerReceive(&identityMsg, &payloadMsg);

      if(numBytes == -1 && zmq_errno() == EAGAIN) // timeout
      {
        std::this_thread::sleep_for(10ms);
        continue;
      }

      ZEROMQ_ASSERT(numBytes >= 0);

      DebugOutput(fmt::format("{}: numBytes={}\r", __func__, numBytes));

      const auto identity = CreateStringFromZMsg(&identityMsg);

      try
      {
        try
        {
          const auto payload = CreateStringFromZMsg(&payloadMsg);
          reqQueue.push(std::make_shared<RequestInterface>(identity, payload));
        }
        catch(const std::bad_alloc &)
        {
          throw RequestInterfaceException(REQ_OUT_OF_MEMORY);
        }
      }
      catch(const IgorException &e)
      {
        const json reply = e;
        rc               = ZeroMQServerSend(identity, reply.dump(4));

        DebugOutput(
            fmt::format("{}: ZeroMQSendAsServer returned {}\r", __func__, rc));
      }
    }
    catch(const std::exception &e)
    {
      XOPNotice_ts(fmt::format(
          "{}: Caught std::exception with what=\"{}\". This must NOT happen!\r",
          __func__, e.what()));
    }
    catch(...)
    {
      XOPNotice_ts(fmt::format("{}: Caught exception. This must NOT happen!\r",
                               __func__));
    }
  }

  // ignore errors
  zmq_msg_close(&identityMsg);
  zmq_msg_close(&payloadMsg);
}

void CallAndReply(const RequestInterfacePtr &req) noexcept
{
  try
  {
    try
    {
      auto message = CallIgorFunctionFromReqInterface(req);
      ZeroMQServerSend(req->GetCallerIdentity(), message);
    }
    catch(const std::exception &e)
    {
      XOPNotice_ts(fmt::format(
          "{}: Caught std::exception with what=\"{}\". This must NOT happen!\r",
          __func__, e.what()));
    }
  }
  catch(...)
  {
    XOPNotice_ts(
        fmt::format("{}: Caught exception. This must NOT happen!\r", __func__));
  }
}

} // anonymous namespace

void MessageHandler::StartHandler()
{
  LockGuard lock(threadMutex);

  if(m_thread.joinable())
  {
    throw IgorException(HANDLER_ALREADY_RUNNING);
  }

  DebugOutput(fmt::format("{}: Trying to start the handler.\r", __func__));

  if(!GlobalData::Instance().HasBinds())
  {
    throw IgorException(HANDLER_NO_CONNECTION);
  }

  DebugOutput(fmt::format("{}: Before WorkerThread() start.\r", __func__));

  auto t = std::thread(WorkerThread);
  m_thread.swap(t);
}

void MessageHandler::StopHandler()
{
  LockGuard lock(threadMutex);

  if(!m_thread.joinable())
  {
    return;
  }

  DebugOutput(fmt::format("{}: Shutting down the handler.\r", __func__));

  {
    LockGuard innerLock(threadShouldFinishMutex);
    threadShouldFinish = true;
  }

  m_thread.join();
}

void MessageHandler::HandleAllQueuedMessages()
{
  if(!RunningInMainThread() || reqQueue.empty())
  {
    return;
  }

  reqQueue.apply_to_all(CallAndReply);
}

MessageHandler::~MessageHandler()
{
  StopHandler();
}
