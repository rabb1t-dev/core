#include "MaNGOSsoap.h"
#include "stdsoap2.h"

#include "World.h"
#include "Log.h"
#include "AccountMgr.h"

#include "IO/Networking/IpAddress.h"
#include "IO/Multithreading/CreateThread.h"

#include <chrono>
#include <thread>

// How long to wait between checks on a command that has been handed to the world thread. This only
// costs anything during a shutdown: a command that finishes normally wakes the wait immediately.
static constexpr int SOAP_COMMAND_POLL_INTERVAL_MS = 500;

// How many times to try claiming the listening port, one second apart. Only reached when a
// predecessor process still holds it, since TIME_WAIT is handled by SO_REUSEADDR below.
static constexpr int SOAP_BIND_ATTEMPTS = 10;

static char const* const SOAP_SHUTTING_DOWN_MESSAGE = "Server is shutting down, the command was not executed.";

class SOAPCommand
{
 public:
    enum class Outcome
    {
        Succeeded,
        Failed,
        Abandoned,  // the world stopped without running it, so no reply is ever coming
    };

    /// Blocks until OnCommandFinished is called, or gives up if the world stops without calling it.
    ///
    /// This was a bare get() on the future, which is only correct for as long as something is still
    /// draining the command queue. Commands run on the world thread, and Master::Run joins the
    /// world thread before the SOAP thread, so a request that lands in that window waits on a reply
    /// nobody is left to send -- and because it never leaves soap_serve, the join on this thread
    /// never returns either. That hung one shutdown for sixteen minutes with the maps already
    /// unloaded and every other thread parked.
    ///
    /// World::CancelQueuedCliCommands now answers the queue as the loop ends, which covers the
    /// ordinary case promptly. It cannot cover a command queued after it has already run, so the
    /// wait still has to be able to give up on its own.
    Outcome Wait()
    {
        std::future<bool> future = m_successStatusPromise.get_future();

        while (true)
        {
            if (future.wait_for(std::chrono::milliseconds(SOAP_COMMAND_POLL_INTERVAL_MS)) == std::future_status::ready)
                return future.get() ? Outcome::Succeeded : Outcome::Failed;

            // Only consulted after a wait that timed out, so a command that beat us to completion
            // is still reported as completed on the next turn of the loop.
            if (World::IsStopped())
                return Outcome::Abandoned;
        }
    }

    static void OnPrint(void* opaquePointer, char const* msg)
    {
        SOAPCommand* self = static_cast<SOAPCommand*>(opaquePointer);
        self->m_printBuffer += msg;
    }

    static void OnCommandFinished(void* opaquePointer, bool success)
    {
        SOAPCommand* self = static_cast<SOAPCommand*>(opaquePointer);
        self->m_successStatusPromise.set_value(success);
    }

    std::string m_printBuffer;
    std::promise<bool> m_successStatusPromise;
};

void SoapThreadBody(struct soap* soap)
{
    while (!World::IsStopped())
    {
        if (!soap_valid_socket(soap_accept(soap)))
            continue; // most likely, we ran into an accept timeout

        auto ip = IO::Networking::IpAddress::FromIpv4Uint32(soap->ip);
        sLog.Out(LOG_RA, LOG_LVL_BASIC, "MaNGOSsoap: Accepted connection from %s", ip.ToString().c_str());

        soap_serve(soap); // handle soap request
    }

    sLog.Out(LOG_RA, LOG_LVL_MINIMAL, "MaNGOSsoap: Stopping...");
    soap_end(soap);
    soap_done(soap);
    soap_destroy(soap);
}

std::unique_ptr<std::thread> StartSoapThread(std::string const& bindHost, uint16 bindPort)
{
    struct soap* soap = soap_new();
    soap_init(soap);
    soap_set_imode(soap, SOAP_C_UTFSTRING);
    soap_set_omode(soap, SOAP_C_UTFSTRING);

    soap->accept_timeout = 3; // sec | Check every 3 seconds if World::IsStopped()
    soap->recv_timeout = 5; // sec
    soap->send_timeout = 5; // sec

    int const acceptBacklogCount = 50;

    // SO_REUSEADDR, because losing this port across a restart was the default outcome rather than
    // an unlucky one. gSOAP initialises bind_flags to 0 and only calls setsockopt when it is set,
    // so without this the bind fails while any socket with local port bindPort sits in TIME_WAIT.
    // `.server restart` arrives over SOAP, which guarantees such a socket exists at the moment the
    // replacement process starts, so the usual way to restart the server was also a reliable way
    // to come back up without it.
    soap->bind_flags = SO_REUSEADDR;

    // A predecessor that is still alive and holding the listening socket is not covered by
    // SO_REUSEADDR, so wait for it briefly instead of giving up for the lifetime of the process.
    // The old behaviour was a single attempt, and the failure is close to invisible: the server
    // carries on and reports healthy and active with no remote administration at all, and the port
    // then reads as free, which makes it look like nothing was ever wrong.
    int boundSocket = SOAP_INVALID_SOCKET;
    for (int attempt = 1; attempt <= SOAP_BIND_ATTEMPTS; ++attempt)
    {
        boundSocket = soap_bind(soap, bindHost.c_str(), bindPort, acceptBacklogCount);
        if (soap_valid_socket(boundSocket))
            break;

        if (attempt < SOAP_BIND_ATTEMPTS)
        {
            sLog.Out(LOG_RA, LOG_LVL_MINIMAL, "MaNGOSsoap: %s:%d still in use, retrying (attempt %d of %d)",
                     bindHost.c_str(), bindPort, attempt, SOAP_BIND_ATTEMPTS);
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    if (!soap_valid_socket(boundSocket))
    {
        sLog.Out(LOG_RA, LOG_LVL_ERROR, "MaNGOSsoap: Couldn't bind to %s:%d after %d attempts, remote administration is unavailable",
                 bindHost.c_str(), bindPort, SOAP_BIND_ATTEMPTS);
        soap_done(soap);
        soap_destroy(soap);
        return nullptr;
    }

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "MaNGOSsoap: Bound to http://%s:%d/", bindHost.c_str(), bindPort);

    return IO::Multithreading::CreateThreadPtr("SOAP", [soap]()
    { SoapThreadBody(soap); });
}

/// Defined by soap.stub
int ns1__executeCommand(soap* soap, char* command, char** result)
{
    // security check
    if (!soap->userid || !soap->passwd)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_DETAIL, "MaNGOSsoap: Client didn't provide login information");
        return 401;
    }

    uint32 accountId = sAccountMgr.GetId(soap->userid);
    if (!accountId)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_DETAIL, "MaNGOSsoap: Client used invalid username '%s'", soap->userid);
        return 401;
    }

    if (!sAccountMgr.CheckPassword(accountId, soap->passwd))
    {
        sLog.Out(LOG_BASIC, LOG_LVL_DETAIL, "MaNGOSsoap: invalid password for account '%s'", soap->userid);
        return 401;
    }

    if (sAccountMgr.GetSecurity(accountId) < SEC_ADMINISTRATOR)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_DETAIL, "MaNGOSsoap: %s's gmlevel is too low", soap->userid);
        return 403;
    }

    if (!command || !*command)
        return soap_sender_fault(soap, "Parameter 'command' can not be empty", "The supplied command was an empty string");

    sLog.Out(LOG_BASIC, LOG_LVL_DEBUG, "MaNGOSsoap: Received command '%s'", command);

    // Refused outright rather than queued, because the world loop that would run it has already
    // gone and World::~World frees the queue without answering anything left in it.
    if (World::IsStopped())
    {
        char* message = soap_strdup(soap, SOAP_SHUTTING_DOWN_MESSAGE);
        return soap_sender_fault(soap, message, message);
    }

    // Commands are executed in the world thread. We have to wait for them to be completed.
    //
    // On the heap, because the give-up path below must not destroy it: the world thread can still
    // be holding this pointer and may complete the command a moment after we stop waiting. A stack
    // object would turn that race into a write through a dangling pointer.
    SOAPCommand* commandHolder = new SOAPCommand();
    {
        // CliCommandHolder will be deleted from world, accessing after queueing is NOT safe
        CliCommandHolder* cmd = new CliCommandHolder(accountId, SEC_CONSOLE, commandHolder, command, &SOAPCommand::OnPrint, &SOAPCommand::OnCommandFinished);
        sWorld.QueueCliCommand(cmd);
    }

    // Wait for callback to complete command
    SOAPCommand::Outcome const outcome = commandHolder->Wait();

    if (outcome == SOAPCommand::Outcome::Abandoned)
    {
        // Deliberately leaked. The queued command still points at it, nothing will come back to
        // tell us when that stops being true, and the alternative is the dangling write described
        // above. One small allocation per stranded command, in a process that is already exiting.
        sLog.Out(LOG_RA, LOG_LVL_MINIMAL, "MaNGOSsoap: World stopped before command '%s' could run", command);

        char* message = soap_strdup(soap, SOAP_SHUTTING_DOWN_MESSAGE);
        return soap_sender_fault(soap, message, message);
    }

    char* printBuffer = soap_strdup(soap, commandHolder->m_printBuffer.c_str());
    bool const wasSuccessful = (outcome == SOAPCommand::Outcome::Succeeded);
    delete commandHolder;

    if (!wasSuccessful)
        return soap_sender_fault(soap, printBuffer, printBuffer);

    *result = printBuffer;
    return SOAP_OK;
}

/// Namespace definition for gSOAP.
/// We must define this, because gSOAP is using it as an external symbol
struct Namespace namespaces[] =
    {{ "SOAP-ENV", "http://schemas.xmlsoap.org/soap/envelope/" }, // must be first
     { "SOAP-ENC", "http://schemas.xmlsoap.org/soap/encoding/" }, // must be second
     { "xsi", "http://www.w3.org/1999/XMLSchema-instance", "http://www.w3.org/*/XMLSchema-instance" },
     { "xsd", "http://www.w3.org/1999/XMLSchema", "http://www.w3.org/*/XMLSchema" },
     { "ns1", "urn:MaNGOS" },     // "ns1" namespace prefix
     { nullptr, nullptr }
    };
