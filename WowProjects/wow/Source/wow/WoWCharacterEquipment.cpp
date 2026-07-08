#include "WoWCharacterEquipment.h"

namespace WoWCharacterEquipment
{
TArray<FWoWCharacterEquipmentDisplay> ParseEquipmentSummary(const FString& EquipmentSummary)
{
	TArray<FWoWCharacterEquipmentDisplay> Equipment;
	if (EquipmentSummary.IsEmpty())
	{
		return Equipment;
	}

	TArray<FString> Parts;
	EquipmentSummary.ParseIntoArray(Parts, TEXT(";"), true);
	Equipment.Reserve(Parts.Num());
	for (const FString& Part : Parts)
	{
		TArray<FString> Fields;
		Part.ParseIntoArray(Fields, TEXT(":"), false);
		if (Fields.Num() < 3)
		{
			continue;
		}

		FWoWCharacterEquipmentDisplay Entry;
		Entry.Slot = FCString::Atoi(*Fields[0]);
		Entry.DisplayId = FCString::Atoi(*Fields[1]);
		Entry.InventoryType = FCString::Atoi(*Fields[2]);
		Entry.EnchantId = Fields.Num() > 3 ? FCString::Atoi(*Fields[3]) : 0;
		if (Entry.Slot >= 0 && Entry.DisplayId > 0)
		{
			Equipment.Add(Entry);
		}
	}
	return Equipment;
}

int32 CountEquipmentEntries(const FString& EquipmentSummary)
{
	return ParseEquipmentSummary(EquipmentSummary).Num();
}

int32 GetDisplayIdForSlot(const FString& EquipmentSummary, int32 Slot)
{
	for (const FWoWCharacterEquipmentDisplay& Entry : ParseEquipmentSummary(EquipmentSummary))
	{
		if (Entry.Slot == Slot && Entry.DisplayId > 0)
		{
			return Entry.DisplayId;
		}
	}
	return 0;
}
}
