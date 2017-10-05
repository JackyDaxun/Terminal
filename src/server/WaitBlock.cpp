/********************************************************
*                                                       *
*   Copyright (C) Microsoft. All rights reserved.       *
*                                                       *
********************************************************/

#include "precomp.h"

#include "WaitBlock.h"
#include "WaitQueue.h"

#include "ApiSorter.h"

#include "..\host\globals.h"
#include "..\host\utils.hpp"

#include "..\interactivity\inc\ServiceLocator.hpp"

// Routine Description:
// - Initializes a ConsoleWaitBlock
// - ConsoleWaitBlocks will self-manage their position in their two queues.
// - They will push themselves into the head and store the iterator for constant deletion time later.
// Arguments:
// - pProcessQueue - The queue attached to the client process ID that requested this action
// - pObjectQueue - The queue attached to the console object that will service the action when data arrives
// - pWaitReplyMessage - The original API message related to the client process's service request
// - pWaiter - The context to return to later when the wait is satisfied.
ConsoleWaitBlock::ConsoleWaitBlock(_In_ ConsoleWaitQueue* const pProcessQueue,
                                   _In_ ConsoleWaitQueue* const pObjectQueue,
                                   _In_ const CONSOLE_API_MSG* const pWaitReplyMessage,
                                   _In_ IWaitRoutine* const pWaiter) :
    _pProcessQueue(THROW_HR_IF_NULL(E_INVALIDARG, pProcessQueue)),
    _pObjectQueue(THROW_HR_IF_NULL(E_INVALIDARG, pObjectQueue)),
    _pWaiter(pWaiter)
{

    _pProcessQueue->_blocks.push_front(this);
    _itProcessQueue = _pProcessQueue->_blocks.cbegin();

    _pObjectQueue->_blocks.push_front(this);
    _itObjectQueue = _pObjectQueue->_blocks.cbegin();

    _WaitReplyMessage = *pWaitReplyMessage;

    // We will write the original message back (with updated out parameters/payload) when the request is finally serviced.
    if (pWaitReplyMessage->Complete.Write.Data != nullptr)
    {
        _WaitReplyMessage.Complete.Write.Data = &_WaitReplyMessage.u;
    }
}

// Routine Description:
// - Destroys a ConsolewaitBlock
// - On deletion, ConsoleWaitBlocks will erase themselves from the process and object queues in
//   constant time with the iterator acquired on construction.
ConsoleWaitBlock::~ConsoleWaitBlock()
{
    _pProcessQueue->_blocks.erase(_itProcessQueue);
    _pObjectQueue->_blocks.erase(_itObjectQueue);

    if (_pWaiter != nullptr)
    {
        delete _pWaiter;
    }
}

// Routine Description:
// - Creates and enqueues a new wait for later callback when a routine cannot be serviced at this time.
// - Will extract the process ID and the target object, enqueuing in both to know when to callback
// Arguments:
// - pWaitReplyMessage - The original API message from the client asking for servicing
// - pWaiter - The context/callback information to restore and dispatch the call later.
// Return Value:
// - S_OK if queued and ready to go. Appropriate HRESULT value if it failed.
HRESULT ConsoleWaitBlock::s_CreateWait(_Inout_ CONSOLE_API_MSG* const pWaitReplyMessage,
                                       _In_ IWaitRoutine* const pWaiter)
{
    HRESULT hr = S_OK;

    ConsoleProcessHandle* const ProcessData = pWaitReplyMessage->GetProcessHandle();
    assert(ProcessData != nullptr);

    ConsoleWaitQueue* const pProcessQueue = ProcessData->pWaitBlockQueue.get();

    ConsoleHandleData* const pHandleData = pWaitReplyMessage->GetObjectHandle();
    assert(pHandleData != nullptr);

    ConsoleWaitQueue* pObjectQueue = nullptr;
    pHandleData->GetWaitQueue(&pObjectQueue);
    assert(pObjectQueue != nullptr);

    ConsoleWaitBlock* pWaitBlock;
    try
    {
        pWaitBlock = new ConsoleWaitBlock(pProcessQueue,
                                          pObjectQueue,
                                          pWaitReplyMessage,
                                          pWaiter);

        THROW_IF_NULL_ALLOC(pWaitBlock);
    }
    catch (...)
    {
        hr = wil::ResultFromCaughtException();
    }

    if (FAILED(hr))
    {
        pWaitReplyMessage->SetReplyStatus(NTSTATUS_FROM_HRESULT(hr));
    }

    return hr;
}

// Routine Description:
// - Used to trigger the callback routine inside this wait block.
// Arguments:
// - TerminationReason - A reason to tell the callback to terminate early or 0 if it should operate normally.
// Return Value:
// - True if the routine was able to successfully return data (or terminate). False otherwise.
bool ConsoleWaitBlock::Notify(_In_ WaitTerminationReason const TerminationReason)
{
    bool fRetVal;

    NTSTATUS status;
    DWORD dwNumBytes;
    DWORD dwControlKeyState;
    BOOLEAN fIsUnicode = TRUE;

    std::deque<std::unique_ptr<IInputEvent>> outEvents;
    void* pOutputData = nullptr;
    // 1. Get unicode status of notify call based on message type.
    // We still need to know the Unicode status on reads as they will be converted after the wait operation.
    // Writes will have been converted before hitting the wait state.
    switch (_WaitReplyMessage.msgHeader.ApiNumber)
    {
    case API_NUMBER_GETCONSOLEINPUT:
    {
        CONSOLE_GETCONSOLEINPUT_MSG* a = &(_WaitReplyMessage.u.consoleMsgL1.GetConsoleInput);
        fIsUnicode = a->Unicode;
        pOutputData = &outEvents;
        break;
    }
    case API_NUMBER_READCONSOLE:
    {
        CONSOLE_READCONSOLE_MSG* a = &(_WaitReplyMessage.u.consoleMsgL1.ReadConsole);
        fIsUnicode = a->Unicode;
        break;
    }
    case API_NUMBER_WRITECONSOLE:
    {
        fIsUnicode = TRUE; // write always unicode now. we translated to Unicode before the wait occurred.
        break;
    }
    default:
    {
        assert(false); // we shouldn't be getting a wait/notify on API numbers we don't support.
        break;
    }
    }

    // 2. If we have a waiter, dispatch to it.
    if (_pWaiter->Notify(TerminationReason, fIsUnicode, &status, &dwNumBytes, &dwControlKeyState, pOutputData))
    {
        // 3. If the wait was successful, set reply info and attach any additional return information that this request type might need.
        _WaitReplyMessage.SetReplyStatus(status);
        _WaitReplyMessage.SetReplyInformation(dwNumBytes);

        if (API_NUMBER_GETCONSOLEINPUT == _WaitReplyMessage.msgHeader.ApiNumber)
        {
            // ReadConsoleInput/PeekConsoleInput has this extra reply
            // information with the number of records, not number of
            // bytes.
            CONSOLE_GETCONSOLEINPUT_MSG* a = &(_WaitReplyMessage.u.consoleMsgL1.GetConsoleInput);

            void* buffer;
            ULONG cbBuffer;
            HRESULT hr = _WaitReplyMessage.GetOutputBuffer(&buffer, &cbBuffer);
            if (FAILED(hr))
            {
                return false;
            }

            INPUT_RECORD* const pRecordBuffer = static_cast<INPUT_RECORD* const>(buffer);
            a->NumRecords = static_cast<ULONG>(outEvents.size());
            for (size_t i = 0; i < a->NumRecords; ++i)
            {
                if (outEvents.empty())
                {
                    break;
                }
                pRecordBuffer[i] = outEvents.front()->ToInputRecord();
                outEvents.pop_front();
            }

        }
        else if (API_NUMBER_READCONSOLE == _WaitReplyMessage.msgHeader.ApiNumber)
        {
            // ReadConsole has this extra reply information with the control key state.
            CONSOLE_READCONSOLE_MSG* a = &(_WaitReplyMessage.u.consoleMsgL1.ReadConsole);
            a->ControlKeyState = dwControlKeyState;
            a->NumBytes = dwNumBytes;

            // - This routine is called when a ReadConsole or ReadFile request is about to be completed.
            // - It sets the number of bytes written as the information to be written with the completion status and,
            //   if CTRL+Z processing is enabled and a CTRL+Z is detected, switches the number of bytes read to zero.
            if (a->ProcessControlZ != FALSE &&
                a->NumBytes > 0 &&
                _WaitReplyMessage.State.OutputBuffer != nullptr &&
                *(PUCHAR)_WaitReplyMessage.State.OutputBuffer == 0x1a)
            {
                // On changing this, we also need to notify the Reply Information because it was stowed above into the reply packet.
                a->NumBytes = 0;
                // Setting the reply length to 0 and returning successfully from a blocked wait
                // will imply that the user has reached "End of File" on a raw read file stream.
                _WaitReplyMessage.SetReplyInformation(0);
            }
        }
        // There is nothing to tell WriteConsole on the way out, that's why it doesn't have an else if case here.

        _WaitReplyMessage.ReleaseMessageBuffers();

        LOG_IF_FAILED(ServiceLocator::LocateGlobals()->pDeviceComm->CompleteIo(&_WaitReplyMessage.Complete));

        fRetVal = true;
    }
    else
    {
        // If fThreadDying is TRUE we need to make sure that we removed the pWaitBlock from the list (which we don't do on this branch).
        assert(IsFlagClear(TerminationReason, WaitTerminationReason::ThreadDying));
        fRetVal = false;
    }

    return fRetVal;
}
