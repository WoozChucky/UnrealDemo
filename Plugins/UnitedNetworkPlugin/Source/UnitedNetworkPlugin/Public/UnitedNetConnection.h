// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Engine/NetConnection.h"
#include "UnitedNetConnection.generated.h"

/**
 *
 */
UCLASS()
class UNITEDNETWORKPLUGIN_API UUnitedNetConnection : public UNetConnection
{
	GENERATED_BODY()

public:
	virtual void LowLevelSend(void* Data, int32 CountBits, FOutPacketTraits& Traits) override;
	virtual void CleanUp() override;
	virtual FString LowLevelGetRemoteAddress(bool bAppendPort = false) override;
	//virtual int32 GetMaxPacket() override { return 1200 * 8; } // in bits; arbitrary for TCP framing

	// Helper used by driver to inject received bytes
	void ReceivedRawPacket(void* Data, int32 CountBytes) override;

};
