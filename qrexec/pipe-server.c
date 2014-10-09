#include "pipe-server.h"

extern HANDLE g_hStopServiceEvent;

CRITICAL_SECTION g_PipesCriticalSection;
PIPEINST g_Pipes[INSTANCES];
HANDLE g_hEvents[INSTANCES + 1];

ULONG64	g_uDaemonRequestsCounter = 1;

ULONG CreatePipeSecurityDescriptor(SECURITY_DESCRIPTOR **ppPipeSecurityDescriptor, ACL **ppACL)
{
    ULONG uResult;
    SID *pEveryoneSid = NULL;
    SID *pAdminSID = NULL;
    ACL *pACL = NULL;
    SECURITY_DESCRIPTOR *pSD = NULL;
    EXPLICIT_ACCESS	ea[2];
    SID_IDENTIFIER_AUTHORITY SIDAuthWorld = SECURITY_WORLD_SID_AUTHORITY;

    if (!ppPipeSecurityDescriptor || !ppACL)
        return ERROR_INVALID_PARAMETER;

    *ppPipeSecurityDescriptor = NULL;
    *ppACL = NULL;

    // Create a well-known SID for the Everyone group.
    if (!AllocateAndInitializeSid(
        &SIDAuthWorld,
        1,
        SECURITY_WORLD_RID,
        0, 0, 0, 0, 0, 0, 0,
        &pEveryoneSid))
    {
        return perror("AllocateAndInitializeSid");
    }

    // Initialize an EXPLICIT_ACCESS structure for an ACE.
    // The ACE will allow Everyone read/write access to the pipe.
    memset(&ea, 0, sizeof(ea));
    ea[0].grfAccessPermissions = FILE_GENERIC_READ | FILE_GENERIC_WRITE | FILE_CREATE_PIPE_INSTANCE | SYNCHRONIZE;
    ea[0].grfAccessMode = SET_ACCESS;
    ea[0].grfInheritance = NO_INHERITANCE;
    ea[0].Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea[0].Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    ea[0].Trustee.ptstrName = (TCHAR *) pEveryoneSid;

    // Create a new ACL that contains the new ACE.
    uResult = SetEntriesInAcl(1, ea, NULL, &pACL);
    FreeSid(pEveryoneSid);

    if (ERROR_SUCCESS != uResult)
    {
        SetLastError(uResult);
        return perror("SetEntriesInAcl");
    }

    // Initialize a security descriptor.
    pSD = (SECURITY_DESCRIPTOR *) LocalAlloc(LPTR, SECURITY_DESCRIPTOR_MIN_LENGTH);
    if (!pSD)
    {
        LocalFree(pACL);
        return perror("LocalAlloc");
    }

    if (!InitializeSecurityDescriptor(pSD, SECURITY_DESCRIPTOR_REVISION))
    {
        uResult = GetLastError();

        LocalFree(pACL);
        LocalFree(pSD);

        return perror2(uResult, "InitializeSecurityDescriptor");
    }

    // Add the ACL to the security descriptor.
    if (!SetSecurityDescriptorDacl(pSD, TRUE, pACL, FALSE))
    {
        uResult = GetLastError();

        LocalFree(pACL);
        LocalFree(pSD);

        return perror2(uResult, "SetSecurityDescriptorDacl");
    }

    *ppPipeSecurityDescriptor = pSD;
    *ppACL = pACL;

    return ERROR_SUCCESS;
}

// This function is called to start an overlapped connect operation.
// It sets *pbPendingIO to TRUE if an operation is pending or to FALSE if the
// connection has been completed.
ULONG ConnectToNewClient(HANDLE hPipe, LPOVERLAPPED lpo, HANDLE hEvent, BOOLEAN *pbPendingIO)
{
    BOOLEAN	bPendingIO = FALSE;

    if (!pbPendingIO)
        return ERROR_INVALID_PARAMETER;

    memset(lpo, 0, sizeof(OVERLAPPED));
    lpo->hEvent = hEvent;

    // Start an overlapped connection for this pipe instance.
    if (ConnectNamedPipe(hPipe, lpo))
    {
        return perror("ConnectNamedPipe");
    }

    switch (GetLastError())
    {
        // The overlapped connection in progress.
    case ERROR_IO_PENDING:
        bPendingIO = TRUE;
        break;

        // Client is already connected, so signal an event.
    case ERROR_PIPE_CONNECTED:
        SetEvent(lpo->hEvent);
        break;

        // If an error occurs during the connect operation
    default:
        return perror("ConnectNamedPipe");
    }

    *pbPendingIO = bPendingIO;
    return ERROR_SUCCESS;
}

// This function is called:
// - when an error occurs;
// - when the client closes its handle to the pipe;
// - when the server disconnects from the client.
// Disconnect from this client, then call ConnectNamedPipe to wait for another client to connect.
ULONG DisconnectAndReconnect(ULONG i)
{
    ULONG uResult;

    LogDebug("Disconnecting pipe %d, state %d\n", i, g_Pipes[i].uState);

    if (g_Pipes[i].hClientProcess)
        CloseHandle(g_Pipes[i].hClientProcess);
    g_Pipes[i].hClientProcess = 0;

    g_Pipes[i].CreateProcessResponse.bType = CPR_TYPE_NONE;

    if (g_Pipes[i].ClientInfo.hWriteStdinPipe)
        CloseHandle(g_Pipes[i].ClientInfo.hWriteStdinPipe);

    // There is no IO going in these pipes, so we can safely pass any
    // client_id to CloseReadPipeHandles - it will not be used anywhere.
    // Once a pipe becomes watched, these handles are moved to g_Clients,
    // and these structures are zeroed.
    if (g_Pipes[i].ClientInfo.Stdout.hReadPipe)
        CloseReadPipeHandles(-1, &g_Pipes[i].ClientInfo.Stdout);

    if (g_Pipes[i].ClientInfo.Stderr.hReadPipe)
        CloseReadPipeHandles(-1, &g_Pipes[i].ClientInfo.Stderr);

    memset(&g_Pipes[i].ClientInfo, 0, sizeof(g_Pipes[i].ClientInfo));
    memset(&g_Pipes[i].RemoteHandles, 0, sizeof(g_Pipes[i].RemoteHandles));
    memset(&g_Pipes[i].params, 0, sizeof(g_Pipes[i].params));
    g_Pipes[i].assigned_client_id = 0;

    // Disconnect the pipe instance.

    if (!DisconnectNamedPipe(g_Pipes[i].hPipeInst))
    {
        return perror("DisconnectNamedPipe");
    }

    // Call a subroutine to connect to the new client.

    uResult = ConnectToNewClient(g_Pipes[i].hPipeInst, &g_Pipes[i].oOverlapped, g_hEvents[i], &g_Pipes[i].fPendingIO);
    if (ERROR_SUCCESS != uResult)
    {
        return perror2(uResult, "ConnectToNewClient");
    }

    g_Pipes[i].uState = g_Pipes[i].fPendingIO ? STATE_WAITING_FOR_CLIENT : STATE_SENDING_IO_HANDLES;
    return ERROR_SUCCESS;
}

ULONG ClosePipeHandles()
{
    ULONG i;

    for (i = 0; i < INSTANCES; i++)
    {
        if (g_Pipes[i].fPendingIO)
        {
            if (CancelIo(g_Pipes[i].hPipeInst))
            {
                // Must wait for the canceled IO to complete, otherwise a race condition may occur on the
                // OVERLAPPED structure.
                WaitForSingleObject(g_Pipes[i].oOverlapped.hEvent, INFINITE);
            }
            else
            {
                perror("CancelIo");
            }
        }

        CloseHandle(g_hEvents[i]);
    }

    return ERROR_SUCCESS;
}

ULONG ConnectExisting(int client_id, HANDLE hClientProcess, CLIENT_INFO *pClientInfo, struct trigger_connect_params *pparams, CREATE_PROCESS_RESPONSE *pCpr)
{
    ULONG uResult;

    if (!pClientInfo || !pparams || !pCpr)
        return ERROR_INVALID_PARAMETER;

    LogDebug("client_id %d: Got the params '%S', vm '%S'\n", client_id, pparams->exec_index, pparams->target_vmname);

    if (CPR_TYPE_ERROR_CODE == pCpr->bType)
    {
        LogWarning("client_id %d: Process creation failed, got the error code %d\n", client_id, pCpr->ResponseData.dwErrorCode);

        uResult = send_exit_code(client_id, MAKE_ERROR_RESPONSE(ERROR_SET_WINDOWS, pCpr->ResponseData.dwErrorCode));
        if (ERROR_SUCCESS != uResult)
            return perror2(uResult, "send_exit_code");

        return uResult;
    }

    if (!DuplicateHandle(
        hClientProcess,
        pCpr->ResponseData.hProcess,
        GetCurrentProcess(),
        &pClientInfo->hProcess,
        0,
        TRUE,
        DUPLICATE_SAME_ACCESS))
    {
        return perror("DuplicateHandle");
    }

    SetLastError(uResult = AddExistingClient(client_id, pClientInfo));
    if (ERROR_SUCCESS != uResult)
    {
        return perror("AddExistingClient");
        // DisconnectAndReconnect will close all the handles later
    }

    // Clear the handles; now the WatchForEvents thread takes care of them.
    memset(pClientInfo, 0, sizeof(CLIENT_INFO));

    return ERROR_SUCCESS;
}

// This routine will be called by a single thread only (WatchForTriggerEvents thread),
// and this is the only place where g_uDaemonRequestsCounter is read and written, so
// there is no need to do InterlockedIncrement on the counter.

// However, g_Pipes list may be accessed by another thread (WatchForEvents thread),
// so it must be locked. This access happens when a daemon responds with a CONNECT_EXISTING
// message.
ULONG SendParametersToDaemon(ULONG i)
{
    HRESULT hResult;
    ULONG uResult;
    struct trigger_connect_params params;

    if (i >= INSTANCES)
        return ERROR_INVALID_PARAMETER;

    EnterCriticalSection(&g_PipesCriticalSection);

    hResult = StringCchPrintfA(
        (STRSAFE_LPSTR) &g_Pipes[i].params.process_fds.ident,
        sizeof(g_Pipes[i].params.process_fds.ident),
        "%I64x",
        g_uDaemonRequestsCounter++);

    if (FAILED(hResult))
    {
        perror2(hResult, "StringCchPrintfA");
        LeaveCriticalSection(&g_PipesCriticalSection);
        return hResult;
    }

    params = g_Pipes[i].params;

    LeaveCriticalSection(&g_PipesCriticalSection);

    uResult = ReturnData(0, MSG_AGENT_TO_SERVER_TRIGGER_CONNECT_EXISTING, &params, sizeof(params), NULL);
    if (ERROR_SUCCESS != uResult)
    {
        return perror2(uResult, "ReturnData");
    }

    return ERROR_SUCCESS;
}

ULONG FindPipeByIdent(UCHAR *pszIdent, ULONG *puPipeNumber)
{
    ULONG i;

    if (!pszIdent || !puPipeNumber)
        return ERROR_INVALID_PARAMETER;

    for (i = 0; i < INSTANCES; i++)
    {
        if (!strcmp(g_Pipes[i].params.process_fds.ident, pszIdent))
        {
            *puPipeNumber = i;
            return ERROR_SUCCESS;
        }
    }

    return ERROR_NOT_FOUND;
}

ULONG ProceedWithExecution(int assigned_client_id, UCHAR *pszIdent)
{
    ULONG uPipeNumber;
    ULONG uResult;

    if (!pszIdent)
        return ERROR_INVALID_PARAMETER;

    EnterCriticalSection(&g_PipesCriticalSection);

    uResult = FindPipeByIdent(pszIdent, &uPipeNumber);
    if (ERROR_SUCCESS != uResult)
    {
        perror2(uResult, "FindPipeByIdent");
        LogError("id=%s", pszIdent);
        LeaveCriticalSection(&g_PipesCriticalSection);
        return uResult;
    }

    if (STATE_WAITING_FOR_DAEMON_DECISION != g_Pipes[uPipeNumber].uState)
    {
        LogWarning("Wrong pipe state %d, should be %d\n", g_Pipes[uPipeNumber].uState, STATE_WAITING_FOR_DAEMON_DECISION);
        LeaveCriticalSection(&g_PipesCriticalSection);
        return ERROR_INVALID_PARAMETER;
    }

    g_Pipes[uPipeNumber].assigned_client_id = assigned_client_id;

    // Signalize that we're allowed to send io handles to qrexec_client_vm.
    SetEvent(g_hEvents[uPipeNumber]);

    LeaveCriticalSection(&g_PipesCriticalSection);

    return ERROR_SUCCESS;
}

ULONG WINAPI WatchForTriggerEvents(void *pParam)
{
    DWORD dwWait, cbRet, cbToWrite, cbRead;
    ULONG i;
    ULONG uResult;
    BOOL fSuccess;
    IO_HANDLES_ARRAY LocalHandles;
    ULONG uClientProcessId;
    SECURITY_ATTRIBUTES sa;
    SECURITY_DESCRIPTOR *pPipeSecurityDescriptor;
    ACL *pACL;

    LogDebug("Init\n");
    memset(&g_Pipes, 0, sizeof(g_Pipes));

    uResult = CreatePipeSecurityDescriptor(&pPipeSecurityDescriptor, &pACL);
    if (ERROR_SUCCESS != uResult)
    {
        return perror2(uResult, "CreatePipeSecurityDescriptor");
    }

    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = FALSE;
    sa.lpSecurityDescriptor = pPipeSecurityDescriptor;

    // The initial loop creates several instances of a named pipe
    // along with an event object for each instance. An
    // overlapped ConnectNamedPipe operation is started for
    // each instance.

    for (i = 0; i < INSTANCES; i++)
    {
        // Create an event object for this instance.

        g_hEvents[i] = CreateEvent(
            NULL, // default security attribute
            FALSE, // auto-reset event
            FALSE, // initial state = not signaled
            NULL); // unnamed event object

        if (g_hEvents[i] == NULL)
        {
            uResult = GetLastError();
            LocalFree(pPipeSecurityDescriptor);
            LocalFree(pACL);
            return perror2(uResult, "CreateEvent");
        }

        g_Pipes[i].hPipeInst = CreateNamedPipe(
            TRIGGER_PIPE_NAME,
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            INSTANCES, // number of instances
            4096, // output buffer size
            4096, // input buffer size
            PIPE_TIMEOUT, // client time-out
            &sa);

        if (INVALID_HANDLE_VALUE == g_Pipes[i].hPipeInst)
        {
            uResult = GetLastError();
            LocalFree(pPipeSecurityDescriptor);
            LocalFree(pACL);
            return perror2(uResult, "CreateNamedPipe");
        }

        // Call the subroutine to connect to the new client

        uResult = ConnectToNewClient(
            g_Pipes[i].hPipeInst,
            &g_Pipes[i].oOverlapped,
            g_hEvents[i],
            &g_Pipes[i].fPendingIO);

        if (ERROR_SUCCESS != uResult)
        {
            LocalFree(pPipeSecurityDescriptor);
            LocalFree(pACL);
            return perror2(uResult, "ConnectToNewClient");
        }

        g_Pipes[i].uState = g_Pipes[i].fPendingIO ? STATE_WAITING_FOR_CLIENT : STATE_SENDING_IO_HANDLES;
    }

    LocalFree(pPipeSecurityDescriptor);
    LocalFree(pACL);

    // Last one will signal the service shutdown.
    g_hEvents[INSTANCES] = g_hStopServiceEvent;

    while (TRUE)
    {
        // Wait for the event object to be signaled, indicating
        // completion of an overlapped read, write, or
        // connect operation.

        dwWait = WaitForMultipleObjects(
            INSTANCES + 1, // number of event objects
            g_hEvents, // array of event objects
            FALSE, // does not wait for all
            INFINITE); // waits indefinitely

        // dwWait shows which pipe completed the operation.

        i = dwWait - WAIT_OBJECT_0; // determines which pipe

        if (INSTANCES == i)
        {
            // Service is shuttiung down, close the pipe handles.
            LogInfo("Shutting down\n");
            ClosePipeHandles();

            return ERROR_SUCCESS;
        }

        if (i > (INSTANCES - 1))
        {
            return perror("WaitForMultipleObjects");
        }

        //lprintf("signaled pipe %d, original state %d\n", i, g_Pipes[i].uState);

        // Get the result of the pending operation that has just finished.
        if (g_Pipes[i].fPendingIO)
        {
            if (!GetOverlappedResult(g_Pipes[i].hPipeInst, &g_Pipes[i].oOverlapped, &cbRet, FALSE))
            {
                perror("GetOverlappedResult");
                DisconnectAndReconnect(i);
                continue;
            }

            // Clear the pending operation flag.
            g_Pipes[i].fPendingIO = FALSE;

            switch (g_Pipes[i].uState)
            {
                // Pending connect operation
            case STATE_WAITING_FOR_CLIENT:

                if (!GetNamedPipeClientProcessId(g_Pipes[i].hPipeInst, &uClientProcessId))
                {
                    perror("GetNamedPipeClientProcessId");
                    DisconnectAndReconnect(i);
                    continue;
                }

                LogDebug("STATE_WAITING_FOR_CLIENT (pending): Accepted connection from the process %d\n", uClientProcessId);

                g_Pipes[i].hClientProcess = OpenProcess(PROCESS_DUP_HANDLE, FALSE, uClientProcessId);
                if (!g_Pipes[i].hClientProcess)
                {
                    perror("OpenProcess");
                    DisconnectAndReconnect(i);
                    continue;
                }

                g_Pipes[i].uState = STATE_RECEIVING_PARAMETERS;
                break;

                // Make sure the incoming message has a right size
            case STATE_RECEIVING_PARAMETERS:
                if (sizeof(g_Pipes[i].params) != cbRet)
                {
                    LogWarning("Wrong incoming parameter size: %d instead of %d\n", cbRet, sizeof(g_Pipes[i].params));
                    DisconnectAndReconnect(i);
                    continue;
                }

                LogDebug("STATE_RECEIVING_PARAMETERS (pending): Received the parameters, sending them to the daemon\n");
                uResult = SendParametersToDaemon(i);
                if (ERROR_SUCCESS != uResult)
                {
                    perror2(uResult, "SendParametersToDaemon");
                    DisconnectAndReconnect(i);
                    continue;
                }
                g_Pipes[i].uState = STATE_WAITING_FOR_DAEMON_DECISION;
                continue;

                // Pending write operation
            case STATE_SENDING_IO_HANDLES:
                if (IO_HANDLES_ARRAY_SIZE != cbRet)
                {
                    LogWarning("Could not send the handles array: sent %d bytes instead of %d\n", cbRet, IO_HANDLES_ARRAY_SIZE);
                    DisconnectAndReconnect(i);
                    continue;
                }

                LogDebug("STATE_SENDING_IO_HANDLES (pending): IO handles have been sent, waiting for the process handle\n");
                g_Pipes[i].uState = STATE_RECEIVING_PROCESS_HANDLE;
                continue;

                // Pending read operation
            case STATE_RECEIVING_PROCESS_HANDLE:
                if (sizeof(CREATE_PROCESS_RESPONSE) != cbRet)
                {
                    LogWarning("Wrong incoming create process response size: %d\n", cbRet);
                    DisconnectAndReconnect(i);
                    continue;
                }

                LogDebug("STATE_RECEIVING_PROCESS_HANDLE (pending): Received the create process response\n");

                uResult = ConnectExisting(
                    g_Pipes[i].assigned_client_id,
                    g_Pipes[i].hClientProcess,
                    &g_Pipes[i].ClientInfo,
                    &g_Pipes[i].params,
                    &g_Pipes[i].CreateProcessResponse);
                if (ERROR_SUCCESS != uResult)
                    perror2(uResult, "ConnectExisting");

                DisconnectAndReconnect(i);
                continue;

            default:
                LogWarning("Invalid pipe state %d\n", g_Pipes[i].uState);
                continue;
            }
        }

        //lprintf("pipe %d, state %d\n", i, g_Pipes[i].uState);

        // The pipe state determines which operation to do next.
        switch (g_Pipes[i].uState)
        {
        case STATE_RECEIVING_PARAMETERS:

            fSuccess = ReadFile(
                g_Pipes[i].hPipeInst,
                &g_Pipes[i].params,
                sizeof(g_Pipes[i].params),
                &cbRead,
                &g_Pipes[i].oOverlapped);

            // The read operation completed successfully.

            if (fSuccess && sizeof(g_Pipes[i].params) == cbRead)
            {
                // g_hEvents[i] is in the signaled state here, so we must reset it before sending anything to the daemon.
                // If the daemon allows the execution then it will be signaled in ProceedWithExecution() later,
                // if not, the pipe will be disconnected.
                ResetEvent(g_hEvents[i]);

                // Change the pipe state before calling SendParametersToDaemon() because another thread may call
                // ProceedWithExecution() even before the current thread returns from SendParametersToDaemon().
                // ProceedWithExecution() checks the pipe state to be STATE_WAITING_FOR_DAEMON_DECISION.
                g_Pipes[i].fPendingIO = FALSE;
                g_Pipes[i].uState = STATE_WAITING_FOR_DAEMON_DECISION;

                LogDebug("STATE_RECEIVING_PARAMETERS: Immediately got the params %S, %S\n", g_Pipes[i].params.exec_index, g_Pipes[i].params.target_vmname);

                uResult = SendParametersToDaemon(i);
                if (ERROR_SUCCESS != uResult)
                {
                    perror2(uResult, "SendParametersToDaemon");
                    DisconnectAndReconnect(i);
                    continue;
                }

                continue;
            }

            // The read operation is still pending.

            uResult = GetLastError();
            if (!fSuccess && (ERROR_IO_PENDING == uResult))
            {
                LogDebug("STATE_RECEIVING_PARAMETERS: Read is pending\n");
                g_Pipes[i].fPendingIO = TRUE;
                continue;
            }

            // An error occurred; disconnect from the client.
            perror2(uResult, "STATE_RECEIVING_PARAMETERS: ReadFile");
            DisconnectAndReconnect(i);
            break;

        case STATE_WAITING_FOR_DAEMON_DECISION:
            LogDebug("STATE_WAITING_FOR_DAEMON_DECISION: Daemon allowed to proceed, sending the IO handles\n");
            // The pipe in this state should never have fPendingIO flag set.
            g_Pipes[i].uState = STATE_SENDING_IO_HANDLES;
            // passthrough

        case STATE_SENDING_IO_HANDLES:

            cbToWrite = IO_HANDLES_ARRAY_SIZE;

            uResult = CreateClientPipes(
                &g_Pipes[i].ClientInfo,
                &LocalHandles.hPipeStdin,
                &LocalHandles.hPipeStdout,
                &LocalHandles.hPipeStderr);

            if (ERROR_SUCCESS != uResult)
            {
                perror2(uResult, "CreateClientPipes");
                DisconnectAndReconnect(i);
                continue;
            }

            if (!DuplicateHandle(
                GetCurrentProcess(),
                LocalHandles.hPipeStdin,
                g_Pipes[i].hClientProcess,
                &g_Pipes[i].RemoteHandles.hPipeStdin,
                0,
                TRUE,
                DUPLICATE_SAME_ACCESS | DUPLICATE_CLOSE_SOURCE))
            {
                perror("DuplicateHandle(stdin)");
                CloseHandle(LocalHandles.hPipeStdout);
                CloseHandle(LocalHandles.hPipeStderr);
                DisconnectAndReconnect(i);
                continue;
            }

            if (!DuplicateHandle(
                GetCurrentProcess(),
                LocalHandles.hPipeStdout,
                g_Pipes[i].hClientProcess,
                &g_Pipes[i].RemoteHandles.hPipeStdout,
                0,
                TRUE,
                DUPLICATE_SAME_ACCESS | DUPLICATE_CLOSE_SOURCE))
            {
                perror("DuplicateHandle(stdout)");
                CloseHandle(LocalHandles.hPipeStderr);
                DisconnectAndReconnect(i);
                continue;
            }

            if (!DuplicateHandle(
                GetCurrentProcess(),
                LocalHandles.hPipeStderr,
                g_Pipes[i].hClientProcess,
                &g_Pipes[i].RemoteHandles.hPipeStderr,
                0,
                TRUE,
                DUPLICATE_SAME_ACCESS | DUPLICATE_CLOSE_SOURCE))
            {
                perror("DuplicateHandle(stderr)");
                DisconnectAndReconnect(i);
                continue;
            }

            fSuccess = WriteFile(
                g_Pipes[i].hPipeInst,
                &g_Pipes[i].RemoteHandles,
                IO_HANDLES_ARRAY_SIZE,
                &cbRet,
                &g_Pipes[i].oOverlapped);

            if (!fSuccess || IO_HANDLES_ARRAY_SIZE != cbRet)
            {
                // The write operation is still pending.

                uResult = GetLastError();
                if ((ERROR_IO_PENDING == uResult) && !fSuccess)
                {
                    LogDebug("STATE_SENDING_IO_HANDLES: Write is pending\n");
                    g_Pipes[i].fPendingIO = TRUE;
                    continue;
                }

                // An error occurred; disconnect from the client.
                perror("STATE_SENDING_IO_HANDLES: WriteFile");
                DisconnectAndReconnect(i);
                break;
            }

            // The write operation completed successfully.
            // g_hEvents[i] is in the signaled state here, but the upcoming ReadFile will change its state accordingly.
            LogDebug("STATE_SENDING_IO_HANDLES: IO handles have been sent, waiting for the process handle\n");
            g_Pipes[i].fPendingIO = FALSE;
            g_Pipes[i].uState = STATE_RECEIVING_PROCESS_HANDLE;
            // passthrough

        case STATE_RECEIVING_PROCESS_HANDLE:

            fSuccess = ReadFile(
                g_Pipes[i].hPipeInst,
                &g_Pipes[i].CreateProcessResponse,
                sizeof(CREATE_PROCESS_RESPONSE),
                &cbRead,
                &g_Pipes[i].oOverlapped);

            // The read operation completed successfully.

            if (fSuccess && sizeof(CREATE_PROCESS_RESPONSE) == cbRead)
            {
                LogDebug("STATE_RECEIVING_PROCESS_HANDLE: Received the create process response\n");

                uResult = ConnectExisting(
                    g_Pipes[i].assigned_client_id,
                    g_Pipes[i].hClientProcess,
                    &g_Pipes[i].ClientInfo,
                    &g_Pipes[i].params,
                    &g_Pipes[i].CreateProcessResponse);
                if (ERROR_SUCCESS != uResult)
                    perror2(uResult, "ConnectExisting");

                DisconnectAndReconnect(i);
                continue;
            }

            // The read operation is still pending.

            uResult = GetLastError();
            if (!fSuccess && (ERROR_IO_PENDING == uResult))
            {
                LogDebug("STATE_RECEIVING_PROCESS_HANDLE: Read is pending\n");
                g_Pipes[i].fPendingIO = TRUE;
                continue;
            }

            // An error occurred; disconnect from the client.
            perror("STATE_RECEIVING_PROCESS_HANDLE: ReadFile");
            DisconnectAndReconnect(i);
            break;

        default:
            LogWarning("Invalid pipe state");
            return ERROR_INVALID_PARAMETER;
        }
    }

    return ERROR_SUCCESS;
}
