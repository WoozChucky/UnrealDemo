// Fill out your copyright notice in the Description page of Project Settings.


#include "UnitedNetConnection.h"

#include "UnitedNetConnection.h"
#include "UnitedNetDriver.h"

void UUnitedNetConnection::LowLevelSend(void* Data, int32 CountBits, FOutPacketTraits& /*Traits*/)
{
	const int32 CountBytes = FMath::DivideAndRoundUp(CountBits, 8);
	TArray<uint8> Payload;
	Payload.Append(static_cast<uint8*>(Data), CountBytes);

	if (UUnitedNetDriver* CastDriver = Cast<UUnitedNetDriver>(GetDriver()))
	{
		CastDriver->EnqueueOutgoingPacket(Payload);
	}
}

void UUnitedNetConnection::ReceivedRawPacket(void* Data, int32 CountBytes)
{
	// Forward raw bytes into the standard UNetConnection path so PacketHandler/Channels can process them
	UNetConnection::ReceivedRawPacket(Data, CountBytes);
}

void UUnitedNetConnection::CleanUp()
{
	Super::CleanUp();
}

FString UUnitedNetConnection::LowLevelGetRemoteAddress(bool bAppendPort)
{
	return TEXT("UnitedTCPRemote"); // or format using your driver’s stored address
}