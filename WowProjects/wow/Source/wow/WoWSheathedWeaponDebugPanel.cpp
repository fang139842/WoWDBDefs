#include "WoWSheathedWeaponDebugPanel.h"

#include "WoWCharacterPreviewComponent.h"

#include "Styling/CoreStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SSlider.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformApplicationMisc.h"

namespace
{
constexpr float LocationSliderRange = 80.0f;
constexpr float RotationSliderRange = 180.0f;
const FLinearColor PanelBackgroundColor(0.035f, 0.041f, 0.052f, 0.86f);
const FLinearColor SectionBackgroundColor(0.075f, 0.083f, 0.100f, 0.92f);
const FLinearColor PanelOutlineColor(0.18f, 0.20f, 0.24f, 0.95f);
const FLinearColor HeaderColor(0.85f, 0.75f, 0.50f, 1.0f);
const FLinearColor TextColor(0.88f, 0.90f, 0.92f, 1.0f);
const FLinearColor MutedTextColor(0.62f, 0.66f, 0.72f, 1.0f);

FText DebugFieldLabel(SWoWSheathedWeaponDebugPanel::EDebugFloatField Field)
{
	switch (Field)
	{
	case SWoWSheathedWeaponDebugPanel::EDebugFloatField::LocationX:
		return FText::FromString(TEXT("位置 X"));
	case SWoWSheathedWeaponDebugPanel::EDebugFloatField::LocationY:
		return FText::FromString(TEXT("位置 Y"));
	case SWoWSheathedWeaponDebugPanel::EDebugFloatField::LocationZ:
		return FText::FromString(TEXT("位置 Z"));
	case SWoWSheathedWeaponDebugPanel::EDebugFloatField::RotationPitch:
		return FText::FromString(TEXT("俯仰"));
	case SWoWSheathedWeaponDebugPanel::EDebugFloatField::RotationYaw:
		return FText::FromString(TEXT("偏航"));
	case SWoWSheathedWeaponDebugPanel::EDebugFloatField::RotationRoll:
		return FText::FromString(TEXT("翻滚"));
	default:
		return FText::FromString(TEXT("未知"));
	}
}

bool IsLocationField(SWoWSheathedWeaponDebugPanel::EDebugFloatField Field)
{
	return Field == SWoWSheathedWeaponDebugPanel::EDebugFloatField::LocationX ||
		Field == SWoWSheathedWeaponDebugPanel::EDebugFloatField::LocationY ||
		Field == SWoWSheathedWeaponDebugPanel::EDebugFloatField::LocationZ;
}
}

void SWoWSheathedWeaponDebugPanel::Construct(const FArguments& InArgs)
{
	PreviewComponent = InArgs._PreviewComponent;

	auto MakeSliderRow = [this](EDebugFloatField Field)
	{
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.0f, 4.0f, 10.0f, 4.0f)
			[
				SNew(SBox)
				.WidthOverride(82.0f)
				[
					SNew(STextBlock)
					.ColorAndOpacity(TextColor)
					.Text(this, &SWoWSheathedWeaponDebugPanel::GetFieldText, Field)
				]
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			.Padding(0.0f, 4.0f)
			[
				SNew(SSlider)
				.Value(this, &SWoWSheathedWeaponDebugPanel::GetSliderValue, Field)
				.OnValueChanged(this, &SWoWSheathedWeaponDebugPanel::OnSliderChanged, Field)
			];
	};

	auto MakeSectionPanel = [](const FText& Title, TSharedRef<SWidget> Content)
	{
		return SNew(SBorder)
			.Padding(10.0f)
			.BorderImage(FCoreStyle::Get().GetBrush(TEXT("WhiteBrush")))
			.BorderBackgroundColor(SectionBackgroundColor)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 0.0f, 0.0f, 6.0f)
				[
					SNew(STextBlock)
					.ColorAndOpacity(HeaderColor)
					.Text(Title)
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					Content
				]
			];
	};

	auto MakeLocationSection = [MakeSliderRow]()
	{
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()[MakeSliderRow(EDebugFloatField::LocationX)]
			+ SVerticalBox::Slot().AutoHeight()[MakeSliderRow(EDebugFloatField::LocationY)]
			+ SVerticalBox::Slot().AutoHeight()[MakeSliderRow(EDebugFloatField::LocationZ)];
	};

	auto MakeRotationSection = [MakeSliderRow]()
	{
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()[MakeSliderRow(EDebugFloatField::RotationPitch)]
			+ SVerticalBox::Slot().AutoHeight()[MakeSliderRow(EDebugFloatField::RotationYaw)]
			+ SVerticalBox::Slot().AutoHeight()[MakeSliderRow(EDebugFloatField::RotationRoll)];
	};

	auto MakeParamsSection = [this]()
	{
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				SNew(SEditableTextBox)
				.IsReadOnly(true)
				.SelectAllTextWhenFocused(true)
				.Text(this, &SWoWSheathedWeaponDebugPanel::GetExportText)
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(STextBlock)
				.ColorAndOpacity(MutedTextColor)
				.AutoWrapText(true)
				.Text(FText::FromString(TEXT("点击文本框可全选，或使用复制按钮把当前参数写入剪贴板。")))
			];
	};

	ChildSlot
	[
		SNew(SBorder)
		.Padding(1.0f)
		.BorderImage(FCoreStyle::Get().GetBrush(TEXT("WhiteBrush")))
		.BorderBackgroundColor(PanelOutlineColor)
		[
			SNew(SBorder)
			.Padding(12.0f)
			.BorderImage(FCoreStyle::Get().GetBrush(TEXT("WhiteBrush")))
			.BorderBackgroundColor(PanelBackgroundColor)
			[
				SNew(SBox)
				.WidthOverride(540.0f)
				.MaxDesiredHeight(720.0f)
				[
					SNew(SScrollBox)
					+ SScrollBox::Slot()
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0.0f, 0.0f, 0.0f, 4.0f)
						[
							SNew(STextBlock)
							.ColorAndOpacity(HeaderColor)
							.Text(FText::FromString(TEXT("模型挂载参数调试器")))
						]
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0.0f, 0.0f, 0.0f, 10.0f)
						[
							SNew(STextBlock)
							.ColorAndOpacity(TextColor)
							.AutoWrapText(true)
							.Text(this, &SWoWSheathedWeaponDebugPanel::GetSummaryText)
						]
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0.0f, 0.0f, 0.0f, 8.0f)
						[
							SNew(SHorizontalBox)
							+ SHorizontalBox::Slot()
							.AutoWidth()
							.Padding(0.0f, 0.0f, 6.0f, 0.0f)
							[
								SNew(SButton)
								.Text(FText::FromString(TEXT("切换挂点")))
								.OnClicked(this, &SWoWSheathedWeaponDebugPanel::OnCycleAttachmentClicked)
							]
							+ SHorizontalBox::Slot()
							.AutoWidth()
							.Padding(0.0f, 0.0f, 6.0f, 0.0f)
							[
								SNew(SButton)
								.Text(FText::FromString(TEXT("切换旋转")))
								.OnClicked(this, &SWoWSheathedWeaponDebugPanel::OnCycleRotationClicked)
							]
							+ SHorizontalBox::Slot()
							.AutoWidth()
							.Padding(0.0f, 0.0f, 6.0f, 0.0f)
							[
								SNew(SButton)
								.Text(FText::FromString(TEXT("重置微调")))
								.OnClicked(this, &SWoWSheathedWeaponDebugPanel::OnResetOffsetsClicked)
							]
							+ SHorizontalBox::Slot()
							.AutoWidth()
							[
								SNew(SButton)
								.Text(FText::FromString(TEXT("复制参数")))
								.OnClicked(this, &SWoWSheathedWeaponDebugPanel::OnCopyParamsClicked)
							]
						]
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0.0f, 0.0f, 0.0f, 8.0f)
						[
							MakeSectionPanel(FText::FromString(TEXT("参数文本")), MakeParamsSection())
						]
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0.0f, 0.0f, 0.0f, 8.0f)
						[
							MakeSectionPanel(FText::FromString(TEXT("位置微调")), MakeLocationSection())
						]
						+ SVerticalBox::Slot()
						.AutoHeight()
						[
							MakeSectionPanel(FText::FromString(TEXT("旋转微调")), MakeRotationSection())
						]
					]
				]
			]
		]
	];
}

FText SWoWSheathedWeaponDebugPanel::GetSummaryText() const
{
	if (const UWoWCharacterPreviewComponent* Component = PreviewComponent.Get())
	{
		const FWoWSheathedWeaponDebugState State = Component->GetSheathedWeaponDebugState();
		return FText::FromString(FString::Printf(
			TEXT("attach=%d(%s) rotIndex=%d loc=%s rotNudge=%s"),
			State.AttachmentId,
			*State.AttachmentName,
			State.RotationIndex,
			*State.LocationOffset.ToString(),
			*State.RotationOffset.ToString()));
	}
	return FText::FromString(TEXT("No preview component"));
}

FText SWoWSheathedWeaponDebugPanel::GetExportText() const
{
	if (const UWoWCharacterPreviewComponent* Component = PreviewComponent.Get())
	{
		return FText::FromString(Component->GetSheathedWeaponDebugState().ExportText);
	}
	return FText::FromString(TEXT(""));
}

FText SWoWSheathedWeaponDebugPanel::GetFieldText(EDebugFloatField Field) const
{
	const float Range = IsLocationField(Field) ? LocationSliderRange : RotationSliderRange;
	const float Value = (GetSliderValue(Field) * 2.0f - 1.0f) * Range;
	return FText::FromString(FString::Printf(TEXT("%s: %.2f"), *DebugFieldLabel(Field).ToString(), Value));
}

float SWoWSheathedWeaponDebugPanel::GetSliderValue(EDebugFloatField Field) const
{
	const UWoWCharacterPreviewComponent* Component = PreviewComponent.Get();
	if (!Component)
	{
		return 0.5f;
	}

	const FWoWSheathedWeaponDebugState State = Component->GetSheathedWeaponDebugState();
	float RawValue = 0.0f;
	float Range = LocationSliderRange;
	switch (Field)
	{
	case EDebugFloatField::LocationX: RawValue = State.LocationOffset.X; break;
	case EDebugFloatField::LocationY: RawValue = State.LocationOffset.Y; break;
	case EDebugFloatField::LocationZ: RawValue = State.LocationOffset.Z; break;
	case EDebugFloatField::RotationPitch: RawValue = State.RotationOffset.Pitch; Range = RotationSliderRange; break;
	case EDebugFloatField::RotationYaw: RawValue = State.RotationOffset.Yaw; Range = RotationSliderRange; break;
	case EDebugFloatField::RotationRoll: RawValue = State.RotationOffset.Roll; Range = RotationSliderRange; break;
	default: break;
	}
	return FMath::Clamp((RawValue / Range + 1.0f) * 0.5f, 0.0f, 1.0f);
}

void SWoWSheathedWeaponDebugPanel::OnSliderChanged(float Value, EDebugFloatField Field)
{
	UWoWCharacterPreviewComponent* Component = PreviewComponent.Get();
	if (!Component)
	{
		return;
	}

	FWoWSheathedWeaponDebugState State = Component->GetSheathedWeaponDebugState();
	const float Range = IsLocationField(Field) ? LocationSliderRange : RotationSliderRange;
	const float RawValue = (Value * 2.0f - 1.0f) * Range;
	switch (Field)
	{
	case EDebugFloatField::LocationX: State.LocationOffset.X = RawValue; break;
	case EDebugFloatField::LocationY: State.LocationOffset.Y = RawValue; break;
	case EDebugFloatField::LocationZ: State.LocationOffset.Z = RawValue; break;
	case EDebugFloatField::RotationPitch: State.RotationOffset.Pitch = RawValue; break;
	case EDebugFloatField::RotationYaw: State.RotationOffset.Yaw = RawValue; break;
	case EDebugFloatField::RotationRoll: State.RotationOffset.Roll = RawValue; break;
	default: break;
	}

	Component->SetDebugSheathedLocationOffset(State.LocationOffset);
	Component->SetDebugSheathedRotationOffset(State.RotationOffset);
}

FReply SWoWSheathedWeaponDebugPanel::OnCycleAttachmentClicked()
{
	if (UWoWCharacterPreviewComponent* Component = PreviewComponent.Get())
	{
		Component->DebugCycleSheathedAttachment();
	}
	return FReply::Handled();
}

FReply SWoWSheathedWeaponDebugPanel::OnCycleRotationClicked()
{
	if (UWoWCharacterPreviewComponent* Component = PreviewComponent.Get())
	{
		Component->DebugCycleSheathedRotation();
	}
	return FReply::Handled();
}

FReply SWoWSheathedWeaponDebugPanel::OnResetOffsetsClicked()
{
	if (UWoWCharacterPreviewComponent* Component = PreviewComponent.Get())
	{
		Component->ResetDebugSheathedOffsetsToDefault();
	}
	return FReply::Handled();
}

FReply SWoWSheathedWeaponDebugPanel::OnCopyParamsClicked()
{
	if (const UWoWCharacterPreviewComponent* Component = PreviewComponent.Get())
	{
		FPlatformApplicationMisc::ClipboardCopy(*Component->GetSheathedWeaponDebugState().ExportText);
	}
	return FReply::Handled();
}
