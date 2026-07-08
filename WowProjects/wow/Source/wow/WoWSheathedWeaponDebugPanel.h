#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

class UWoWCharacterPreviewComponent;

class SWoWSheathedWeaponDebugPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SWoWSheathedWeaponDebugPanel) {}
		SLATE_ARGUMENT(TWeakObjectPtr<UWoWCharacterPreviewComponent>, PreviewComponent)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	enum class EDebugFloatField : uint8
	{
		LocationX,
		LocationY,
		LocationZ,
		RotationPitch,
		RotationYaw,
		RotationRoll
	};

private:
	FText GetSummaryText() const;
	FText GetExportText() const;
	FText GetFieldText(EDebugFloatField Field) const;
	float GetSliderValue(EDebugFloatField Field) const;
	void OnSliderChanged(float Value, EDebugFloatField Field);
	FReply OnCycleAttachmentClicked();
	FReply OnCycleRotationClicked();
	FReply OnResetOffsetsClicked();
	FReply OnCopyParamsClicked();

	TWeakObjectPtr<UWoWCharacterPreviewComponent> PreviewComponent;
};
