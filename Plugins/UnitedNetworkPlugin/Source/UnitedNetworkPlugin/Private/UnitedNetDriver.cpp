// Fill out your copyright notice in the Description page of Project Settings.


#include "UnitedNetDriver.h"
#include "UnitedNetConnection.h"

// cross-platform includes for htonl/ntohl
#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <winsock2.h>
#include <ws2tcpip.h>   // optional but useful
#include "Windows/HideWindowsPlatformTypes.h"
#else
#include <arpa/inet.h>
#endif

#include "Sockets.h"
#include "SocketSubsystem.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "Async/Async.h"
#include "Logging/LogMacros.h"


DEFINE_LOG_CATEGORY_STATIC(LogUnitedNetDriver, Log, All);

class FTcpRecvRunnable : public FRunnable
{
public:
    FTcpRecvRunnable(FSocket* InSocket, UUnitedNetDriver* InDriver)
        : Socket(InSocket), Driver(InDriver), bStop(false), ExpectedSize(0) {}

    virtual uint32 Run() override
    {
        constexpr int32 BufferSize = 4096;
        TArray<uint8> Buffer;
        Buffer.SetNumUninitialized(BufferSize);

        while (!bStop)
        {
            int32 BytesRead = 0;
            if (Socket && Socket->Recv(Buffer.GetData(), Buffer.Num(), BytesRead, ESocketReceiveFlags::None))
            {
                if (BytesRead > 0)
                {
                    Stream.Append(Buffer.GetData(), BytesRead);
                    ProcessStream();
                }
            }
            else
            {
                // small sleep to avoid busy-loop
                FPlatformProcess::Sleep(0.002f);
            }
        }
        return 0;
    }

    virtual void Stop() override { bStop = true; }

private:
    FSocket* Socket;
    UUnitedNetDriver* Driver;
    FThreadSafeBool bStop;

    TArray<uint8> Stream;
    uint32 ExpectedSize;

    void ProcessStream()
    {
        // Repeatedly extract length-prefixed packets: [uint32 length (network)][payload]
        while (true)
        {
            if (ExpectedSize == 0)
            {
                if (Stream.Num() < 4) return;

                uint32 NetLen = 0;
                FMemory::Memcpy(&NetLen, Stream.GetData(), 4);
                ExpectedSize = ntohl(NetLen);
                Stream.RemoveAt(0, 4, EAllowShrinking::No);
            }

            if (Stream.Num() < static_cast<int32>(ExpectedSize)) return;

            TArray<uint8> Packet;
            Packet.Append(Stream.GetData(), ExpectedSize);
            Stream.RemoveAt(0, ExpectedSize, EAllowShrinking::No);
            ExpectedSize = 0;

            // hand off to game thread
            AsyncTask(ENamedThreads::GameThread, [this, Packet = MoveTemp(Packet)]() mutable {
                if (Driver)
                {
                    Driver->HandleIncomingPacket(Packet);
                }
            });
        }
    }
};


UUnitedNetDriver::UUnitedNetDriver()
{
	Socket = nullptr;
	RecvThread = nullptr;
	RecvRunnable = nullptr;
}

bool UUnitedNetDriver::InitBase(bool bInitAsClient, FNetworkNotify* InNotify, const FURL& URL, bool bReuseAddressAndPort, FString& Error)
{
	// This method is called by the engine when bringing up the net driver.
	// We expect bInitAsClient == true for a client-side driver.
	if (!bInitAsClient)
	{
		Error = TEXT("This driver is client-only in the current design.");
		return false;
	}

	// store notify pointer normally expected by engine
	Notify = InNotify;

	const FString Host = URL.Host;
	const int32 Port = (URL.Port > 0) ? URL.Port : 7777;

	if (!StartSocket(Host, Port, Error))
	{
		UE_LOG(LogUnitedNetDriver, Error, TEXT("StartSocket failed: %s"), *Error);
		return false;
	}

	// Manually create a UNetConnection object for the client
	UUnitedNetConnection* NewConn = NewObject<UUnitedNetConnection>(GetTransientPackage(), UUnitedNetConnection::StaticClass());
	NewConn->Driver = this;

	NewConn->InitConnection(this, USOCK_Open, URL);

	ServerConnection = NewConn;

	return true;
}

bool UUnitedNetDriver::StartSocket(const FString& Host, int32 Port, FString& OutError)
{
	ISocketSubsystem* SockSub = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (!SockSub) { OutError = TEXT("No socket subsystem"); return false; }

	Socket = SockSub->CreateSocket(NAME_Stream, TEXT("UnitedNetSocket"), false);
	if (!Socket) { OutError = TEXT("Socket creation failed"); return false; }

	TSharedRef<FInternetAddr> RemoteAddr = SockSub->CreateInternetAddr();
	bool bValid = false;
	RemoteAddr->SetIp(*Host, bValid);
	RemoteAddr->SetPort(Port);

	if (!bValid)
	{
		OutError = TEXT("Invalid remote IP");
		SockSub->DestroySocket(Socket);
		Socket = nullptr;
		return false;
	}

	// Connect (blocking) — handled by FSocket::Connect
	if (!Socket->Connect(*RemoteAddr))
	{
		OutError = TEXT("TCP connect failed");
		SockSub->DestroySocket(Socket);
		Socket = nullptr;
		return false;
	}

	// Disable Nagle
	Socket->SetNoDelay(true);

	// Start receive runnable
	RecvRunnable = new FTcpRecvRunnable(Socket, this);
	RecvThread = FRunnableThread::Create(RecvRunnable, TEXT("UnitedRecvThread"));
	if (!RecvThread)
	{
		OutError = TEXT("Failed to create recv thread");
		RecvRunnable->Stop();
		delete RecvRunnable;
		RecvRunnable = nullptr;
		Socket->Close();
		SockSub->DestroySocket(Socket);
		Socket = nullptr;
		return false;
	}

	return true;
}

void UUnitedNetDriver::StopSocket()
{
	if (RecvRunnable) { RecvRunnable->Stop(); }
	if (RecvThread)
	{
		RecvThread->Kill(true);
		delete RecvThread;
		RecvThread = nullptr;
	}
	if (RecvRunnable) { delete RecvRunnable; RecvRunnable = nullptr; }

	if (Socket)
	{
		Socket->Close();
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Socket);
		Socket = nullptr;
	}
}

void UUnitedNetDriver::Shutdown()
{
	StopSocket();

	if (ServerConnection)
	{
		ServerConnection->Close();
		ServerConnection = nullptr;
	}

	Super::Shutdown();
}

void UUnitedNetDriver::HandleIncomingPacket(const TArray<uint8>& Packet)
{
	// Executed on the game thread (RecvRunnable posts here).
	if (!ServerConnection) return;

	UUnitedNetConnection* MyConn = Cast<UUnitedNetConnection>(ServerConnection);
	if (MyConn)
	{
		MyConn->ReceivedRawPacket(const_cast<uint8*>(Packet.GetData()), Packet.Num());
	}
}

void UUnitedNetDriver::EnqueueOutgoingPacket(const TArray<uint8>& Packet)
{
	FScopeLock Lock(&OutQueueMutex);
	OutgoingPackets.Add(Packet);
}

void UUnitedNetDriver::FlushOutgoingSends()
{
	TArray<TArray<uint8>> LocalCopy;
	{
		FScopeLock Lock(&OutQueueMutex);
		if (OutgoingPackets.Num() == 0) return;
		LocalCopy = MoveTemp(OutgoingPackets);
		OutgoingPackets.Empty();
	}

	for (const TArray<uint8>& Buf : LocalCopy)
	{
		if (!Socket) continue;

		// Frame: 4-byte length prefix (network order) + payload
		uint32 NetLen = htonl(Buf.Num());
		TArray<uint8> Framed;
		Framed.Append((uint8*)&NetLen, 4);
		Framed.Append(Buf);

		int32 BytesSent = 0;
		Socket->Send(Framed.GetData(), Framed.Num(), BytesSent);
		// Note: for production, handle partial sends: loop until all bytes are sent.
	}
}

void UUnitedNetDriver::TickDispatch(float DeltaTime)
{
	// flush outgoing writes on the game thread
	FlushOutgoingSends();

	// run base driver tick for timeouts/keepalives and internal engine bookkeeping
	Super::TickDispatch(DeltaTime);
}

