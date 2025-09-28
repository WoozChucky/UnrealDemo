// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Engine/NetDriver.h"
#include "UnitedNetDriver.generated.h"

/**
 * 
 */
UCLASS()
class UNITEDNETWORKPLUGIN_API UUnitedNetDriver : public UNetDriver
{
	GENERATED_BODY()

public:
	UUnitedNetDriver();

	virtual bool InitBase(bool bInitAsClient, FNetworkNotify* InNotify, const FURL& URL, bool bReuseAddressAndPort, FString& Error) override;
	virtual void TickDispatch(float DeltaTime) override;
	virtual void Shutdown() override;

	void HandleIncomingPacket(const TArray<uint8>& Packet);
	void EnqueueOutgoingPacket(const TArray<uint8>& Packet);

private:
	FSocket* Socket;
	FRunnableThread* RecvThread;
	class FTcpRecvRunnable* RecvRunnable;

	FCriticalSection OutQueueMutex;
	TArray<TArray<uint8>> OutgoingPackets;

	bool StartSocket(const FString& Host, int32 Port, FString& OutError);
	void StopSocket();
	void FlushOutgoingSends();
};
