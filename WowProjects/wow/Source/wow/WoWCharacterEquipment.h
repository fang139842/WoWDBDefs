#pragma once

#include "CoreMinimal.h"

struct WOW_API FWoWCharacterEquipmentDisplay
{
	int32 Slot = INDEX_NONE;
	int32 DisplayId = 0;
	int32 InventoryType = 0;
	int32 EnchantId = 0;
};

namespace WoWCharacterEquipment
{
	WOW_API TArray<FWoWCharacterEquipmentDisplay> ParseEquipmentSummary(const FString& EquipmentSummary);
	WOW_API int32 CountEquipmentEntries(const FString& EquipmentSummary);
	WOW_API int32 GetDisplayIdForSlot(const FString& EquipmentSummary, int32 Slot);
}
