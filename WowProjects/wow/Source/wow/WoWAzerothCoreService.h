#pragma once

#include "CoreMinimal.h"

struct FWoWAzerothAccountRecord
{
	int32 AccountId = 0;
	FString Username;
};

struct FWoWGlueCharacterEquipmentSlot
{
	uint32 DisplayId = 0;
	uint8 InventoryType = 0;
	uint32 EnchantId = 0;
};

struct FWoWGlueCharacterSummary
{
	int32 Guid = 0;
	int32 AccountId = 0;
	FString Name;
	uint8 Race = 1;
	uint8 ClassId = 1;
	uint8 Gender = 0;
	uint8 Level = 1;
	uint8 Skin = 0;
	uint8 Face = 0;
	uint8 HairStyle = 0;
	uint8 HairColor = 0;
	uint8 FacialStyle = 0;
	int32 Zone = 0;
	int32 Map = 0;
	float PositionX = 0.0f;
	float PositionY = 0.0f;
	float PositionZ = 0.0f;
	float Orientation = 0.0f;
	uint32 Flags = 0;
	// SMSG_CHAR_ENUM 的 at_login 标记；Glue GetCharacterInfo 的 PCC/PRC/PFC 从这里映射。
	uint32 AtLoginFlags = 0;
	bool bHardcore = false;
	TArray<FWoWGlueCharacterEquipmentSlot> Equipment;
};

struct FWoWGlueRealmSummary
{
	int32 Index = 0;
	FString Name;
	FString Address;
	uint8 Icon = 0;
	uint8 Lock = 0;
	uint8 Flags = 0;
	float Population = 0.0f;
	uint8 CharacterCount = 0;
	uint8 Timezone = 0;
	uint8 RealmId = 1;
	uint8 MajorVersion = 0;
	uint8 MinorVersion = 0;
	uint8 PatchVersion = 0;
	uint16 Build = 0;
};

struct FWoWGlueCharacterCreateRequest
{
	FString Name;
	uint8 Race = 1;
	uint8 ClassId = 1;
	uint8 Gender = 0;
	uint8 Skin = 0;
	uint8 Face = 0;
	uint8 HairStyle = 0;
	uint8 HairColor = 0;
	uint8 FacialStyle = 0;
};

// Client-side WoW 3.3.5a Auth/World protocol bridge.
// This class must never talk directly to AzerothCore MySQL tables; it is a UE client and
// therefore only authenticates through authserver/worldserver packets, like wow.exe.
class WOW_API FWoWAzerothCoreService
{
public:
	static bool AuthenticateAccount(
		const FString& AccountName,
		const FString& Password,
		FWoWAzerothAccountRecord& OutAccount,
		FString& OutError);

	static bool LoadCharactersForAccount(
		int32 AccountId,
		TArray<FWoWGlueCharacterSummary>& OutCharacters,
		FString& OutError);

	static bool GetCachedCharactersForAccount(
		int32 AccountId,
		TArray<FWoWGlueCharacterSummary>& OutCharacters,
		FString& OutError);

	static bool LoadRealmList(
		TArray<FWoWGlueRealmSummary>& OutRealms,
		FString& OutError);

	static bool ConnectToRealm(
		int32 RealmIndex,
		FString& OutError);

	static bool CreateCharacter(
		int32 AccountId,
		const FWoWGlueCharacterCreateRequest& Request,
		FWoWGlueCharacterSummary& OutCharacter,
		FString& OutError);

	static void Disconnect();
};
