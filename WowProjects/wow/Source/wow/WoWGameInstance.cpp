#include "WoWGameInstance.h"

void UWoWGameInstance::SetSelectedCharacterRuntimeAppearance(const FWoWSelectedCharacterRuntimeAppearance& InAppearance)
{
	SelectedCharacterRuntimeAppearance = InAppearance;
}

void UWoWGameInstance::ClearSelectedCharacterRuntimeAppearance()
{
	SelectedCharacterRuntimeAppearance = FWoWSelectedCharacterRuntimeAppearance();
}
