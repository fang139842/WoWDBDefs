#include "WoWLoginSceneDebugPanel.h"

#include "WoWLoginGameMode.h"

#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Input/Reply.h"
#include "InputCoreTypes.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Rendering/SlateRenderTransform.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMenuAnchor.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Input/SNumericEntryBox.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Input/SSlider.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
constexpr float LocationSliderRange = 500.0f;
constexpr float RotationSliderRange = 180.0f;
constexpr float ScaleSliderRange = 2.0f;
constexpr float FovSliderRange = 45.0f;
const FLinearColor PanelBackgroundColor(0.035f, 0.041f, 0.052f, 0.86f);
const FLinearColor SectionBackgroundColor(0.075f, 0.083f, 0.100f, 0.92f);
const FLinearColor PopupBackgroundColor(0.045f, 0.050f, 0.058f, 1.0f);
const FLinearColor SelectedRowBackgroundColor(0.30f, 0.24f, 0.12f, 1.0f);
const FLinearColor RowBackgroundColor(0.115f, 0.120f, 0.128f, 1.0f);
const FLinearColor PanelOutlineColor(0.18f, 0.20f, 0.24f, 0.95f);
const FLinearColor HeaderColor(0.85f, 0.75f, 0.50f, 1.0f);
const FLinearColor TextColor(0.88f, 0.90f, 0.92f, 1.0f);
const FLinearColor MutedTextColor(0.62f, 0.66f, 0.72f, 1.0f);

class SWheelFineTuneSlider : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SWheelFineTuneSlider) {}
		SLATE_ATTRIBUTE(float, Value)
		SLATE_EVENT(FOnFloatValueChanged, OnValueChanged)
		SLATE_EVENT(TDelegate<void(float)>, OnWheelDelta)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		OnWheelDelta = InArgs._OnWheelDelta;
		ChildSlot
		[
			SNew(SSlider)
			.Value(InArgs._Value)
			.OnValueChanged(InArgs._OnValueChanged)
		];
	}

	virtual FReply OnMouseWheel(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override
	{
		if (OnWheelDelta.IsBound())
		{
			OnWheelDelta.Execute(MouseEvent.GetWheelDelta());
			return FReply::Handled();
		}
		return SCompoundWidget::OnMouseWheel(MyGeometry, MouseEvent);
	}

private:
	TDelegate<void(float)> OnWheelDelta;
};

FString TextureRenderOverrideLabel(EWoWLoginTextureRenderOverride OverrideMode)
{
	switch (OverrideMode)
	{
	case EWoWLoginTextureRenderOverride::M2Original: return TEXT("M2原始");
	case EWoWLoginTextureRenderOverride::AdditiveAlpha: return TEXT("RM_AdditiveAlpha");
	case EWoWLoginTextureRenderOverride::AlphaBlend: return TEXT("RM_AlphaBlending");
	case EWoWLoginTextureRenderOverride::Opaque: return TEXT("RM_Opaque");
	case EWoWLoginTextureRenderOverride::Masked: return TEXT("UE_Masked");
	case EWoWLoginTextureRenderOverride::Additive: return TEXT("UE_Additive");
	case EWoWLoginTextureRenderOverride::Modulate: return TEXT("UE_Modulate近似");
	case EWoWLoginTextureRenderOverride::AlphaComposite: return TEXT("UE_AlphaComposite近似");
	default: return TEXT("未知");
	}
}

FString M2RenderFlagLabel(int32 Flags)
{
	switch (Flags)
	{
	case 0: return TEXT("RF_None");
	case 1: return TEXT("RF_Unlit");
	case 2: return TEXT("RF_Unfogged");
	case 3: return TEXT("RF_Unlit_Unfogged");
	case 4: return TEXT("RF_TwoSided");
	case 5: return TEXT("RF_Unlit_Two_Sided");
	case 6: return TEXT("RF_Unfogged_TwoSided");
	case 7: return TEXT("RF_Unlit_Unfogged_TwoSided");
	case 8: return TEXT("RF_Billboard");
	case 9: return TEXT("RF_Unlit_Billboard");
	case 10: return TEXT("RF_Unfogged_Billboard");
	case 11: return TEXT("RF_Unlit_Unfogged_Billboard");
	case 12: return TEXT("RF_Billboard_TwoSided");
	case 13: return TEXT("RF_Unlit_Billboard_TwoSided");
	case 14: return TEXT("RF_Unfogged_Billboard_TwoSided");
	case 15: return TEXT("RF_Unlit_Unfogged_Billboard_TwoSided");
	case 16: return TEXT("RF_Not_ZBuffered");
	case 17: return TEXT("RF_Not_ZBuffered_Unlit");
	case 18: return TEXT("RF_Not_ZBuffered_Unfogged");
	case 19: return TEXT("RF_Not_ZBuffered_Unlit_Unfogged");
	case 20: return TEXT("RF_Not_ZBuffered_TwoSided");
	case 21: return TEXT("RF_Not_ZBuffered_Unlit_Two_Sided");
	case 22: return TEXT("RF_Not_ZBuffered_Unfogged_TwoSided");
	case 23: return TEXT("RF_Not_ZBuffered_Unlit_Unfogged_TwoSided");
	case 24: return TEXT("RF_Not_ZBuffered_Billboard");
	case 25: return TEXT("RF_Not_ZBuffered_Unlit_Billboard");
	case 26: return TEXT("RF_Not_ZBuffered_Unfogged_Billboard");
	case 27: return TEXT("RF_Not_ZBuffered_Unlit_Unfogged_Billboard");
	case 28: return TEXT("RF_Not_ZBuffered_Billboard_TwoSided");
	case 29: return TEXT("RF_Not_ZBuffered_Unlit_Billboard_TwoSided");
	case 30: return TEXT("RF_Not_ZBuffered_Unfogged_Billboard_TwoSided");
	case 31: return TEXT("RF_Not_ZBuffered_Unlit_Unfogged_Billboard_TwoSided");
	default: return FString::Printf(TEXT("RF_%d"), Flags);
	}
}

FString M2RenderModeLabel(int32 RenderMode)
{
	switch (RenderMode)
	{
	case 0: return TEXT("RM_Opaque");
	case 1: return TEXT("RM_AlphaTesting");
	case 2: return TEXT("RM_AlphaBlending");
	case 3: return TEXT("RM_Additive");
	case 4: return TEXT("RM_AdditiveAlpha");
	case 5: return TEXT("RM_Modulate");
	case 6: return TEXT("RM_DeeprumTram");
	default: return FString::Printf(TEXT("RM_%d"), RenderMode);
	}
}

FString AnimationObjectPathLabel(const FString& ObjectPath)
{
	FString PackagePath;
	FString AssetName;
	if (ObjectPath.Split(TEXT("."), &PackagePath, &AssetName, ESearchCase::IgnoreCase, ESearchDir::FromEnd) && !AssetName.IsEmpty())
	{
		return AssetName;
	}
	return ObjectPath.IsEmpty() ? TEXT("使用当前角色自动 Stand") : ObjectPath;
}

bool TryParsePreviewAnimationIdFromObjectPath(const FString& ObjectPath, int32& OutAnimationId)
{
	FString PackagePath;
	FString AssetName;
	if (!ObjectPath.Split(TEXT("."), &PackagePath, &AssetName, ESearchCase::IgnoreCase, ESearchDir::FromEnd) || AssetName.IsEmpty())
	{
		AssetName = FPackageName::GetLongPackageAssetName(ObjectPath);
	}

	TArray<FString> Parts;
	AssetName.ParseIntoArray(Parts, TEXT("_"), true);
	for (const FString& Part : Parts)
	{
		if (Part.Len() == 3 && Part.IsNumeric())
		{
			OutAnimationId = FCString::Atoi(*Part);
			return true;
		}
	}
	return false;
}

FString M2RenderFlagEffectText(int32 Flags)
{
	return FString::Printf(
		TEXT("UE映射: %s, %s, %s, %s, %s"),
		(Flags & 0x0001) != 0 ? TEXT("Unlit") : TEXT("Lit"),
		(Flags & 0x0002) != 0 ? TEXT("NoFog") : TEXT("Fog"),
		(Flags & 0x0004) != 0 ? TEXT("TwoSided") : TEXT("OneSided"),
		(Flags & 0x0008) != 0 ? TEXT("Billboard待几何朝向") : TEXT("NoBillboard"),
		(Flags & 0x0010) != 0 ? TEXT("NoDepthWrite") : TEXT("DepthWrite"));
}

bool IsSceneLocationField(SWoWLoginSceneDebugPanel::EDebugFloatField Field)
{
	return Field == SWoWLoginSceneDebugPanel::EDebugFloatField::SceneLocationX ||
		Field == SWoWLoginSceneDebugPanel::EDebugFloatField::SceneLocationY ||
		Field == SWoWLoginSceneDebugPanel::EDebugFloatField::SceneLocationZ;
}

bool IsSceneRotationField(SWoWLoginSceneDebugPanel::EDebugFloatField Field)
{
	return Field == SWoWLoginSceneDebugPanel::EDebugFloatField::SceneRotationPitch ||
		Field == SWoWLoginSceneDebugPanel::EDebugFloatField::SceneRotationYaw ||
		Field == SWoWLoginSceneDebugPanel::EDebugFloatField::SceneRotationRoll;
}

bool IsSceneScaleField(SWoWLoginSceneDebugPanel::EDebugFloatField Field)
{
	return Field == SWoWLoginSceneDebugPanel::EDebugFloatField::SceneScaleX ||
		Field == SWoWLoginSceneDebugPanel::EDebugFloatField::SceneScaleY ||
		Field == SWoWLoginSceneDebugPanel::EDebugFloatField::SceneScaleZ;
}

bool IsCharacterPreviewLocationField(SWoWLoginSceneDebugPanel::EDebugFloatField Field)
{
	return Field == SWoWLoginSceneDebugPanel::EDebugFloatField::CharacterPreviewLocationX ||
		Field == SWoWLoginSceneDebugPanel::EDebugFloatField::CharacterPreviewLocationY ||
		Field == SWoWLoginSceneDebugPanel::EDebugFloatField::CharacterPreviewLocationZ;
}

bool IsCharacterPreviewRotationField(SWoWLoginSceneDebugPanel::EDebugFloatField Field)
{
	return Field == SWoWLoginSceneDebugPanel::EDebugFloatField::CharacterPreviewRotationPitch ||
		Field == SWoWLoginSceneDebugPanel::EDebugFloatField::CharacterPreviewRotationYaw ||
		Field == SWoWLoginSceneDebugPanel::EDebugFloatField::CharacterPreviewRotationRoll;
}

bool IsCharacterPreviewScaleField(SWoWLoginSceneDebugPanel::EDebugFloatField Field)
{
	return Field == SWoWLoginSceneDebugPanel::EDebugFloatField::CharacterPreviewUniformScale ||
		Field == SWoWLoginSceneDebugPanel::EDebugFloatField::CharacterPreviewScaleX ||
		Field == SWoWLoginSceneDebugPanel::EDebugFloatField::CharacterPreviewScaleY ||
		Field == SWoWLoginSceneDebugPanel::EDebugFloatField::CharacterPreviewScaleZ;
}

bool IsCameraPositionField(SWoWLoginSceneDebugPanel::EDebugFloatField Field)
{
	return Field == SWoWLoginSceneDebugPanel::EDebugFloatField::CameraPositionX ||
		Field == SWoWLoginSceneDebugPanel::EDebugFloatField::CameraPositionY ||
		Field == SWoWLoginSceneDebugPanel::EDebugFloatField::CameraPositionZ;
}

bool IsCameraTargetField(SWoWLoginSceneDebugPanel::EDebugFloatField Field)
{
	return Field == SWoWLoginSceneDebugPanel::EDebugFloatField::CameraTargetX ||
		Field == SWoWLoginSceneDebugPanel::EDebugFloatField::CameraTargetY ||
		Field == SWoWLoginSceneDebugPanel::EDebugFloatField::CameraTargetZ;
}

bool IsParticlePositionField(SWoWLoginSceneDebugPanel::EDebugFloatField Field)
{
	return Field == SWoWLoginSceneDebugPanel::EDebugFloatField::ParticlePositionX ||
		Field == SWoWLoginSceneDebugPanel::EDebugFloatField::ParticlePositionY ||
		Field == SWoWLoginSceneDebugPanel::EDebugFloatField::ParticlePositionZ;
}

bool IsParticleField(SWoWLoginSceneDebugPanel::EDebugFloatField Field)
{
	return IsParticlePositionField(Field) ||
		Field == SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleEmissionRateScale ||
		Field == SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleEmissionSpeedScale ||
		Field == SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleLifespanScale ||
		Field == SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleAreaLengthScale ||
		Field == SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleAreaWidthScale ||
		Field == SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleGravityScale ||
		Field == SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleGravity2Scale ||
		Field == SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleVerticalRangeScale ||
		Field == SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleHorizontalRangeScale ||
		Field == SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleSizeScale ||
		Field == SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleLifecycleSizeStartScale ||
		Field == SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleLifecycleSizeMidScale ||
		Field == SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleLifecycleSizeEndScale ||
		Field == SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleAlphaScale ||
		Field == SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleLifecycleAlphaStartScale ||
		Field == SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleLifecycleAlphaMidScale ||
		Field == SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleLifecycleAlphaEndScale ||
		Field == SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleAlphaCutoff ||
		Field == SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleBrightnessScale;
}

bool IsTextureField(SWoWLoginSceneDebugPanel::EDebugFloatField Field)
{
	return Field == SWoWLoginSceneDebugPanel::EDebugFloatField::TextureUVOffsetX ||
		Field == SWoWLoginSceneDebugPanel::EDebugFloatField::TextureUVOffsetY ||
		Field == SWoWLoginSceneDebugPanel::EDebugFloatField::TextureUVScaleX ||
		Field == SWoWLoginSceneDebugPanel::EDebugFloatField::TextureUVScaleY ||
		Field == SWoWLoginSceneDebugPanel::EDebugFloatField::TextureUVRotationDegrees ||
		Field == SWoWLoginSceneDebugPanel::EDebugFloatField::TextureAlphaScale ||
		Field == SWoWLoginSceneDebugPanel::EDebugFloatField::TextureBrightnessScale;
}

bool IsLightField(SWoWLoginSceneDebugPanel::EDebugFloatField Field)
{
	return Field == SWoWLoginSceneDebugPanel::EDebugFloatField::LightDiffuseIntensityScale ||
		Field == SWoWLoginSceneDebugPanel::EDebugFloatField::LightAmbientIntensityScale ||
		Field == SWoWLoginSceneDebugPanel::EDebugFloatField::LightDiffuseColorScaleR ||
		Field == SWoWLoginSceneDebugPanel::EDebugFloatField::LightDiffuseColorScaleG ||
		Field == SWoWLoginSceneDebugPanel::EDebugFloatField::LightDiffuseColorScaleB ||
		Field == SWoWLoginSceneDebugPanel::EDebugFloatField::LightAmbientColorScaleR ||
		Field == SWoWLoginSceneDebugPanel::EDebugFloatField::LightAmbientColorScaleG ||
		Field == SWoWLoginSceneDebugPanel::EDebugFloatField::LightAmbientColorScaleB ||
		Field == SWoWLoginSceneDebugPanel::EDebugFloatField::LightAttenuationRadiusScale;
}

float GetFieldRange(SWoWLoginSceneDebugPanel::EDebugFloatField Field)
{
	if (IsSceneLocationField(Field) || IsCameraPositionField(Field) || IsCameraTargetField(Field) || IsCharacterPreviewLocationField(Field))
	{
		return 50.0f;
	}
	if (IsParticlePositionField(Field))
	{
		return 1000.0f;
	}
	if (IsSceneRotationField(Field) || IsCharacterPreviewRotationField(Field))
	{
		return RotationSliderRange;
	}
	if (IsSceneScaleField(Field) || IsCharacterPreviewScaleField(Field))
	{
		return ScaleSliderRange;
	}
	switch (Field)
	{
	case SWoWLoginSceneDebugPanel::EDebugFloatField::GlobalBrightnessScale:
		return 2.0f;
	case SWoWLoginSceneDebugPanel::EDebugFloatField::M2PhysicalLightIntensityScale:
		return 300.0f;
	case SWoWLoginSceneDebugPanel::EDebugFloatField::LightDiffuseIntensityScale:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::LightAmbientIntensityScale:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::LightDiffuseColorScaleR:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::LightDiffuseColorScaleG:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::LightDiffuseColorScaleB:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::LightAmbientColorScaleR:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::LightAmbientColorScaleG:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::LightAmbientColorScaleB:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::LightAttenuationRadiusScale:
		return 4.0f;
	case SWoWLoginSceneDebugPanel::EDebugFloatField::CameraFov:
		return 1.5f;
	case SWoWLoginSceneDebugPanel::EDebugFloatField::CameraNearClipping:
		return 5.0f;
	case SWoWLoginSceneDebugPanel::EDebugFloatField::CameraFarClipping:
		return 5000.0f;
	case SWoWLoginSceneDebugPanel::EDebugFloatField::CameraRollEffectX:
		return UE_TWO_PI;
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleEmissionRateScale:
		return 8.0f;
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleEmissionSpeedScale:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleLifespanScale:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleAreaLengthScale:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleAreaWidthScale:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleGravityScale:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleGravity2Scale:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleVerticalRangeScale:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleHorizontalRangeScale:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleSizeScale:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleLifecycleSizeStartScale:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleLifecycleSizeMidScale:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleLifecycleSizeEndScale:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleAlphaScale:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleLifecycleAlphaStartScale:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleLifecycleAlphaMidScale:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleLifecycleAlphaEndScale:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleBrightnessScale:
		return 4.0f;
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleAlphaCutoff:
		return 0.25f;
	case SWoWLoginSceneDebugPanel::EDebugFloatField::TextureUVOffsetX:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::TextureUVOffsetY:
		return 4.0f;
	case SWoWLoginSceneDebugPanel::EDebugFloatField::TextureUVScaleX:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::TextureUVScaleY:
		return 8.0f;
	case SWoWLoginSceneDebugPanel::EDebugFloatField::TextureUVRotationDegrees:
		return 360.0f;
	case SWoWLoginSceneDebugPanel::EDebugFloatField::TextureAlphaScale:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::TextureBrightnessScale:
		return 16.0f;
	default:
		return FovSliderRange;
	}
}

float GetFieldWheelStep(SWoWLoginSceneDebugPanel::EDebugFloatField Field)
{
	if (IsSceneLocationField(Field) || IsCameraPositionField(Field) || IsCameraTargetField(Field))
	{
		return 0.01f;
	}
	if (IsParticlePositionField(Field))
	{
		return 1.0f;
	}
	if (IsSceneRotationField(Field))
	{
		return 0.1f;
	}
	if (IsSceneScaleField(Field))
	{
		return 0.005f;
	}

	switch (Field)
	{
	case SWoWLoginSceneDebugPanel::EDebugFloatField::GlobalBrightnessScale:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::LightDiffuseIntensityScale:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::LightAmbientIntensityScale:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::LightDiffuseColorScaleR:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::LightDiffuseColorScaleG:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::LightDiffuseColorScaleB:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::LightAmbientColorScaleR:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::LightAmbientColorScaleG:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::LightAmbientColorScaleB:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::LightAttenuationRadiusScale:
		return 0.01f;
	case SWoWLoginSceneDebugPanel::EDebugFloatField::M2PhysicalLightIntensityScale:
		return 1.0f;
	case SWoWLoginSceneDebugPanel::EDebugFloatField::CameraFov:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::CameraRollEffectX:
		return 0.001f;
	case SWoWLoginSceneDebugPanel::EDebugFloatField::CameraNearClipping:
		return 0.001f;
	case SWoWLoginSceneDebugPanel::EDebugFloatField::CameraFarClipping:
		return 1.0f;
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleEmissionRateScale:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleEmissionSpeedScale:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleLifespanScale:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleAreaLengthScale:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleAreaWidthScale:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleGravityScale:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleGravity2Scale:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleVerticalRangeScale:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleHorizontalRangeScale:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleSizeScale:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleLifecycleSizeStartScale:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleLifecycleSizeMidScale:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleLifecycleSizeEndScale:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleAlphaScale:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleLifecycleAlphaStartScale:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleLifecycleAlphaMidScale:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleLifecycleAlphaEndScale:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleBrightnessScale:
		return 0.01f;
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleAlphaCutoff:
		return 0.001f;
	case SWoWLoginSceneDebugPanel::EDebugFloatField::TextureUVOffsetX:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::TextureUVOffsetY:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::TextureUVScaleX:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::TextureUVScaleY:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::TextureAlphaScale:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::TextureBrightnessScale:
		return 0.01f;
	case SWoWLoginSceneDebugPanel::EDebugFloatField::TextureUVRotationDegrees:
		return 0.1f;
	case SWoWLoginSceneDebugPanel::EDebugFloatField::CharacterPreviewLocationX:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::CharacterPreviewLocationY:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::CharacterPreviewLocationZ:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::CharacterPreviewRotationPitch:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::CharacterPreviewRotationYaw:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::CharacterPreviewRotationRoll:
		return 1.0f;
	case SWoWLoginSceneDebugPanel::EDebugFloatField::CharacterPreviewUniformScale:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::CharacterPreviewScaleX:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::CharacterPreviewScaleY:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::CharacterPreviewScaleZ:
		return 0.01f;
	default:
		return 0.01f;
	}
}

FText DebugFieldLabel(SWoWLoginSceneDebugPanel::EDebugFloatField Field)
{
	switch (Field)
	{
	case SWoWLoginSceneDebugPanel::EDebugFloatField::SceneLocationX: return FText::FromString(TEXT("SceneLocation.x"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::SceneLocationY: return FText::FromString(TEXT("SceneLocation.y"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::SceneLocationZ: return FText::FromString(TEXT("SceneLocation.z"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::SceneRotationPitch: return FText::FromString(TEXT("SceneRotation.pitch"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::SceneRotationYaw: return FText::FromString(TEXT("SceneRotation.yaw"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::SceneRotationRoll: return FText::FromString(TEXT("SceneRotation.roll"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::SceneScaleX: return FText::FromString(TEXT("SceneScale.x"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::SceneScaleY: return FText::FromString(TEXT("SceneScale.y"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::SceneScaleZ: return FText::FromString(TEXT("SceneScale.z"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::CameraPositionX: return FText::FromString(TEXT("struct C3Vector Pos.x"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::CameraPositionY: return FText::FromString(TEXT("struct C3Vector Pos.y"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::CameraPositionZ: return FText::FromString(TEXT("struct C3Vector Pos.z"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::CameraTargetX: return FText::FromString(TEXT("struct C3Vector Target.x"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::CameraTargetY: return FText::FromString(TEXT("struct C3Vector Target.y"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::CameraTargetZ: return FText::FromString(TEXT("struct C3Vector Target.z"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::CameraFov: return FText::FromString(TEXT("float FOV"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::CameraNearClipping: return FText::FromString(TEXT("float NearClipping"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::CameraFarClipping: return FText::FromString(TEXT("float FarClipping"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::CameraRollEffectX: return FText::FromString(TEXT("struct ABlock_F RollEffect.val.x"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::GlobalBrightnessScale: return FText::FromString(TEXT("GlobalBrightnessScale"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::M2PhysicalLightIntensityScale: return FText::FromString(TEXT("M2Light.GlobalIntensity"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::LightDiffuseIntensityScale: return FText::FromString(TEXT("Light.DiffuseScale"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::LightAmbientIntensityScale: return FText::FromString(TEXT("Light.AmbientScale"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::LightDiffuseColorScaleR: return FText::FromString(TEXT("Light.DiffuseColor.r"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::LightDiffuseColorScaleG: return FText::FromString(TEXT("Light.DiffuseColor.g"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::LightDiffuseColorScaleB: return FText::FromString(TEXT("Light.DiffuseColor.b"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::LightAmbientColorScaleR: return FText::FromString(TEXT("Light.AmbientColor.r"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::LightAmbientColorScaleG: return FText::FromString(TEXT("Light.AmbientColor.g"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::LightAmbientColorScaleB: return FText::FromString(TEXT("Light.AmbientColor.b"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::LightAttenuationRadiusScale: return FText::FromString(TEXT("Light.AttenuationScale"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticlePositionX: return FText::FromString(TEXT("M2 Pos.x + UE offset"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticlePositionY: return FText::FromString(TEXT("M2 Pos.y + UE offset"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticlePositionZ: return FText::FromString(TEXT("M2 Pos.z + UE offset"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleEmissionRateScale: return FText::FromString(TEXT("struct ABlock_f Emissionrate | scale"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleEmissionSpeedScale: return FText::FromString(TEXT("struct ABlock_f emissionspeed | scale"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleLifespanScale: return FText::FromString(TEXT("struct ABlock_f Lifespan | scale"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleAreaLengthScale: return FText::FromString(TEXT("struct ABlock_f Emissionarea_length | scale"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleAreaWidthScale: return FText::FromString(TEXT("struct ABlock_f Emissionarea_width | scale"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleGravityScale: return FText::FromString(TEXT("struct ABlock_f Gravity | scale"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleGravity2Scale: return FText::FromString(TEXT("struct ABlock_f Gravity2 | scale"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleVerticalRangeScale: return FText::FromString(TEXT("struct ABlock_f Vertical_range | scale"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleHorizontalRangeScale: return FText::FromString(TEXT("struct ABlock_f Horizontal_range | scale"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleSizeScale: return FText::FromString(TEXT("FakeABlock_ff scaleTrack | size scale"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleLifecycleSizeStartScale: return FText::FromString(TEXT("FakeABlock_ff Scale[0] | start size"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleLifecycleSizeMidScale: return FText::FromString(TEXT("FakeABlock_ff Scale[1] | mid size"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleLifecycleSizeEndScale: return FText::FromString(TEXT("FakeABlock_ff Scale[2] | end size"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleAlphaScale: return FText::FromString(TEXT("FakeABlock_s alphaTrack | alpha scale"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleLifecycleAlphaStartScale: return FText::FromString(TEXT("FakeABlock_s Opacity[0] | start alpha"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleLifecycleAlphaMidScale: return FText::FromString(TEXT("FakeABlock_s Opacity[1] | mid alpha"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleLifecycleAlphaEndScale: return FText::FromString(TEXT("FakeABlock_s Opacity[2] | end alpha"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleAlphaCutoff: return FText::FromString(TEXT("UE AlphaCutoff"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleBrightnessScale: return FText::FromString(TEXT("UE BrightnessScale"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::TextureUVOffsetX: return FText::FromString(TEXT("Texture.UVOffset.x"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::TextureUVOffsetY: return FText::FromString(TEXT("Texture.UVOffset.y"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::TextureUVScaleX: return FText::FromString(TEXT("Texture.UVScale.x"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::TextureUVScaleY: return FText::FromString(TEXT("Texture.UVScale.y"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::TextureUVRotationDegrees: return FText::FromString(TEXT("Texture.UVRotation"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::TextureAlphaScale: return FText::FromString(TEXT("Texture.AlphaScale"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::TextureBrightnessScale: return FText::FromString(TEXT("Texture.Brightness"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::CharacterPreviewLocationX: return FText::FromString(TEXT("Character.LocationOffset.x"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::CharacterPreviewLocationY: return FText::FromString(TEXT("Character.LocationOffset.y"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::CharacterPreviewLocationZ: return FText::FromString(TEXT("Character.LocationOffset.z"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::CharacterPreviewRotationPitch: return FText::FromString(TEXT("Character.RotationOffset.pitch"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::CharacterPreviewRotationYaw: return FText::FromString(TEXT("Character.RotationOffset.yaw"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::CharacterPreviewRotationRoll: return FText::FromString(TEXT("Character.RotationOffset.roll"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::CharacterPreviewUniformScale: return FText::FromString(TEXT("Character.UniformScale"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::CharacterPreviewScaleX: return FText::FromString(TEXT("Character.Scale.x"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::CharacterPreviewScaleY: return FText::FromString(TEXT("Character.Scale.y"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::CharacterPreviewScaleZ: return FText::FromString(TEXT("Character.Scale.z"));
	default: return FText::FromString(TEXT("未知"));
	}
}

FText DebugFieldDescription(SWoWLoginSceneDebugPanel::EDebugFloatField Field)
{
	switch (Field)
	{
	case SWoWLoginSceneDebugPanel::EDebugFloatField::SceneLocationX:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::SceneLocationY:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::SceneLocationZ:
		return FText::FromString(TEXT("UE场景整体位置偏移"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::SceneRotationPitch:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::SceneRotationYaw:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::SceneRotationRoll:
		return FText::FromString(TEXT("UE场景整体旋转偏移"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::SceneScaleX:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::SceneScaleY:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::SceneScaleZ:
		return FText::FromString(TEXT("UE场景整体缩放倍率"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::CharacterPreviewLocationX:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::CharacterPreviewLocationY:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::CharacterPreviewLocationZ:
		return FText::FromString(TEXT("角色选择玩家模型相对自动站位的位置偏移"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::CharacterPreviewRotationPitch:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::CharacterPreviewRotationYaw:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::CharacterPreviewRotationRoll:
		return FText::FromString(TEXT("角色选择玩家模型相对自动朝向的旋转偏移"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::CharacterPreviewScaleX:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::CharacterPreviewScaleY:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::CharacterPreviewScaleZ:
		return FText::FromString(TEXT("角色选择玩家模型相对自动比例的缩放倍率"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::CharacterPreviewUniformScale:
		return FText::FromString(TEXT("角色选择玩家模型的等比例总缩放倍率"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::CameraPositionX:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::CameraPositionY:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::CameraPositionZ:
		return FText::FromString(TEXT("010字段 Pos；相机静态位置，当前值可覆盖保存到UE调参资产"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::CameraTargetX:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::CameraTargetY:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::CameraTargetZ:
		return FText::FromString(TEXT("010字段 Target；相机静态观察点，当前值可覆盖保存到UE调参资产"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::CameraFov:
		return FText::FromString(TEXT("010字段 FOV，原始弧度；UE投影角=FOV*34.5"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::CameraNearClipping:
		return FText::FromString(TEXT("010字段 NearClipping；WoW单位"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::CameraFarClipping:
		return FText::FromString(TEXT("010字段 FarClipping；WoW单位"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::CameraRollEffectX:
		return FText::FromString(TEXT("010字段 RollEffect key value x；横滚弧度"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::GlobalBrightnessScale:
		return FText::FromString(TEXT("UE调参资产字段；乘到所有登录M2 section Brightness，用于整体压暗/提亮"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::M2PhysicalLightIntensityScale:
		return FText::FromString(TEXT("M2物理灯桥接的全局强度倍率；只在桥接开关打开时生效"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::LightDiffuseIntensityScale:
		return FText::FromString(TEXT("当前 Light[i] 的 diffuseIntensity 缩放；桥接关闭时影响材质光照亮度，桥接开启时同时影响UE诊断点光"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::LightAmbientIntensityScale:
		return FText::FromString(TEXT("当前 Light[i] 的 ambientIntensity 缩放；桥接关闭时影响材质环境光亮度"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::LightDiffuseColorScaleR:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::LightDiffuseColorScaleG:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::LightDiffuseColorScaleB:
		return FText::FromString(TEXT("当前 Light[i] 漫反射颜色缩放；用于调冷暖和局部色彩层次"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::LightAmbientColorScaleR:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::LightAmbientColorScaleG:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::LightAmbientColorScaleB:
		return FText::FromString(TEXT("当前 Light[i] 环境色缩放；桥接关闭时参与材质光照色调"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::LightAttenuationRadiusScale:
		return FText::FromString(TEXT("当前 Light[i] 衰减半径缩放；桥接关闭时影响材质光照权重，桥接开启时控制UE诊断点光范围"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticlePositionX:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticlePositionY:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticlePositionZ:
		return FText::FromString(TEXT("粒子发射器位置偏移"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleEmissionRateScale:
		return FText::FromString(TEXT("发射率倍率"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleEmissionSpeedScale:
		return FText::FromString(TEXT("初速度倍率"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleLifespanScale:
		return FText::FromString(TEXT("粒子寿命倍率"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleAreaLengthScale:
		return FText::FromString(TEXT("发射区域长度倍率"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleAreaWidthScale:
		return FText::FromString(TEXT("发射区域宽度倍率"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleGravityScale:
		return FText::FromString(TEXT("重力倍率"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleGravity2Scale:
		return FText::FromString(TEXT("强重力/二段重力倍率"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleVerticalRangeScale:
		return FText::FromString(TEXT("垂直扩散角倍率"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleHorizontalRangeScale:
		return FText::FromString(TEXT("水平扩散角倍率"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleSizeScale:
		return FText::FromString(TEXT("粒子尺寸倍率"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleLifecycleSizeStartScale:
		return FText::FromString(TEXT("粒子起始尺寸键倍率"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleLifecycleSizeMidScale:
		return FText::FromString(TEXT("粒子中段尺寸键倍率"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleLifecycleSizeEndScale:
		return FText::FromString(TEXT("粒子结束尺寸键倍率"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleAlphaScale:
		return FText::FromString(TEXT("透明度倍率"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleLifecycleAlphaStartScale:
		return FText::FromString(TEXT("粒子起始透明度键倍率"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleLifecycleAlphaMidScale:
		return FText::FromString(TEXT("粒子中段透明度键倍率"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleLifecycleAlphaEndScale:
		return FText::FromString(TEXT("粒子结束透明度键倍率"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleAlphaCutoff:
		return FText::FromString(TEXT("低 alpha 截断；用于去掉粒子序列帧贴图透明背景的方块边"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::ParticleBrightnessScale:
		return FText::FromString(TEXT("亮度倍率"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::TextureUVOffsetX:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::TextureUVOffsetY:
		return FText::FromString(TEXT("纹理平移偏移，叠加到 M2 TexAnimation.translation"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::TextureUVScaleX:
	case SWoWLoginSceneDebugPanel::EDebugFloatField::TextureUVScaleY:
		return FText::FromString(TEXT("纹理缩放倍率，叠加到 M2 TexAnimation.scaling"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::TextureUVRotationDegrees:
		return FText::FromString(TEXT("纹理旋转角度，叠加到 M2 TexAnimation.rotation"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::TextureAlphaScale:
		return FText::FromString(TEXT("当前 section 透明度倍率，强制可视化时会放大"));
	case SWoWLoginSceneDebugPanel::EDebugFloatField::TextureBrightnessScale:
		return FText::FromString(TEXT("当前 section 亮度倍率，强制可视化时会放大"));
	default:
		return FText::FromString(TEXT(""));
	}
}

float SampleParticleTrackForDebug(const TArray<AWoWLoginGameMode::FParticleFloatSample>& Samples, float TimeSeconds, float DefaultValue)
{
	if (Samples.IsEmpty())
	{
		return DefaultValue;
	}
	if (Samples.Num() == 1)
	{
		return Samples[0].Value;
	}

	float DurationMs = 0.0f;
	for (const AWoWLoginGameMode::FParticleFloatSample& Sample : Samples)
	{
		DurationMs = FMath::Max(DurationMs, Sample.TimeMs);
	}
	if (DurationMs <= KINDA_SMALL_NUMBER)
	{
		return Samples.Last().Value;
	}

	const float LoopTimeMs = FMath::Fmod(FMath::Max(TimeSeconds, 0.0f) * 1000.0f, DurationMs);
	for (int32 SampleIndex = 0; SampleIndex < Samples.Num() - 1; ++SampleIndex)
	{
		const AWoWLoginGameMode::FParticleFloatSample& A = Samples[SampleIndex];
		const AWoWLoginGameMode::FParticleFloatSample& B = Samples[SampleIndex + 1];
		if (LoopTimeMs <= B.TimeMs)
		{
			const float Ratio = B.TimeMs > A.TimeMs ? (LoopTimeMs - A.TimeMs) / (B.TimeMs - A.TimeMs) : 0.0f;
			return FMath::Lerp(A.Value, B.Value, Ratio);
		}
	}
	return Samples.Last().Value;
}
}

FText SWoWLoginSceneDebugPanel::GetFieldDescriptionText(EDebugFloatField Field) const
{
	const AWoWLoginGameMode* GameMode = LoginGameMode.Get();
	if (!GameMode || !IsParticleField(Field))
	{
		return DebugFieldDescription(Field);
	}

	const TArray<int32> Indices = GameMode->GetLoginParticleEmitterIndices();
	if (Indices.IsEmpty())
	{
		return DebugFieldDescription(Field);
	}

	const int32 EmitterIndex = SelectedParticleEmitterIndex == INDEX_NONE ? Indices[0] : SelectedParticleEmitterIndex;
	const AWoWLoginGameMode::FLoginParticleEmitter* Emitter = GameMode->GetLoginParticleEmitterByIndex(EmitterIndex);
	if (!Emitter)
	{
		return DebugFieldDescription(Field);
	}

	const FWoWLoginParticleEmitterTuning Tuning = GameMode->GetLoginParticleEmitterTuning(EmitterIndex);
	const float TimeSeconds = GameMode->GetWorld() ? GameMode->GetWorld()->GetTimeSeconds() : 0.0f;

	auto MakeScaleText = [](const TCHAR* ChineseName, float BaseValue, float ScaleValue, float FinalValue, int32 KeyCount)
	{
		return FText::FromString(FString::Printf(
			TEXT("%s。010原值=%.4f，轨道键=%d，当前倍率=%.4f，当前生效=%.4f"),
			ChineseName,
			BaseValue,
			KeyCount,
			ScaleValue,
			FinalValue));
	};

	switch (Field)
	{
	case EDebugFloatField::ParticlePositionX:
		return FText::FromString(FString::Printf(TEXT("粒子发射器位置 X。010原值=%.4f，当前UE偏移=%.4f，最终=%.4f"), Emitter->Position.X, Tuning.PositionOffset.X, Emitter->Position.X + Tuning.PositionOffset.X));
	case EDebugFloatField::ParticlePositionY:
		return FText::FromString(FString::Printf(TEXT("粒子发射器位置 Y。010原值=%.4f，当前UE偏移=%.4f，最终=%.4f"), Emitter->Position.Y, Tuning.PositionOffset.Y, Emitter->Position.Y + Tuning.PositionOffset.Y));
	case EDebugFloatField::ParticlePositionZ:
		return FText::FromString(FString::Printf(TEXT("粒子发射器位置 Z。010原值=%.4f，当前UE偏移=%.4f，最终=%.4f"), Emitter->Position.Z, Tuning.PositionOffset.Z, Emitter->Position.Z + Tuning.PositionOffset.Z));
	case EDebugFloatField::ParticleEmissionRateScale:
		return MakeScaleText(TEXT("发射率"), SampleParticleTrackForDebug(Emitter->EmissionRateSamples, TimeSeconds, 0.0f), Tuning.EmissionRateScale, SampleParticleTrackForDebug(Emitter->EmissionRateSamples, TimeSeconds, 0.0f) * Tuning.EmissionRateScale, Emitter->EmissionRateSamples.Num());
	case EDebugFloatField::ParticleEmissionSpeedScale:
		return MakeScaleText(TEXT("初速度"), SampleParticleTrackForDebug(Emitter->EmissionSpeedSamples, TimeSeconds, 0.0f), Tuning.EmissionSpeedScale, SampleParticleTrackForDebug(Emitter->EmissionSpeedSamples, TimeSeconds, 0.0f) * Tuning.EmissionSpeedScale, Emitter->EmissionSpeedSamples.Num());
	case EDebugFloatField::ParticleLifespanScale:
		return MakeScaleText(TEXT("寿命"), SampleParticleTrackForDebug(Emitter->LifespanSamples, TimeSeconds, 0.0f), Tuning.LifespanScale, SampleParticleTrackForDebug(Emitter->LifespanSamples, TimeSeconds, 0.0f) * Tuning.LifespanScale, Emitter->LifespanSamples.Num());
	case EDebugFloatField::ParticleAreaLengthScale:
		return MakeScaleText(TEXT("发射区域长度"), SampleParticleTrackForDebug(Emitter->EmissionAreaLengthSamples, TimeSeconds, 0.0f), Tuning.AreaLengthScale, SampleParticleTrackForDebug(Emitter->EmissionAreaLengthSamples, TimeSeconds, 0.0f) * Tuning.AreaLengthScale, Emitter->EmissionAreaLengthSamples.Num());
	case EDebugFloatField::ParticleAreaWidthScale:
		return MakeScaleText(TEXT("发射区域宽度"), SampleParticleTrackForDebug(Emitter->EmissionAreaWidthSamples, TimeSeconds, 0.0f), Tuning.AreaWidthScale, SampleParticleTrackForDebug(Emitter->EmissionAreaWidthSamples, TimeSeconds, 0.0f) * Tuning.AreaWidthScale, Emitter->EmissionAreaWidthSamples.Num());
	case EDebugFloatField::ParticleGravityScale:
		return MakeScaleText(TEXT("重力"), SampleParticleTrackForDebug(Emitter->GravitySamples, TimeSeconds, 0.0f), Tuning.GravityScale, SampleParticleTrackForDebug(Emitter->GravitySamples, TimeSeconds, 0.0f) * Tuning.GravityScale, Emitter->GravitySamples.Num());
	case EDebugFloatField::ParticleGravity2Scale:
		return MakeScaleText(TEXT("二段重力/减速"), SampleParticleTrackForDebug(Emitter->Gravity2Samples, TimeSeconds, 0.0f), Tuning.Gravity2Scale, SampleParticleTrackForDebug(Emitter->Gravity2Samples, TimeSeconds, 0.0f) * Tuning.Gravity2Scale, Emitter->Gravity2Samples.Num());
	case EDebugFloatField::ParticleVerticalRangeScale:
		return MakeScaleText(TEXT("垂直角范围"), SampleParticleTrackForDebug(Emitter->VerticalRangeSamples, TimeSeconds, 0.0f), Tuning.VerticalRangeScale, SampleParticleTrackForDebug(Emitter->VerticalRangeSamples, TimeSeconds, 0.0f) * Tuning.VerticalRangeScale, Emitter->VerticalRangeSamples.Num());
	case EDebugFloatField::ParticleHorizontalRangeScale:
		return MakeScaleText(TEXT("水平角范围"), SampleParticleTrackForDebug(Emitter->HorizontalRangeSamples, TimeSeconds, 0.0f), Tuning.HorizontalRangeScale, SampleParticleTrackForDebug(Emitter->HorizontalRangeSamples, TimeSeconds, 0.0f) * Tuning.HorizontalRangeScale, Emitter->HorizontalRangeSamples.Num());
	case EDebugFloatField::ParticleSizeScale:
		return FText::FromString(FString::Printf(TEXT("粒子尺寸倍率。010原轨道=FakeABlock_ff scaleTrack，当前倍率=%.4f"), Tuning.SizeScale));
	case EDebugFloatField::ParticleLifecycleSizeStartScale:
		return FText::FromString(FString::Printf(TEXT("粒子起始尺寸键。010原值=%.6f，当前倍率=%.4f，当前生效=%.6f"), Emitter->LifecycleSizes[0], Tuning.LifecycleSizeStartScale, Emitter->LifecycleSizes[0] * Tuning.LifecycleSizeStartScale));
	case EDebugFloatField::ParticleLifecycleSizeMidScale:
		return FText::FromString(FString::Printf(TEXT("粒子中段尺寸键。010原值=%.6f，当前倍率=%.4f，当前生效=%.6f"), Emitter->LifecycleSizes[1], Tuning.LifecycleSizeMidScale, Emitter->LifecycleSizes[1] * Tuning.LifecycleSizeMidScale));
	case EDebugFloatField::ParticleLifecycleSizeEndScale:
		return FText::FromString(FString::Printf(TEXT("粒子结束尺寸键。010原值=%.6f，当前倍率=%.4f，当前生效=%.6f"), Emitter->LifecycleSizes[2], Tuning.LifecycleSizeEndScale, Emitter->LifecycleSizes[2] * Tuning.LifecycleSizeEndScale));
	case EDebugFloatField::ParticleAlphaScale:
		return FText::FromString(FString::Printf(TEXT("粒子透明度倍率。010原轨道=FakeABlock_s alphaTrack，当前倍率=%.4f"), Tuning.AlphaScale));
	case EDebugFloatField::ParticleLifecycleAlphaStartScale:
		return FText::FromString(FString::Printf(TEXT("粒子起始透明度键。010原值=%.6f，当前倍率=%.4f，当前生效=%.6f"), Emitter->LifecycleColors[0].A, Tuning.LifecycleAlphaStartScale, Emitter->LifecycleColors[0].A * Tuning.LifecycleAlphaStartScale));
	case EDebugFloatField::ParticleLifecycleAlphaMidScale:
		return FText::FromString(FString::Printf(TEXT("粒子中段透明度键。010原值=%.6f，当前倍率=%.4f，当前生效=%.6f"), Emitter->LifecycleColors[1].A, Tuning.LifecycleAlphaMidScale, Emitter->LifecycleColors[1].A * Tuning.LifecycleAlphaMidScale));
	case EDebugFloatField::ParticleLifecycleAlphaEndScale:
		return FText::FromString(FString::Printf(TEXT("粒子结束透明度键。010原值=%.6f，当前倍率=%.4f，当前生效=%.6f"), Emitter->LifecycleColors[2].A, Tuning.LifecycleAlphaEndScale, Emitter->LifecycleColors[2].A * Tuning.LifecycleAlphaEndScale));
	case EDebugFloatField::ParticleAlphaCutoff:
		return FText::FromString(FString::Printf(TEXT("UE专用透明裁剪阈值，不是010原始字段。当前=%.4f"), Tuning.AlphaCutoff));
	case EDebugFloatField::ParticleBrightnessScale:
		return FText::FromString(FString::Printf(TEXT("UE专用亮度倍率，不是010原始字段。当前=%.4f"), Tuning.BrightnessScale));
	default:
		return DebugFieldDescription(Field);
	}
}

void SWoWLoginSceneDebugPanel::Construct(const FArguments& InArgs)
{
	LoginGameMode = InArgs._LoginGameMode;
	OnCloseRequested = InArgs._OnCloseRequested;

	auto MakeFieldRow = [this](EDebugFloatField Field)
	{
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.0f, 4.0f, 8.0f, 4.0f)
			[
				SNew(SBox)
				.WidthOverride(230.0f)
				[
					SNew(STextBlock)
					.ColorAndOpacity(TextColor)
					.Text_Lambda([Field]()
					{
						return DebugFieldLabel(Field);
					})
				]
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.0f, 4.0f, 8.0f, 4.0f)
			[
				SNew(SBox)
				.WidthOverride(112.0f)
				[
					SNew(SNumericEntryBox<float>)
					.AllowSpin(false)
					.MinDesiredValueWidth(88.0f)
					.Value(this, &SWoWLoginSceneDebugPanel::GetFieldNumericValue, Field)
					.OnValueCommitted(this, &SWoWLoginSceneDebugPanel::OnFieldNumericCommitted, Field)
				]
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			.Padding(0.0f, 4.0f, 8.0f, 4.0f)
			[
				SNew(SWheelFineTuneSlider)
				.Value(this, &SWoWLoginSceneDebugPanel::GetSliderValue, Field)
				.OnValueChanged(this, &SWoWLoginSceneDebugPanel::OnSliderChanged, Field)
				.OnWheelDelta(TDelegate<void(float)>::CreateSP(this, &SWoWLoginSceneDebugPanel::OnSliderWheelDelta, Field))
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.0f, 4.0f, 8.0f, 4.0f)
			[
				SNew(SButton)
				.Text(FText::FromString(TEXT("重置")))
				.OnClicked(this, &SWoWLoginSceneDebugPanel::OnResetFieldClicked, Field)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.0f, 4.0f)
			[
				SNew(SBox)
				.WidthOverride(250.0f)
				[
					SNew(STextBlock)
					.ColorAndOpacity(MutedTextColor)
					.AutoWrapText(true)
					.Text(this, &SWoWLoginSceneDebugPanel::GetFieldDescriptionText, Field)
				]
			];
	};

	auto MakeReadOnlyCameraIdRow = [this]()
	{
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.0f, 4.0f, 10.0f, 4.0f)
			[
				SNew(SBox)
				.WidthOverride(230.0f)
				[
					SNew(STextBlock)
					.ColorAndOpacity(TextColor)
					.Text_Lambda([this]()
					{
						if (const AWoWLoginGameMode* GameMode = LoginGameMode.Get())
						{
							return FText::FromString(FString::Printf(TEXT("enum E_CAMERATYPE Id: %d"), GameMode->GetLoginSceneM2CameraState().Id));
						}
						return FText::FromString(TEXT("enum E_CAMERATYPE Id: -1"));
					})
				]
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			.Padding(0.0f, 4.0f)
			[
				SNew(STextBlock)
				.ColorAndOpacity(MutedTextColor)
				.Text(FText::FromString(TEXT("010字段 Id；当前模型为 CT_FlyBy(-1)，只读")))
			];
	};

	auto MakeReadOnlyCameraBlockRow = [](const FText& FieldName, const FText& Description)
	{
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.0f, 4.0f, 10.0f, 4.0f)
			[
				SNew(SBox)
				.WidthOverride(230.0f)
				[
					SNew(STextBlock)
					.ColorAndOpacity(TextColor)
					.Text(FieldName)
				]
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			.Padding(0.0f, 4.0f)
			[
				SNew(STextBlock)
				.ColorAndOpacity(MutedTextColor)
				.AutoWrapText(true)
				.Text(Description)
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
				.FillHeight(1.0f)
				[
					Content
				]
			];
	};

	auto MakeTabButton = [this](const FText& Label, EDebugTab Tab)
	{
		return SNew(SButton)
			.ContentPadding(FMargin(14.0f, 5.0f))
			.ButtonColorAndOpacity_Lambda([this, Tab]()
			{
				return ActiveTab == Tab ? FLinearColor(0.78f, 0.57f, 0.22f, 1.0f) : FLinearColor(0.12f, 0.13f, 0.15f, 1.0f);
			})
			.ForegroundColor_Lambda([this, Tab]()
			{
				return ActiveTab == Tab ? FSlateColor(FLinearColor(0.035f, 0.041f, 0.052f, 1.0f)) : FSlateColor(TextColor);
			})
			.Text(Label)
			.OnClicked(this, &SWoWLoginSceneDebugPanel::OnTabClicked, Tab);
	};

	auto MakeNoteRow = [this]()
	{
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
			[
				SNew(STextBlock)
				.ColorAndOpacity(TextColor)
				.Text(FText::FromString(TEXT("备注")))
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f)
			[
				SNew(SEditableTextBox)
				.HintText(FText::FromString(TEXT("给当前项写备注，例如：大门旋转粒子。点击保存资产后下次仍保留。")))
				.Text(this, &SWoWLoginSceneDebugPanel::GetSelectedNoteText)
				.OnTextCommitted(this, &SWoWLoginSceneDebugPanel::OnNoteCommitted)
			];
	};

	auto MakeRawDetailsToggle = [this]()
	{
		return SNew(SButton)
			.Text_Lambda([this]()
			{
				return FText::FromString(bShowRawDetails ? TEXT("隐藏原始字段") : TEXT("显示原始字段"));
			})
			.OnClicked(this, &SWoWLoginSceneDebugPanel::OnToggleRawDetailsClicked);
	};

	auto MakeTabScroll = [](TSharedRef<SWidget> Content)
	{
		return SNew(SScrollBox)
			+ SScrollBox::Slot()
			[
				Content
			];
	};

	auto MakeParamsSection = [this, MakeRawDetailsToggle]()
	{
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				SNew(STextBlock)
				.ColorAndOpacity(MutedTextColor)
				.AutoWrapText(true)
				.Text(FText::FromString(TEXT("当前 Scene 页只显示可调资产字段。导出文本默认收起，需要复制/对照时再展开。")))
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				MakeRawDetailsToggle()
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(STextBlock)
				.ColorAndOpacity(MutedTextColor)
				.AutoWrapText(true)
				.Text(this, &SWoWLoginSceneDebugPanel::GetSceneRawDetailsText)
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 6.0f, 0.0f, 0.0f)
			[
				SNew(SBox)
				.Visibility_Lambda([this]()
				{
					return bShowRawDetails ? EVisibility::Visible : EVisibility::Collapsed;
				})
				[
					SNew(SEditableTextBox)
					.IsReadOnly(true)
					.SelectAllTextWhenFocused(true)
					.Text(this, &SWoWLoginSceneDebugPanel::GetExportText)
				]
			];
	};

	auto MakeMappingSection = [this]()
	{
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(STextBlock)
				.ColorAndOpacity(TextColor)
				.AutoWrapText(true)
				.Text(this, &SWoWLoginSceneDebugPanel::GetMappingHelpText)
			];
	};

	auto MakeSceneTransformSection = [this, MakeFieldRow]()
	{
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					SNew(SButton)
					.Text(FText::FromString(TEXT("使用M2原始相机")))
					.OnClicked(this, &SWoWLoginSceneDebugPanel::OnResetAllInTabClicked)
				]
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.ColorAndOpacity(MutedTextColor)
					.AutoWrapText(true)
					.Text(FText::FromString(TEXT("清空当前UE调参资产里的相机偏移，回到M2 cameras[] 的 Pos/Target/FOV/Near/Far。确认效果后点击“保存资产”。")))
				]
			]
			+ SVerticalBox::Slot().AutoHeight()[MakeFieldRow(EDebugFloatField::SceneLocationX)]
			+ SVerticalBox::Slot().AutoHeight()[MakeFieldRow(EDebugFloatField::SceneLocationY)]
			+ SVerticalBox::Slot().AutoHeight()[MakeFieldRow(EDebugFloatField::SceneLocationZ)]
			+ SVerticalBox::Slot().AutoHeight()[MakeFieldRow(EDebugFloatField::SceneRotationPitch)]
			+ SVerticalBox::Slot().AutoHeight()[MakeFieldRow(EDebugFloatField::SceneRotationYaw)]
			+ SVerticalBox::Slot().AutoHeight()[MakeFieldRow(EDebugFloatField::SceneRotationRoll)]
			+ SVerticalBox::Slot().AutoHeight()[MakeFieldRow(EDebugFloatField::SceneScaleX)]
			+ SVerticalBox::Slot().AutoHeight()[MakeFieldRow(EDebugFloatField::SceneScaleY)]
			+ SVerticalBox::Slot().AutoHeight()[MakeFieldRow(EDebugFloatField::SceneScaleZ)];
	};

	auto MakeCameraBaseSection = [this, MakeFieldRow, MakeReadOnlyCameraIdRow, MakeReadOnlyCameraBlockRow]()
	{
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					SNew(SButton)
					.Text(FText::FromString(TEXT("重置当前页")))
					.OnClicked(this, &SWoWLoginSceneDebugPanel::OnResetAllInTabClicked)
				]
			]
			+ SVerticalBox::Slot().AutoHeight()[MakeReadOnlyCameraIdRow()]
			+ SVerticalBox::Slot().AutoHeight()[MakeFieldRow(EDebugFloatField::CameraFov)]
			+ SVerticalBox::Slot().AutoHeight()[MakeFieldRow(EDebugFloatField::CameraFarClipping)]
			+ SVerticalBox::Slot().AutoHeight()[MakeFieldRow(EDebugFloatField::CameraNearClipping)]
			+ SVerticalBox::Slot().AutoHeight()[MakeReadOnlyCameraBlockRow(
				FText::FromString(TEXT("struct ABlock_BF TranslationPos")),
				FText::FromString(TEXT("010字段；相机位置动画块，已解析为 position_samples。具体摘要见下方 ABlock 区。")))]
			+ SVerticalBox::Slot().AutoHeight()[MakeFieldRow(EDebugFloatField::CameraPositionX)]
			+ SVerticalBox::Slot().AutoHeight()[MakeFieldRow(EDebugFloatField::CameraPositionY)]
			+ SVerticalBox::Slot().AutoHeight()[MakeFieldRow(EDebugFloatField::CameraPositionZ)]
			+ SVerticalBox::Slot().AutoHeight()[MakeReadOnlyCameraBlockRow(
				FText::FromString(TEXT("struct ABlock_BF TranslationTar")),
				FText::FromString(TEXT("010字段；相机目标点动画块，已解析为 target_samples。具体摘要见下方 ABlock 区。")))]
			+ SVerticalBox::Slot().AutoHeight()[MakeFieldRow(EDebugFloatField::CameraTargetX)]
			+ SVerticalBox::Slot().AutoHeight()[MakeFieldRow(EDebugFloatField::CameraTargetY)]
			+ SVerticalBox::Slot().AutoHeight()[MakeFieldRow(EDebugFloatField::CameraTargetZ)]
			+ SVerticalBox::Slot().AutoHeight()[MakeReadOnlyCameraBlockRow(
				FText::FromString(TEXT("struct ABlock_F RollEffect")),
				FText::FromString(TEXT("010字段；横滚动画块，val.x 可通过下面 RollEffect.val.x 调节。")))]
			+ SVerticalBox::Slot().AutoHeight()[MakeFieldRow(EDebugFloatField::CameraRollEffectX)];
	};

	auto MakeCameraAnimBlockSection = [this]()
	{
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(STextBlock)
				.ColorAndOpacity(MutedTextColor)
				.AutoWrapText(true)
				.Text(this, &SWoWLoginSceneDebugPanel::GetCameraAnimBlockSummaryText)
			];
	};

	auto MakeParticleSection = [this, MakeFieldRow, MakeNoteRow, MakeRawDetailsToggle]()
	{
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					SNew(SButton)
					.Text(FText::FromString(TEXT("上一项")))
					.OnClicked(this, &SWoWLoginSceneDebugPanel::OnPrevParticleClicked)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					SNew(SButton)
					.Text(FText::FromString(TEXT("下一项")))
					.OnClicked(this, &SWoWLoginSceneDebugPanel::OnNextParticleClicked)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					SNew(SButton)
					.Text(FText::FromString(TEXT("启用/禁用")))
					.OnClicked(this, &SWoWLoginSceneDebugPanel::OnToggleParticleEnabledClicked)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					SNew(SButton)
					.Text(FText::FromString(TEXT("清除位置偏移")))
					.OnClicked(this, &SWoWLoginSceneDebugPanel::OnClearParticlePositionOffsetClicked)
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					SNew(SButton)
					.Text(FText::FromString(TEXT("重置当前")))
					.OnClicked(this, &SWoWLoginSceneDebugPanel::OnResetCurrentClicked)
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					SNew(SButton)
					.Text(FText::FromString(TEXT("重置所有")))
					.OnClicked(this, &SWoWLoginSceneDebugPanel::OnResetAllInTabClicked)
				]
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.ColorAndOpacity(TextColor)
					.Text(this, &SWoWLoginSceneDebugPanel::GetSelectedParticleText)
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				MakeNoteRow()
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				MakeRawDetailsToggle()
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				SNew(STextBlock)
				.ColorAndOpacity(MutedTextColor)
				.AutoWrapText(true)
				.Text(this, &SWoWLoginSceneDebugPanel::GetSelectedParticleDetailsText)
			]
			+ SVerticalBox::Slot().AutoHeight()[MakeFieldRow(EDebugFloatField::ParticlePositionX)]
			+ SVerticalBox::Slot().AutoHeight()[MakeFieldRow(EDebugFloatField::ParticlePositionY)]
			+ SVerticalBox::Slot().AutoHeight()[MakeFieldRow(EDebugFloatField::ParticlePositionZ)]
			+ SVerticalBox::Slot().AutoHeight()[MakeFieldRow(EDebugFloatField::ParticleEmissionRateScale)]
			+ SVerticalBox::Slot().AutoHeight()[MakeFieldRow(EDebugFloatField::ParticleEmissionSpeedScale)]
			+ SVerticalBox::Slot().AutoHeight()[MakeFieldRow(EDebugFloatField::ParticleLifespanScale)]
			+ SVerticalBox::Slot().AutoHeight()[MakeFieldRow(EDebugFloatField::ParticleAreaLengthScale)]
			+ SVerticalBox::Slot().AutoHeight()[MakeFieldRow(EDebugFloatField::ParticleAreaWidthScale)]
			+ SVerticalBox::Slot().AutoHeight()[MakeFieldRow(EDebugFloatField::ParticleVerticalRangeScale)]
			+ SVerticalBox::Slot().AutoHeight()[MakeFieldRow(EDebugFloatField::ParticleHorizontalRangeScale)]
			+ SVerticalBox::Slot().AutoHeight()[MakeFieldRow(EDebugFloatField::ParticleGravityScale)]
			+ SVerticalBox::Slot().AutoHeight()[MakeFieldRow(EDebugFloatField::ParticleGravity2Scale)]
			+ SVerticalBox::Slot().AutoHeight()[MakeFieldRow(EDebugFloatField::ParticleSizeScale)]
			+ SVerticalBox::Slot().AutoHeight()[MakeFieldRow(EDebugFloatField::ParticleLifecycleSizeStartScale)]
			+ SVerticalBox::Slot().AutoHeight()[MakeFieldRow(EDebugFloatField::ParticleLifecycleSizeMidScale)]
			+ SVerticalBox::Slot().AutoHeight()[MakeFieldRow(EDebugFloatField::ParticleLifecycleSizeEndScale)]
			+ SVerticalBox::Slot().AutoHeight()[MakeFieldRow(EDebugFloatField::ParticleAlphaScale)]
			+ SVerticalBox::Slot().AutoHeight()[MakeFieldRow(EDebugFloatField::ParticleLifecycleAlphaStartScale)]
			+ SVerticalBox::Slot().AutoHeight()[MakeFieldRow(EDebugFloatField::ParticleLifecycleAlphaMidScale)]
			+ SVerticalBox::Slot().AutoHeight()[MakeFieldRow(EDebugFloatField::ParticleLifecycleAlphaEndScale)]
			+ SVerticalBox::Slot().AutoHeight()[MakeFieldRow(EDebugFloatField::ParticleAlphaCutoff)]
			+ SVerticalBox::Slot().AutoHeight()[MakeFieldRow(EDebugFloatField::ParticleBrightnessScale)];
	};

	auto MakeSceneTab = [MakeSectionPanel, MakeParamsSection, MakeMappingSection, MakeSceneTransformSection]()
	{
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
			[
				MakeSectionPanel(FText::FromString(TEXT("场景偏移")), MakeSceneTransformSection())
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
			[
				MakeSectionPanel(FText::FromString(TEXT("参数文本")), MakeParamsSection())
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				MakeSectionPanel(FText::FromString(TEXT("M2 字段对照")), MakeMappingSection())
			];
	};

	auto MakeCameraRawDetailsSection = [this, MakeRawDetailsToggle]()
	{
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				MakeRawDetailsToggle()
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(STextBlock)
				.ColorAndOpacity(MutedTextColor)
				.AutoWrapText(true)
				.Text(this, &SWoWLoginSceneDebugPanel::GetCameraRawDetailsText)
			];
	};

	auto MakeCharacterPreviewSection = [this, MakeFieldRow]()
	{
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				SNew(STextBlock)
				.ColorAndOpacity(MutedTextColor)
				.AutoWrapText(true)
				.Text(FText::FromString(TEXT("这里调整角色选择界面中央玩家模型。数值会叠加在自动站位基础上；点击“保存资产”后随当前登录场景调参资产一起保存。")))
			]
			+ SVerticalBox::Slot().AutoHeight()[MakeFieldRow(EDebugFloatField::CharacterPreviewLocationX)]
			+ SVerticalBox::Slot().AutoHeight()[MakeFieldRow(EDebugFloatField::CharacterPreviewLocationY)]
			+ SVerticalBox::Slot().AutoHeight()[MakeFieldRow(EDebugFloatField::CharacterPreviewLocationZ)]
			+ SVerticalBox::Slot().AutoHeight()[MakeFieldRow(EDebugFloatField::CharacterPreviewRotationPitch)]
			+ SVerticalBox::Slot().AutoHeight()[MakeFieldRow(EDebugFloatField::CharacterPreviewRotationYaw)]
			+ SVerticalBox::Slot().AutoHeight()[MakeFieldRow(EDebugFloatField::CharacterPreviewRotationRoll)]
			+ SVerticalBox::Slot().AutoHeight()[MakeFieldRow(EDebugFloatField::CharacterPreviewUniformScale)]
			+ SVerticalBox::Slot().AutoHeight()[MakeFieldRow(EDebugFloatField::CharacterPreviewScaleX)]
			+ SVerticalBox::Slot().AutoHeight()[MakeFieldRow(EDebugFloatField::CharacterPreviewScaleY)]
			+ SVerticalBox::Slot().AutoHeight()[MakeFieldRow(EDebugFloatField::CharacterPreviewScaleZ)]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 4.0f)
			[
				SNew(STextBlock)
				.ColorAndOpacity(TextColor)
				.Text(FText::FromString(TEXT("Preview Animation")))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 4.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 4.0f, 0.0f)
				[
					SNew(SButton)
					.Text(FText::FromString(TEXT("上一项")))
					.OnClicked(this, &SWoWLoginSceneDebugPanel::OnPrevCharacterPreviewAnimationClicked)
				]
				+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(0.0f, 0.0f, 4.0f, 0.0f)
				[
					MakeCharacterPreviewAnimationCombo()
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SButton)
					.Text(FText::FromString(TEXT("下一项")))
					.OnClicked(this, &SWoWLoginSceneDebugPanel::OnNextCharacterPreviewAnimationClicked)
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(4.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SButton)
					.Text(FText::FromString(TEXT("清空覆盖")))
					.OnClicked(this, &SWoWLoginSceneDebugPanel::OnClearCharacterPreviewAnimationClicked)
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 4.0f)
			[
				SNew(STextBlock)
				.ColorAndOpacity(MutedTextColor)
				.AutoWrapText(true)
				.Text(FText::FromString(TEXT("下拉按当前角色已导入的 WoW 动作 ID 选择 Stand/Walk/Run/Ready/Jump 等逻辑动作。保存资产后会固定这个预览姿态。")))
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SEditableTextBox)
				.SelectAllTextWhenFocused(true)
				.HintText(FText::FromString(TEXT("留空使用当前角色自动 Stand；可手填 /Game/.../A_xxx.A_xxx")))
				.Text(this, &SWoWLoginSceneDebugPanel::GetCharacterPreviewAnimationOverrideText)
				.OnTextCommitted(this, &SWoWLoginSceneDebugPanel::OnCharacterPreviewAnimationOverrideCommitted)
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 0.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					SNew(SButton)
					.Text(FText::FromString(TEXT("复制角色调试")))
					.OnClicked(this, &SWoWLoginSceneDebugPanel::OnCopyCharacterDebugClicked)
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SButton)
					.Text(FText::FromString(TEXT("导出角色日志")))
					.OnClicked(this, &SWoWLoginSceneDebugPanel::OnExportCharacterDebugLogClicked)
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 0.0f)
			[
				SNew(SMultiLineEditableTextBox)
				.IsReadOnly(true)
				.AutoWrapText(true)
				.Text(this, &SWoWLoginSceneDebugPanel::GetCharacterPreviewDebugText)
			];
	};

	auto MakeCameraTab = [MakeSectionPanel, MakeCameraBaseSection, MakeCameraAnimBlockSection, MakeCameraRawDetailsSection]()
	{
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
			[
				MakeSectionPanel(FText::FromString(TEXT("struct Cameras _Camera 基础字段")), MakeCameraBaseSection())
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
			[
				MakeSectionPanel(FText::FromString(TEXT("010 原始结构")), MakeCameraRawDetailsSection())
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				MakeSectionPanel(FText::FromString(TEXT("ABlock 动画字段对照")), MakeCameraAnimBlockSection())
			];
	};

	auto MakeLightingSection = [this, MakeFieldRow, MakeNoteRow, MakeRawDetailsToggle]()
	{
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
			[
				SNew(STextBlock)
				.ColorAndOpacity(MutedTextColor)
				.AutoWrapText(true)
				.Text(this, &SWoWLoginSceneDebugPanel::GetLightingSummaryText)
			]
			+ SVerticalBox::Slot().AutoHeight()[MakeFieldRow(EDebugFloatField::GlobalBrightnessScale)]
			+ SVerticalBox::Slot().AutoHeight()[MakeFieldRow(EDebugFloatField::M2PhysicalLightIntensityScale)]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 6.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					SNew(SButton)
					.Text(FText::FromString(TEXT("上一盏"))).OnClicked(this, &SWoWLoginSceneDebugPanel::OnPrevLightClicked)
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					SNew(SButton)
					.Text(FText::FromString(TEXT("下一盏"))).OnClicked(this, &SWoWLoginSceneDebugPanel::OnNextLightClicked)
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					SNew(SButton)
					.Text(FText::FromString(TEXT("M2物理灯桥接 开/关"))).OnClicked(this, &SWoWLoginSceneDebugPanel::OnToggleLightBridgeClicked)
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					SNew(SButton)
					.Text(FText::FromString(TEXT("当前灯 开/关"))).OnClicked(this, &SWoWLoginSceneDebugPanel::OnToggleCurrentLightClicked)
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					SNew(SButton)
					.Text(FText::FromString(TEXT("重置当前"))).OnClicked(this, &SWoWLoginSceneDebugPanel::OnResetCurrentClicked)
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					SNew(SButton)
					.Text(FText::FromString(TEXT("重置所有"))).OnClicked(this, &SWoWLoginSceneDebugPanel::OnResetAllInTabClicked)
				]
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.ColorAndOpacity(TextColor)
					.Text(this, &SWoWLoginSceneDebugPanel::GetSelectedLightText)
				]
			]
			+ SVerticalBox::Slot().AutoHeight()[MakeFieldRow(EDebugFloatField::LightDiffuseIntensityScale)]
			+ SVerticalBox::Slot().AutoHeight()[MakeFieldRow(EDebugFloatField::LightAmbientIntensityScale)]
			+ SVerticalBox::Slot().AutoHeight()[MakeFieldRow(EDebugFloatField::LightDiffuseColorScaleR)]
			+ SVerticalBox::Slot().AutoHeight()[MakeFieldRow(EDebugFloatField::LightDiffuseColorScaleG)]
			+ SVerticalBox::Slot().AutoHeight()[MakeFieldRow(EDebugFloatField::LightDiffuseColorScaleB)]
			+ SVerticalBox::Slot().AutoHeight()[MakeFieldRow(EDebugFloatField::LightAmbientColorScaleR)]
			+ SVerticalBox::Slot().AutoHeight()[MakeFieldRow(EDebugFloatField::LightAmbientColorScaleG)]
			+ SVerticalBox::Slot().AutoHeight()[MakeFieldRow(EDebugFloatField::LightAmbientColorScaleB)]
			+ SVerticalBox::Slot().AutoHeight()[MakeFieldRow(EDebugFloatField::LightAttenuationRadiusScale)]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 0.0f)
			[
				MakeNoteRow()
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 0.0f)
			[
				MakeRawDetailsToggle()
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.ColorAndOpacity(MutedTextColor)
				.AutoWrapText(true)
				.Text(this, &SWoWLoginSceneDebugPanel::GetM2LightsDetailsText)
			];
	};

	auto MakeLightingTab = [MakeSectionPanel, MakeLightingSection]()
	{
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				MakeSectionPanel(FText::FromString(TEXT("M2 Lights / 逐灯与全局亮度")), MakeLightingSection())
			];
	};

	auto MakeParticlesTab = [MakeSectionPanel, MakeParticleSection]()
	{
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				MakeSectionPanel(FText::FromString(TEXT("ParticleEmitters 粒子调试")), MakeParticleSection())
			];
	};

	auto MakeTextureSection = [this, MakeFieldRow, MakeNoteRow, MakeRawDetailsToggle]()
	{
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					SNew(SButton)
					.Text(FText::FromString(TEXT("上一项")))
					.OnClicked(this, &SWoWLoginSceneDebugPanel::OnPrevTextureClicked)
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					SNew(SButton)
					.Text(FText::FromString(TEXT("下一项")))
					.OnClicked(this, &SWoWLoginSceneDebugPanel::OnNextTextureClicked)
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					SNew(SButton)
					.Text(FText::FromString(TEXT("M2动画开关")))
					.OnClicked(this, &SWoWLoginSceneDebugPanel::OnToggleTextureM2AnimationClicked)
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					SNew(SButton)
					.Text(FText::FromString(TEXT("显示/隐藏当前")))
					.OnClicked(this, &SWoWLoginSceneDebugPanel::OnToggleTextureEnabledClicked)
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					SNew(SButton)
					.Text(FText::FromString(TEXT("强制可视化")))
					.OnClicked(this, &SWoWLoginSceneDebugPanel::OnToggleTextureForceVisibleClicked)
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					SNew(SButton)
					.Text(FText::FromString(TEXT("重置当前")))
					.OnClicked(this, &SWoWLoginSceneDebugPanel::OnResetCurrentClicked)
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					SNew(SButton)
					.Text(FText::FromString(TEXT("重置所有")))
					.OnClicked(this, &SWoWLoginSceneDebugPanel::OnResetAllInTabClicked)
				]
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.ColorAndOpacity(TextColor)
					.Text(this, &SWoWLoginSceneDebugPanel::GetSelectedTextureText)
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				MakeNoteRow()
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				MakeRawDetailsToggle()
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				SNew(STextBlock)
				.ColorAndOpacity(MutedTextColor)
				.AutoWrapText(true)
				.Text(this, &SWoWLoginSceneDebugPanel::GetSelectedTextureDetailsText)
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 4.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
				[
					SNew(STextBlock)
					.MinDesiredWidth(160.0f)
					.ColorAndOpacity(TextColor)
					.Text(FText::FromString(TEXT("UE Render Override")))
				]
				+ SHorizontalBox::Slot().FillWidth(1.0f).MaxWidth(360.0f)
				[
					MakeTextureUERenderOverrideCombo()
				]
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(8.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.ColorAndOpacity(MutedTextColor)
					.AutoWrapText(true)
					.Text(FText::FromString(TEXT("UE标准/近似混合模式诊断覆盖；不改变 M2 原始 flag/blendingmode。")))
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 4.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
				[
					SNew(STextBlock)
					.MinDesiredWidth(160.0f)
					.ColorAndOpacity(TextColor)
					.Text(FText::FromString(TEXT("Materials.flag")))
				]
				+ SHorizontalBox::Slot().FillWidth(1.0f).MaxWidth(360.0f)
				[
					MakeTextureRenderFlagCombo()
				]
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(8.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.ColorAndOpacity(MutedTextColor)
					.AutoWrapText(true)
					.Text(FText::FromString(TEXT("010 E_RENDERFLAGS；控制不受光照、雾、双面、深度测试、公告板等渲染标志。")))
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
				[
					SNew(STextBlock)
					.MinDesiredWidth(160.0f)
					.ColorAndOpacity(TextColor)
					.Text(FText::FromString(TEXT("Materials.blendingmode")))
				]
				+ SHorizontalBox::Slot().FillWidth(1.0f).MaxWidth(360.0f)
				[
					MakeTextureBlendModeCombo()
				]
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(8.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.ColorAndOpacity(MutedTextColor)
					.AutoWrapText(true)
					.Text(FText::FromString(TEXT("010 E_RENDERMODES；控制不透明、AlphaTest、AlphaBlend、Additive 等混合方式。")))
				]
			]
			+ SVerticalBox::Slot().AutoHeight()[MakeFieldRow(EDebugFloatField::TextureUVOffsetX)]
			+ SVerticalBox::Slot().AutoHeight()[MakeFieldRow(EDebugFloatField::TextureUVOffsetY)]
			+ SVerticalBox::Slot().AutoHeight()[MakeFieldRow(EDebugFloatField::TextureUVScaleX)]
			+ SVerticalBox::Slot().AutoHeight()[MakeFieldRow(EDebugFloatField::TextureUVScaleY)]
			+ SVerticalBox::Slot().AutoHeight()[MakeFieldRow(EDebugFloatField::TextureUVRotationDegrees)]
			+ SVerticalBox::Slot().AutoHeight()[MakeFieldRow(EDebugFloatField::TextureAlphaScale)]
			+ SVerticalBox::Slot().AutoHeight()[MakeFieldRow(EDebugFloatField::TextureBrightnessScale)];
	};

	auto MakeTexturesTab = [MakeSectionPanel, MakeTextureSection]()
	{
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				MakeSectionPanel(FText::FromString(TEXT("Render Sections / TexAnimations 渲染批次调试")), MakeTextureSection())
			];
	};

	auto MakeBonesSection = [this]()
	{
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					SNew(SButton)
					.Text(FText::FromString(TEXT("上一根")))
					.OnClicked(this, &SWoWLoginSceneDebugPanel::OnPrevBoneClicked)
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					SNew(SButton)
					.Text(FText::FromString(TEXT("下一根")))
					.OnClicked(this, &SWoWLoginSceneDebugPanel::OnNextBoneClicked)
				]
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.ColorAndOpacity(TextColor)
					.Text(this, &SWoWLoginSceneDebugPanel::GetSelectedBoneText)
				]
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(STextBlock)
				.ColorAndOpacity(MutedTextColor)
				.AutoWrapText(true)
				.Text(this, &SWoWLoginSceneDebugPanel::GetSelectedBoneDetailsText)
			];
	};

	auto MakeBonesTab = [MakeSectionPanel, MakeBonesSection]()
	{
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				MakeSectionPanel(FText::FromString(TEXT("Bones / 010 骨骼动画采样")), MakeBonesSection())
			];
	};

	auto MakeGlueLogTab = [this, MakeSectionPanel]()
	{
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.FillHeight(0.55f)
			.Padding(0.0f, 0.0f, 0.0f, 8.0f)
			[
				MakeSectionPanel(
					FText::FromString(TEXT("Glue Lua print 日志")),
					SNew(SVerticalBox)
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 0.0f, 0.0f, 8.0f)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot()
						.FillWidth(1.0f)
						.VAlign(VAlign_Center)
						[
							SNew(STextBlock)
							.ColorAndOpacity(MutedTextColor)
							.Text(FText::FromString(TEXT("Lua 中 print(...) 的输出会同步显示到这里，最多保留最近 200 行。")))
						]
						+ SHorizontalBox::Slot()
						.AutoWidth()
						[
							SNew(SButton)
							.Text(FText::FromString(TEXT("清空日志")))
							.OnClicked(this, &SWoWLoginSceneDebugPanel::OnClearGlueDebugLogClicked)
						]
					]
					+ SVerticalBox::Slot()
					.FillHeight(1.0f)
					[
						SNew(SMultiLineEditableTextBox)
						.IsReadOnly(true)
						.AllowMultiLine(true)
						.AutoWrapText(false)
						.AlwaysShowScrollbars(true)
						.SelectAllTextWhenFocused(false)
						.ClearTextSelectionOnFocusLoss(false)
						.AllowContextMenu(true)
						.Padding(FMargin(8.0f))
						.BackgroundColor(FLinearColor(0.015f, 0.017f, 0.020f, 0.88f))
						.ForegroundColor(TextColor)
						.ReadOnlyForegroundColor(TextColor)
						.Text(this, &SWoWLoginSceneDebugPanel::GetGlueDebugLogText)
					])
			]
			+ SVerticalBox::Slot()
			.FillHeight(0.45f)
			[
				MakeSectionPanel(
					FText::FromString(TEXT("GetCharacterInfo 返回值核对")),
					SNew(SMultiLineEditableTextBox)
					.IsReadOnly(true)
					.AllowMultiLine(true)
					.AutoWrapText(false)
					.AlwaysShowScrollbars(true)
					.SelectAllTextWhenFocused(false)
					.ClearTextSelectionOnFocusLoss(false)
					.AllowContextMenu(true)
					.Padding(FMargin(8.0f))
					.BackgroundColor(FLinearColor(0.015f, 0.017f, 0.020f, 0.88f))
					.ForegroundColor(TextColor)
					.ReadOnlyForegroundColor(TextColor)
					.Text(this, &SWoWLoginSceneDebugPanel::GetGlueCharacterInfoDebugText))
			];
	};

	ChildSlot
	[
		SNew(SBorder)
		.RenderTransform(this, &SWoWLoginSceneDebugPanel::GetPanelRenderTransform)
		.Padding(1.0f)
		.BorderImage(FCoreStyle::Get().GetBrush(TEXT("WhiteBrush")))
		.BorderBackgroundColor(PanelOutlineColor)
		.OnMouseButtonDown(this, &SWoWLoginSceneDebugPanel::OnHeaderMouseButtonDown)
		[
			SNew(SBorder)
			.Padding(12.0f)
			.BorderImage(FCoreStyle::Get().GetBrush(TEXT("WhiteBrush")))
			.BorderBackgroundColor(PanelBackgroundColor)
			[
				SNew(SBox)
				.WidthOverride(960.0f)
				.MaxDesiredHeight(760.0f)
				[
					SNew(SVerticalBox)
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0.0f, 0.0f, 0.0f, 4.0f)
						[
							SNew(SBorder)
							.Padding(FMargin(6.0f, 4.0f))
							.BorderImage(FCoreStyle::Get().GetBrush(TEXT("WhiteBrush")))
							.BorderBackgroundColor(FLinearColor(0.10f, 0.11f, 0.13f, 0.72f))
							[
								SNew(SHorizontalBox)
								+ SHorizontalBox::Slot()
								.FillWidth(1.0f)
								.VAlign(VAlign_Center)
								[
									SNew(STextBlock)
									.ColorAndOpacity(HeaderColor)
									.Text(FText::FromString(TEXT("登录场景资产调试器  -  拖动面板空白处可移动")))
								]
								+ SHorizontalBox::Slot()
								.AutoWidth()
								[
									SNew(SButton)
									.Text(FText::FromString(TEXT("关闭")))
									.OnClicked(this, &SWoWLoginSceneDebugPanel::OnCloseClicked)
								]
							]
						]
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0.0f, 0.0f, 0.0f, 10.0f)
						[
							SNew(STextBlock)
							.ColorAndOpacity(TextColor)
							.AutoWrapText(true)
							.Text(this, &SWoWLoginSceneDebugPanel::GetSummaryText)
						]
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0.0f, 0.0f, 0.0f, 8.0f)
						[
							SNew(STextBlock)
							.ColorAndOpacity(MutedTextColor)
							.AutoWrapText(true)
							.Text(this, &SWoWLoginSceneDebugPanel::GetPresetBakeStatusText)
						]
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0.0f, 0.0f, 0.0f, 8.0f)
						[
							SNew(SHorizontalBox)
							+ SHorizontalBox::Slot()
							.AutoWidth()
							.VAlign(VAlign_Center)
							.Padding(0.0f, 0.0f, 8.0f, 0.0f)
							[
								SNew(STextBlock)
								.ColorAndOpacity(TextColor)
								.Text(FText::FromString(TEXT("界面选择")))
							]
							+ SHorizontalBox::Slot()
							.AutoWidth()
							.Padding(0.0f, 0.0f, 12.0f, 0.0f)
							[
								SNew(SBox)
								.WidthOverride(220.0f)
								[
									MakeLoginSceneCombo()
								]
							]
							+ SHorizontalBox::Slot()
							.AutoWidth()
							.Padding(0.0f, 0.0f, 6.0f, 0.0f)
							[
								SNew(SButton)
								.Text(FText::FromString(TEXT("复制参数")))
								.OnClicked(this, &SWoWLoginSceneDebugPanel::OnCopyParamsClicked)
							]
							+ SHorizontalBox::Slot()
							.AutoWidth()
							.Padding(0.0f, 0.0f, 6.0f, 0.0f)
							[
								SNew(SButton)
								.Text(FText::FromString(TEXT("保存资产")))
								.OnClicked(this, &SWoWLoginSceneDebugPanel::OnSaveAssetClicked)
							]
							+ SHorizontalBox::Slot()
							.AutoWidth()
							.Padding(0.0f, 0.0f, 6.0f, 0.0f)
							[
								SNew(SButton)
								.Text(FText::FromString(TEXT("烘焙场景")))
								.OnClicked(this, &SWoWLoginSceneDebugPanel::OnBakeScenePresetClicked)
							]
							+ SHorizontalBox::Slot()
							.AutoWidth()
							[
								SNew(SButton)
								.Text(FText::FromString(TEXT("批量烘焙")))
								.OnClicked(this, &SWoWLoginSceneDebugPanel::OnBakeAllScenePresetsClicked)
							]
						]
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0.0f, 0.0f, 0.0f, 8.0f)
						[
							SNew(SHorizontalBox)
							+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 6.0f, 0.0f)
							[
								MakeTabButton(FText::FromString(TEXT("Scene")), EDebugTab::Scene)
							]
							+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 6.0f, 0.0f)
							[
								MakeTabButton(FText::FromString(TEXT("Camera")), EDebugTab::Camera)
							]
							+ SHorizontalBox::Slot().AutoWidth()
							[
								MakeTabButton(FText::FromString(TEXT("Lighting")), EDebugTab::Lighting)
							]
							+ SHorizontalBox::Slot().AutoWidth().Padding(6.0f, 0.0f, 0.0f, 0.0f)
							[
								MakeTabButton(FText::FromString(TEXT("Particles")), EDebugTab::Particles)
							]
							+ SHorizontalBox::Slot().AutoWidth().Padding(6.0f, 0.0f, 0.0f, 0.0f)
							[
								MakeTabButton(FText::FromString(TEXT("Textures")), EDebugTab::Textures)
							]
							+ SHorizontalBox::Slot().AutoWidth().Padding(6.0f, 0.0f, 0.0f, 0.0f)
							[
								MakeTabButton(FText::FromString(TEXT("Bones")), EDebugTab::Bones)
							]
							+ SHorizontalBox::Slot().AutoWidth().Padding(6.0f, 0.0f, 0.0f, 0.0f)
							[
								MakeTabButton(FText::FromString(TEXT("Character")), EDebugTab::Character)
							]
							+ SHorizontalBox::Slot().AutoWidth().Padding(6.0f, 0.0f, 0.0f, 0.0f)
							[
								MakeTabButton(FText::FromString(TEXT("Glue日志")), EDebugTab::GlueLog)
							]
						]
						+ SVerticalBox::Slot()
						.FillHeight(1.0f)
						[
							SNew(SWidgetSwitcher)
							.WidgetIndex(this, &SWoWLoginSceneDebugPanel::GetActiveTabIndex)
							+ SWidgetSwitcher::Slot()
							[
								MakeTabScroll(MakeSceneTab())
							]
							+ SWidgetSwitcher::Slot()
							[
								MakeTabScroll(MakeCameraTab())
							]
							+ SWidgetSwitcher::Slot()
							[
								MakeTabScroll(MakeLightingTab())
							]
							+ SWidgetSwitcher::Slot()
							[
								MakeTabScroll(MakeParticlesTab())
							]
							+ SWidgetSwitcher::Slot()
							[
								MakeTabScroll(MakeTexturesTab())
							]
							+ SWidgetSwitcher::Slot()
							[
								MakeTabScroll(MakeBonesTab())
							]
							+ SWidgetSwitcher::Slot()
							[
								MakeTabScroll(MakeCharacterPreviewSection())
							]
							+ SWidgetSwitcher::Slot()
							[
								MakeGlueLogTab()
							]
						]
				]
			]
		]
	];
}

bool SWoWLoginSceneDebugPanel::SupportsKeyboardFocus() const
{
	return true;
}

FReply SWoWLoginSceneDebugPanel::OnPreviewKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	const FKey Key = InKeyEvent.GetKey();
	if (ActiveKeyboardEnumField == EKeyboardEnumField::CharacterAnimation && Key == EKeys::Up)
	{
		return StepKeyboardEnumField(-1) ? FReply::Handled() : SCompoundWidget::OnPreviewKeyDown(MyGeometry, InKeyEvent);
	}
	if (ActiveKeyboardEnumField == EKeyboardEnumField::CharacterAnimation && Key == EKeys::Down)
	{
		return StepKeyboardEnumField(1) ? FReply::Handled() : SCompoundWidget::OnPreviewKeyDown(MyGeometry, InKeyEvent);
	}

	return SCompoundWidget::OnPreviewKeyDown(MyGeometry, InKeyEvent);
}

FReply SWoWLoginSceneDebugPanel::OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	const FKey Key = InKeyEvent.GetKey();
	if (Key == EKeys::Up)
	{
		return StepKeyboardEnumField(-1) ? FReply::Handled() : SCompoundWidget::OnKeyDown(MyGeometry, InKeyEvent);
	}
	if (Key == EKeys::Down)
	{
		return StepKeyboardEnumField(1) ? FReply::Handled() : SCompoundWidget::OnKeyDown(MyGeometry, InKeyEvent);
	}

	return SCompoundWidget::OnKeyDown(MyGeometry, InKeyEvent);
}

FReply SWoWLoginSceneDebugPanel::OnHeaderMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if (MouseEvent.GetEffectingButton() != EKeys::LeftMouseButton)
	{
		return FReply::Unhandled();
	}

	bDraggingPanel = true;
	PanelDragStartScreen = MouseEvent.GetScreenSpacePosition();
	PanelDragStartOffset = PanelDragOffset;
	return FReply::Handled().CaptureMouse(AsShared());
}

FReply SWoWLoginSceneDebugPanel::OnMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if (bDraggingPanel && MouseEvent.GetEffectingButton() == EKeys::LeftMouseButton)
	{
		bDraggingPanel = false;
		return FReply::Handled().ReleaseMouseCapture();
	}

	return SCompoundWidget::OnMouseButtonUp(MyGeometry, MouseEvent);
}

FReply SWoWLoginSceneDebugPanel::OnMouseMove(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if (bDraggingPanel)
	{
		PanelDragOffset = PanelDragStartOffset + MouseEvent.GetScreenSpacePosition() - PanelDragStartScreen;
		return FReply::Handled();
	}

	return SCompoundWidget::OnMouseMove(MyGeometry, MouseEvent);
}

TOptional<FSlateRenderTransform> SWoWLoginSceneDebugPanel::GetPanelRenderTransform() const
{
	return FSlateRenderTransform(PanelDragOffset);
}

FText SWoWLoginSceneDebugPanel::GetSummaryText() const
{
	if (const AWoWLoginGameMode* GameMode = LoginGameMode.Get())
	{
		const FWoWLoginSceneTuningState State = GameMode->GetLoginSceneTuningState();
		const FWoWLoginSceneM2CameraState Camera = GameMode->GetLoginSceneM2CameraState();
		return FText::FromString(FString::Printf(
			TEXT("Scene %s | Camera Id=%d FOV(raw)=%.6f Projection=%.3f deg | Pos=%s Target=%s"),
			*State.SceneLocationOffset.ToString(),
			Camera.Id,
			Camera.FovRadians,
			Camera.FovRadians * 34.5f,
			*Camera.Pos.ToString(),
			*Camera.Target.ToString()));
	}
	return FText::FromString(TEXT("No login game mode"));
}

FText SWoWLoginSceneDebugPanel::GetPresetBakeStatusText() const
{
	return PresetBakeStatusText.IsEmpty()
		? FText::FromString(TEXT("Preset尚未烘焙"))
		: FText::FromString(PresetBakeStatusText);
}

FText SWoWLoginSceneDebugPanel::GetGlueDebugLogText() const
{
	if (const AWoWLoginGameMode* GameMode = LoginGameMode.Get())
	{
		return FText::FromString(GameMode->GetGlueDebugLogText());
	}
	return FText::FromString(TEXT("No login game mode"));
}

FText SWoWLoginSceneDebugPanel::GetGlueCharacterInfoDebugText() const
{
	if (const AWoWLoginGameMode* GameMode = LoginGameMode.Get())
	{
		return FText::FromString(GameMode->GetGlueCharacterInfoDebugText());
	}
	return FText::FromString(TEXT("No login game mode"));
}

FText SWoWLoginSceneDebugPanel::GetCharacterPreviewAnimationOverrideText() const
{
	if (const AWoWLoginGameMode* GameMode = LoginGameMode.Get())
	{
		return FText::FromString(GameMode->GetLoginSceneTuningState().CharacterPreviewAnimationOverridePath);
	}
	return FText::GetEmpty();
}

FText SWoWLoginSceneDebugPanel::GetCharacterPreviewAnimationComboText() const
{
	if (const AWoWLoginGameMode* GameMode = LoginGameMode.Get())
	{
		const FWoWLoginSceneTuningState Tuning = GameMode->GetLoginSceneTuningState();
		const AWoWLoginGameMode::FCharacterSelectPreviewDebugState State = GameMode->GetCharacterSelectPreviewDebugState();
		const FString Prefix = Tuning.CharacterPreviewAnimationOverrideId == INDEX_NONE ? TEXT("自动: ") : FString::Printf(TEXT("覆盖[%d]: "), Tuning.CharacterPreviewAnimationOverrideId);
		return FText::FromString(Prefix + AnimationObjectPathLabel(State.ActiveAnimationPath));
	}
	return FText::FromString(TEXT("No GameMode"));
}

FText SWoWLoginSceneDebugPanel::GetCharacterPreviewDebugText() const
{
	if (const AWoWLoginGameMode* GameMode = LoginGameMode.Get())
	{
		const AWoWLoginGameMode::FCharacterSelectPreviewDebugState State = GameMode->GetCharacterSelectPreviewDebugState();
		const TArray<FString> AnimationPaths = GameMode->GetCharacterPreviewAvailableAnimationPaths();
		const TArray<AWoWLoginGameMode::FCharacterPreviewAnimationOption> LogicalOptions = GameMode->GetCharacterPreviewLogicalAnimationOptions();
		const AWoWLoginGameMode::FCharacterPreviewAnimationCatalogState CatalogState = GameMode->GetCharacterPreviewAnimationCatalogState();
		FString AnimationOptionSummary;
		int32 AvailableLogicalAnimationCount = 0;
		constexpr int32 MaxDebugAnimationNames = 32;
		for (const AWoWLoginGameMode::FCharacterPreviewAnimationOption& Option : LogicalOptions)
		{
			if (!Option.bAvailable)
			{
				continue;
			}

			++AvailableLogicalAnimationCount;
			if (AvailableLogicalAnimationCount > MaxDebugAnimationNames)
			{
				continue;
			}
			if (!AnimationOptionSummary.IsEmpty())
			{
				AnimationOptionSummary += TEXT(", ");
			}
			AnimationOptionSummary += FString::Printf(TEXT("%s(%d)"), *Option.Name, Option.AnimationId);
		}
		if (AnimationOptionSummary.IsEmpty())
		{
			AnimationOptionSummary = TEXT("<none>");
		}
		else if (AvailableLogicalAnimationCount > MaxDebugAnimationNames)
		{
			AnimationOptionSummary += FString::Printf(TEXT(", ... +%d"), AvailableLogicalAnimationCount - MaxDebugAnimationNames);
		}

		return FText::FromString(FString::Printf(
			TEXT("HasPreview=%d Guid=%d Race=%d Gender=%d Id=%s\nAppearance: Skin=%d Face=%d HairStyle=%d HairColor=%d Facial=%d EquipmentEntries=%d Descriptor=%d SignatureAsset=%d\nBuild: status=%s reason=%s log=%s\nDescriptorPath=%s\nSignatureMesh=%s\nSignatureStand=%s\nSignatureSections=%s\nAppearanceKey=%s\nEquipment=%s\nMesh=%s\nStand=%s\nActiveAnim=%s\nAnimations: paths=%d logicalAvailable=%d logical=%s\nCatalog: ready=%d warming=%d candidates=%d imported=%d resolved=%d remaining=%d\nBase: Loc=%s Rot=%s Scale=%.4f\nFinal: Loc=%s Rot=%s Scale=%s"),
			State.bHasPreviewActor ? 1 : 0,
			State.SelectedGuid,
			State.Race,
			State.Gender,
			*State.DefinitionId,
			State.Skin,
			State.Face,
			State.HairStyle,
			State.HairColor,
			State.FacialStyle,
			State.EquipmentEntryCount,
			State.bHasAppearanceDescriptor ? 1 : 0,
			State.bHasCharacterPreviewAsset ? 1 : 0,
			*State.CharacterPreviewBuildStatus,
			*State.CharacterPreviewBuildReason,
			*State.CharacterPreviewBuildLogPath,
			*State.AppearanceDescriptorPath,
			*State.CharacterPreviewMeshPath,
			*State.CharacterPreviewAnimationPath,
			*State.CharacterPreviewSectionMetadataPath,
			*State.AppearanceCacheKey,
			*State.EquipmentSummary,
			*State.MeshPath,
			*State.StandAnimationPath,
			*State.ActiveAnimationPath,
			AnimationPaths.Num(),
			AvailableLogicalAnimationCount,
			*AnimationOptionSummary,
			CatalogState.bReady ? 1 : 0,
			CatalogState.bWarming ? 1 : 0,
			CatalogState.CandidatePathCount,
			CatalogState.ImportedAnimationIdCount,
			CatalogState.ResolvedOptionCount,
			CatalogState.RemainingAnimationIdCount,
			*State.BaseLocation.ToString(),
			*State.BaseRotation.ToString(),
			State.BaseScale,
			*State.FinalLocation.ToString(),
			*State.FinalRotation.ToString(),
			*State.FinalScale.ToString()));
	}
	return FText::FromString(TEXT("No login game mode"));
}

FText SWoWLoginSceneDebugPanel::GetSceneRawDetailsText() const
{
	if (!bShowRawDetails)
	{
		return FText::FromString(TEXT("原始导出文本已收起。点击“显示原始字段”后可复制完整 Scene/Camera 参数。"));
	}
	return FText::FromString(TEXT("完整导出参数如下，内容来自当前 UE 调参资产和 M2 相机解析结果。"));
}

FText SWoWLoginSceneDebugPanel::GetCameraRawDetailsText() const
{
	if (!bShowRawDetails)
	{
		return FText::FromString(TEXT("010 Cameras 结构顺序已收起。需要逐项对照 010 模板时点击“显示原始字段”。"));
	}

	return FText::FromString(TEXT("010 struct Cameras order: enum E_CAMERATYPE Id, float FOV, float FarClipping, float NearClipping, struct ABlock_BF TranslationPos, struct C3Vector Pos, struct ABlock_BF TranslationTar, struct C3Vector Target, struct ABlock_F RollEffect."));
}

void SWoWLoginSceneDebugPanel::RebuildCharacterAnimationMenuContent()
{
	if (!CharacterAnimationMenuContentBox.IsValid())
	{
		return;
	}
	CharacterAnimationRowWidgets.Empty();
	CharacterAnimationMenuContentBox->ClearChildren();

	CharacterAnimationMenuContentBox->AddSlot()
	.AutoHeight()
	.Padding(0.0f, 0.0f, 0.0f, 6.0f)
	[
		SNew(STextBlock)
		.ColorAndOpacity(TextColor)
		.Text(FText::FromString(TEXT("选择预览动作")))
	];

	CharacterAnimationMenuContentBox->AddSlot()
	.AutoHeight()
	.Padding(0.0f, 0.0f, 0.0f, 6.0f)
	[
		SNew(SSearchBox)
		.HintText(FText::FromString(TEXT("按动作名、ID 或资产名过滤")))
		.OnTextChanged_Lambda([this](const FText& NewText)
		{
			CharacterAnimationFilterText = NewText.ToString().TrimStartAndEnd();
			RebuildCharacterAnimationMenuContent();
		})
	];

	CharacterAnimationMenuContentBox->AddSlot()
	.AutoHeight()
	.Padding(0.0f, 1.0f)
	[
		SNew(SBorder)
		.BorderImage(FCoreStyle::Get().GetBrush(TEXT("WhiteBrush")))
		.BorderBackgroundColor_Lambda([this]()
		{
			if (const AWoWLoginGameMode* GameMode = LoginGameMode.Get())
			{
				return GameMode->GetLoginSceneTuningState().CharacterPreviewAnimationOverrideId == INDEX_NONE
					? SelectedRowBackgroundColor
					: RowBackgroundColor;
			}
			return RowBackgroundColor;
		})
		.Padding(FMargin(4.0f, 2.0f))
		[
			SNew(SButton)
			.ButtonColorAndOpacity(FLinearColor::Transparent)
			.ContentPadding(FMargin(6.0f, 3.0f))
			.ToolTipText(FText::FromString(TEXT("清空动作覆盖，使用当前角色自动选择的 Stand/默认动作。")))
			.OnClicked_Lambda([this]()
			{
				FocusKeyboardEnumField(EKeyboardEnumField::CharacterAnimation);
				FReply Reply = OnCharacterPreviewAnimationSelected(FString());
				ScrollCharacterAnimationSelectionIntoView();
				return Reply;
			})
			[
				SNew(STextBlock)
				.ColorAndOpacity(TextColor)
				.Text(FText::FromString(TEXT("使用当前角色自动 Stand")))
			]
		]
	];

	if (const AWoWLoginGameMode* GameMode = LoginGameMode.Get())
	{
		const TArray<AWoWLoginGameMode::FCharacterPreviewAnimationOption> LogicalOptions = GameMode->GetCharacterPreviewLogicalAnimationOptions();
		const AWoWLoginGameMode::FCharacterPreviewAnimationCatalogState CatalogState = GameMode->GetCharacterPreviewAnimationCatalogState();

		if (!LogicalOptions.IsEmpty())
		{
			CharacterAnimationMenuContentBox->AddSlot()
			.AutoHeight()
			.Padding(0.0f, 8.0f, 0.0f, 2.0f)
			[
				SNew(STextBlock)
				.ColorAndOpacity(MutedTextColor)
				.Text(FText::FromString(FString::Printf(TEXT("逻辑动作枚举 (%d 已就绪%s)"), LogicalOptions.Num(), CatalogState.bWarming ? TEXT("，持续补全中") : TEXT(""))))
			];
		}
		else if (CatalogState.bWarming)
		{
			CharacterAnimationMenuContentBox->AddSlot()
			.AutoHeight()
			.Padding(0.0f, 8.0f, 0.0f, 2.0f)
			[
				SNew(STextBlock)
				.ColorAndOpacity(MutedTextColor)
				.Text(FText::FromString(TEXT("当前角色动作菜单预热中…")))
			];
		}

		for (const AWoWLoginGameMode::FCharacterPreviewAnimationOption& Option : LogicalOptions)
		{
			const FString Label = FString::Printf(TEXT("%s (%d)%s"), *Option.Name, Option.AnimationId, Option.bAvailable ? TEXT("") : TEXT(" - 未导入"));
			TSharedPtr<SBorder> RowBorder;
			CharacterAnimationMenuContentBox->AddSlot()
			.AutoHeight()
			.Padding(0.0f, 1.0f)
			[
				SAssignNew(RowBorder, SBorder)
				.Visibility_Lambda([this, Label, ObjectPath = Option.ObjectPath]()
				{
					const FString Filter = CharacterAnimationFilterText.TrimStartAndEnd();
					if (Filter.IsEmpty())
					{
						return EVisibility::Visible;
					}
					return Label.Contains(Filter, ESearchCase::IgnoreCase) || ObjectPath.Contains(Filter, ESearchCase::IgnoreCase) ? EVisibility::Visible : EVisibility::Collapsed;
				})
				.BorderImage(FCoreStyle::Get().GetBrush(TEXT("WhiteBrush")))
				.BorderBackgroundColor_Lambda([this, AnimationId = Option.AnimationId]()
				{
					return GetCurrentCharacterPreviewAnimationId() == AnimationId ? SelectedRowBackgroundColor : RowBackgroundColor;
				})
				.Padding(FMargin(4.0f, 2.0f))
				[
					SNew(SButton)
					.IsEnabled(Option.bAvailable)
					.ButtonColorAndOpacity(FLinearColor::Transparent)
					.ContentPadding(FMargin(6.0f, 3.0f))
					.ToolTipText(FText::FromString(Option.bAvailable ? Option.ObjectPath : TEXT("当前角色预览缓存里还没有这个 animId 的 UAnimSequence")))
					.OnClicked_Lambda([this, AnimationId = Option.AnimationId]()
					{
						FocusKeyboardEnumField(EKeyboardEnumField::CharacterAnimation);
						FReply Reply = OnCharacterPreviewLogicalAnimationSelected(AnimationId);
						ScrollCharacterAnimationSelectionIntoView();
						return Reply;
					})
					[
						SNew(STextBlock)
						.ColorAndOpacity(Option.bAvailable ? TextColor : MutedTextColor)
						.Text(FText::FromString(Label))
					]
				]
			];
			CharacterAnimationRowWidgets.Add(Option.AnimationId, RowBorder);
		}
	}
}

FText SWoWLoginSceneDebugPanel::GetMappingHelpText() const
{
	if (const AWoWLoginGameMode* GameMode = LoginGameMode.Get())
	{
		const FWoWLoginSceneM2CameraState Base = GameMode->GetLoginSceneBaseM2CameraState();
		const FWoWLoginSceneM2CameraState Current = GameMode->GetLoginSceneM2CameraState();
		return FText::FromString(FString::Printf(
			TEXT("Id: %d\nFOV raw: base %.6f / current %.6f\nProjectionDeg: base %.6f / current %.6f\nFarClipping: base %.6f / current %.6f\nNearClipping: base %.6f / current %.6f\nPos: base %s / current %s\nTarget: base %s / current %s\nRollEffect.x: base %.6f / current %.6f"),
			Current.Id,
			Base.FovRadians,
			Current.FovRadians,
			Base.FovRadians * 34.5f,
			Current.FovRadians * 34.5f,
			Base.FarClipping,
			Current.FarClipping,
			Base.NearClipping,
			Current.NearClipping,
			*Base.Pos.ToString(),
			*Current.Pos.ToString(),
			*Base.Target.ToString(),
			*Current.Target.ToString(),
			Base.RollRadians,
			Current.RollRadians));
	}
	return FText::FromString(TEXT(""));
}

FText SWoWLoginSceneDebugPanel::GetCameraAnimBlockSummaryText() const
{
	if (!bShowRawDetails)
	{
		return FText::FromString(TEXT("ABlock 动画摘要已收起。需要查看 position/target/roll 样本数量和时长时点击“显示原始字段”。"));
	}

	const AWoWLoginGameMode* GameMode = LoginGameMode.Get();
	if (!GameMode)
	{
		return FText::FromString(TEXT("No login game mode"));
	}

	const FWoWLoginSceneM2CameraState Base = GameMode->GetLoginSceneBaseM2CameraState();
	const FWoWLoginSceneM2CameraState Current = GameMode->GetLoginSceneM2CameraState();
	return FText::FromString(FString::Printf(
		TEXT("struct ABlock_BF TranslationPos: samples=%d duration=%.1fms current Pos=%s base Pos=%s\n")
		TEXT("struct ABlock_BF TranslationTar: samples=%d duration=%.1fms current Target=%s base Target=%s\n")
		TEXT("struct ABlock_F RollEffect: samples=%d duration=%.1fms current val.x(rad)=%.6f base val.x(rad)=%.6f\n")
		TEXT("说明: TranslationPos/TranslationTar/RollEffect 是 M2 动画块。当前面板不编辑二进制 offset/count，而是显示已解析结果；实际调节请改 Pos/Target/RollEffect.val.x，保存到UE调参资产。"),
		GameMode->GetLoginCameraPositionSampleCount(),
		GameMode->GetLoginCameraTrackDurationMs(),
		*Current.Pos.ToString(),
		*Base.Pos.ToString(),
		GameMode->GetLoginCameraTargetSampleCount(),
		GameMode->GetLoginCameraTrackDurationMs(),
		*Current.Target.ToString(),
		*Base.Target.ToString(),
		GameMode->GetLoginCameraRollSampleCount(),
		GameMode->GetLoginCameraTrackDurationMs(),
		Current.RollRadians,
		Base.RollRadians));
}

FText SWoWLoginSceneDebugPanel::GetLightingSummaryText() const
{
	const AWoWLoginGameMode* GameMode = LoginGameMode.Get();
	if (!GameMode)
	{
		return FText::FromString(TEXT("No login game mode"));
	}

	const FWoWLoginSceneTuningState State = GameMode->GetLoginSceneTuningState();
	return FText::FromString(FString::Printf(
		TEXT("GlobalBrightnessScale=%.3f；M2物理灯桥接=%s。关闭桥接时走正式 WoW/M2 材质光照路径：M2 Lights[] 的 ambient/diffuse/color/attenuation 会折算到材质 TintColor 与 Brightness，不依赖 UE 物理灯。开启桥接时额外生成 UE PointLight，仅用于诊断对照。"),
		State.GlobalBrightnessScale,
		State.bUseM2PhysicalLightBridge ? TEXT("On") : TEXT("Off")));
}

FText SWoWLoginSceneDebugPanel::GetM2LightsDetailsText() const
{
	if (!bShowRawDetails)
	{
		return FText::FromString(TEXT("原始 M2 Lights[] 字段已收起。需要对照 010 时点击“显示原始字段”。"));
	}

	const AWoWLoginGameMode* GameMode = LoginGameMode.Get();
	if (!GameMode)
	{
		return FText::FromString(TEXT(""));
	}

	const TArray<AWoWLoginGameMode::FLoginSceneLight>& Lights = GameMode->GetLoginSceneLights();
	if (Lights.IsEmpty())
	{
		return FText::FromString(TEXT("M2 Lights[]: 0"));
	}

	FString Result = FString::Printf(TEXT("M2 Lights[]: %d\n"), Lights.Num());
	for (const AWoWLoginGameMode::FLoginSceneLight& Light : Lights)
	{
		Result += FString::Printf(
			TEXT("[%d] type=%d bone=%d pos=%s ambient=%s ambientIntensity=%.3f diffuse=%s diffuseIntensity=%.3f atten=%.3f..%.3f\n"),
			Light.Index,
			Light.LightType,
			Light.BoneIndex,
			*Light.Position.ToString(),
			*Light.AmbientColor.ToString(),
			Light.AmbientIntensity,
			*Light.DiffuseColor.ToString(),
			Light.DiffuseIntensity,
			Light.AttenuationStart,
			Light.AttenuationEnd);
	}
	return FText::FromString(Result);
}

FText SWoWLoginSceneDebugPanel::GetSelectedLightText() const
{
	const AWoWLoginGameMode* GameMode = LoginGameMode.Get();
	if (!GameMode)
	{
		return FText::FromString(TEXT("No login game mode"));
	}
	const TArray<AWoWLoginGameMode::FLoginSceneLight>& Lights = GameMode->GetLoginSceneLights();
	if (Lights.IsEmpty())
	{
		return FText::FromString(TEXT("M2 Lights[]: 0"));
	}
	const int32 CurrentIndex = SelectedLightIndex == INDEX_NONE ? Lights[0].Index : SelectedLightIndex;
	const AWoWLoginGameMode::FLoginSceneLight* Light = Lights.FindByPredicate([CurrentIndex](const AWoWLoginGameMode::FLoginSceneLight& Candidate)
	{
		return Candidate.Index == CurrentIndex;
	});
	if (!Light)
	{
		Light = &Lights[0];
	}
	const FWoWLoginLightTuning Tuning = GameMode->GetLoginSceneLightTuning(Light->Index);
	const FWoWLoginSceneTuningState State = GameMode->GetLoginSceneTuningState();
	return FText::FromString(FString::Printf(
		TEXT("当前 Light[%d]  备注=%s  bridge=%s  enabled=%s  type=%d  bone=%d  diffuse=%.3f*%.3f colorScale=(%.2f, %.2f, %.2f)  atten=%.3f..%.3f*%.3f"),
		Light->Index,
		Tuning.Note.IsEmpty() ? TEXT("-") : *Tuning.Note,
		State.bUseM2PhysicalLightBridge ? TEXT("On") : TEXT("Off"),
		Tuning.bEnabled ? TEXT("On") : TEXT("Off"),
		Light->LightType,
		Light->BoneIndex,
		Light->DiffuseIntensity,
		Tuning.DiffuseIntensityScale,
		Tuning.DiffuseColorScale.R,
		Tuning.DiffuseColorScale.G,
		Tuning.DiffuseColorScale.B,
		Light->AttenuationStart,
		Light->AttenuationEnd,
		Tuning.AttenuationRadiusScale));
}

FText SWoWLoginSceneDebugPanel::GetSelectedNoteText() const
{
	const AWoWLoginGameMode* GameMode = LoginGameMode.Get();
	if (!GameMode)
	{
		return FText::GetEmpty();
	}

	switch (ActiveTab)
	{
	case EDebugTab::Lighting:
	{
		const TArray<AWoWLoginGameMode::FLoginSceneLight>& Lights = GameMode->GetLoginSceneLights();
		const int32 LightIndex = SelectedLightIndex == INDEX_NONE && !Lights.IsEmpty() ? Lights[0].Index : SelectedLightIndex;
		return FText::FromString(GameMode->GetLoginSceneLightTuning(LightIndex).Note);
	}
	case EDebugTab::Particles:
	{
		const TArray<int32> Indices = GameMode->GetLoginParticleEmitterIndices();
		const int32 EmitterIndex = SelectedParticleEmitterIndex == INDEX_NONE && !Indices.IsEmpty() ? Indices[0] : SelectedParticleEmitterIndex;
		return FText::FromString(GameMode->GetLoginParticleEmitterTuning(EmitterIndex).Note);
	}
	case EDebugTab::Textures:
	{
		const TArray<int32> Indices = GetActiveTextureSectionIndices();
		const int32 SectionIndex = SelectedTextureSectionIndex == INDEX_NONE && !Indices.IsEmpty() ? Indices[0] : SelectedTextureSectionIndex;
		return FText::FromString(GameMode->GetLoginTextureSectionTuning(SectionIndex).Note);
	}
	default:
		return FText::GetEmpty();
	}
}

FText SWoWLoginSceneDebugPanel::GetLoginSceneComboText() const
{
	const AWoWLoginGameMode* GameMode = LoginGameMode.Get();
	if (!GameMode)
	{
		return FText::FromString(TEXT("无登录界面"));
	}

	const TArray<AWoWLoginGameMode::FLoginSceneDefinition>& Definitions = GameMode->GetLoginSceneDefinitions();
	const int32 ActiveIndex = GameMode->GetActiveLoginSceneIndex();
	if (!Definitions.IsValidIndex(ActiveIndex))
	{
		return FText::FromString(TEXT("无登录界面"));
	}

	return FText::FromString(FString::Printf(TEXT("%d %s"), ActiveIndex + 1, *Definitions[ActiveIndex].DisplayName));
}

FText SWoWLoginSceneDebugPanel::GetExportText() const
{
	if (const AWoWLoginGameMode* GameMode = LoginGameMode.Get())
	{
		return FText::FromString(
			GameMode->ExportLoginSceneBaseCameraState() + TEXT(" | ") +
			GameMode->ExportLoginSceneTuningState() + TEXT(" | ") +
			GameMode->ExportLoginSceneFinalCameraState());
	}
	return FText::FromString(TEXT(""));
}

FText SWoWLoginSceneDebugPanel::GetSelectedParticleText() const
{
	const AWoWLoginGameMode* GameMode = LoginGameMode.Get();
	if (!GameMode)
	{
		return FText::FromString(TEXT("No login game mode"));
	}

	const TArray<int32> Indices = GameMode->GetLoginParticleEmitterIndices();
	if (Indices.IsEmpty())
	{
		return FText::FromString(TEXT("没有粒子发射器"));
	}

	const int32 EmitterIndex = SelectedParticleEmitterIndex == INDEX_NONE ? Indices[0] : SelectedParticleEmitterIndex;
	const FWoWLoginParticleEmitterTuning Tuning = GameMode->GetLoginParticleEmitterTuning(EmitterIndex);
	return FText::FromString(FString::Printf(
		TEXT("当前 Emitter[%d]  备注=%s  %s  live=%d"),
		EmitterIndex,
		Tuning.Note.IsEmpty() ? TEXT("-") : *Tuning.Note,
		Tuning.bEnabled ? TEXT("Enabled") : TEXT("Disabled"),
		GameMode->GetLoginParticleCount(EmitterIndex)));
}

FText SWoWLoginSceneDebugPanel::GetSelectedParticleDetailsText() const
{
	const AWoWLoginGameMode* GameMode = LoginGameMode.Get();
	if (!GameMode)
	{
		return FText::FromString(TEXT(""));
	}

	const TArray<int32> Indices = GameMode->GetLoginParticleEmitterIndices();
	if (Indices.IsEmpty())
	{
		return FText::FromString(TEXT("M2 ParticleEmitters[] empty."));
	}

	const int32 EmitterIndex = SelectedParticleEmitterIndex == INDEX_NONE ? Indices[0] : SelectedParticleEmitterIndex;
	const AWoWLoginGameMode::FLoginParticleEmitter* Emitter = GameMode->GetLoginParticleEmitterByIndex(EmitterIndex);
	if (!Emitter)
	{
		return FText::FromString(TEXT("请选择粒子条目。"));
	}

	const FWoWLoginParticleEmitterTuning Tuning = GameMode->GetLoginParticleEmitterTuning(EmitterIndex);
	const FString TextureName = FPaths::GetCleanFilename(Emitter->PreviewPng);
	const float TimeSeconds = GameMode->GetWorld() ? GameMode->GetWorld()->GetTimeSeconds() : 0.0f;
	const float EmissionSpeedValue = SampleParticleTrackForDebug(Emitter->EmissionSpeedSamples, TimeSeconds, 0.0f);
	const float VerticalRangeValue = SampleParticleTrackForDebug(Emitter->VerticalRangeSamples, TimeSeconds, 0.0f);
	const float HorizontalRangeValue = SampleParticleTrackForDebug(Emitter->HorizontalRangeSamples, TimeSeconds, 0.0f);
	const float GravityValue = SampleParticleTrackForDebug(Emitter->GravitySamples, TimeSeconds, 0.0f);
	const float Gravity2Value = SampleParticleTrackForDebug(Emitter->Gravity2Samples, TimeSeconds, 0.0f);
	const float LifespanValue = SampleParticleTrackForDebug(Emitter->LifespanSamples, TimeSeconds, 0.0f);
	const float EmissionRateValue = SampleParticleTrackForDebug(Emitter->EmissionRateSamples, TimeSeconds, 0.0f);
	const float AreaLengthValue = SampleParticleTrackForDebug(Emitter->EmissionAreaLengthSamples, TimeSeconds, 0.0f);
	const float AreaWidthValue = SampleParticleTrackForDebug(Emitter->EmissionAreaWidthSamples, TimeSeconds, 0.0f);
	const float ZSourceValue = SampleParticleTrackForDebug(Emitter->ZSourceSamples, TimeSeconds, 0.0f);
	const AWoWLoginGameMode::FLoginBoneDebugSummary BoneSummary = GameMode->GetLoginBoneDebugSummary(Emitter->BoneIndex);
	const FVector BoneTravel = BoneSummary.MaxTranslation - BoneSummary.MinTranslation;
	if (!bShowRawDetails)
	{
		return FText::FromString(FString::Printf(
			TEXT("Emitter %d  texture=%s  blend=%d  bone=%d  boneAnimated=%s  boneRange=%s  type=%d  live=%d  note=%s"),
			Emitter->Index,
			*TextureName,
			Emitter->Blend,
			Emitter->BoneIndex,
			BoneSummary.bHasAnimation ? TEXT("Yes") : TEXT("No"),
			*BoneTravel.ToString(),
			Emitter->EmitterType,
			GameMode->GetLoginParticleCount(EmitterIndex),
			Tuning.Note.IsEmpty() ? TEXT("-") : *Tuning.Note));
	}
	return FText::FromString(FString::Printf(
		TEXT("010/WLK字段: id=%d flags=%d(0x%X) bone=%d pos=%s texture=%s blend=%d Emitter_type=%d Head_or_Tail=%d Texturerot=%d rows=%d cols=%d\nFlags解码: ModelSpace_0x80=%d GroundClamp_0x2000=%d Outwards_0x20000=%d Inwards_0x40000=%d NoTrail_0x10=%d UnLit_0x20=%d\nABlock当前值: emissionspeed=%.4f Speed_var(trackCount=%d) Vertical_range=%.4f Horizontal_range=%.4f Gravity=%.4f Gravity2=%.4f Lifespan=%.4f Emissionrate=%.4f Emissionarea_length=%.4f Emissionarea_width=%.4f zSource=%.4f\n生命周期键: size=[%.6f, %.6f, %.6f] alpha=[%.6f, %.6f, %.6f]\n当前覆盖: posOffset=%s rateScale=%.2f speedScale=%.2f lifeScale=%.2f areaLenScale=%.2f areaWidthScale=%.2f verticalScale=%.2f horizontalScale=%.2f gravityScale=%.2f gravity2Scale=%.2f sizeScale=%.2f sizeKeyScale=[%.2f, %.2f, %.2f] alphaScale=%.2f alphaKeyScale=[%.2f, %.2f, %.2f] alphaCutoff=%.3f brightnessScale=%.2f"),
		Emitter->Index,
		Emitter->Flags,
		Emitter->Flags,
		Emitter->BoneIndex,
		*Emitter->Position.ToString(),
		*TextureName,
		Emitter->Blend,
		Emitter->EmitterType,
		Emitter->HeadOrTail,
		Emitter->TextureTileRotation,
		Emitter->TextureRows,
		Emitter->TextureColumns,
		(Emitter->Flags & 0x80) != 0 ? 1 : 0,
		(Emitter->Flags & 0x2000) != 0 ? 1 : 0,
		(Emitter->Flags & 0x20000) != 0 ? 1 : 0,
		(Emitter->Flags & 0x40000) != 0 ? 1 : 0,
		(Emitter->Flags & 0x10) != 0 ? 1 : 0,
		(Emitter->Flags & 0x20) != 0 ? 1 : 0,
		EmissionSpeedValue,
		Emitter->SpeedVariationSamples.Num(),
		VerticalRangeValue,
		HorizontalRangeValue,
		GravityValue,
		Gravity2Value,
		LifespanValue,
		EmissionRateValue,
		AreaLengthValue,
		AreaWidthValue,
		ZSourceValue,
		Emitter->LifecycleSizes[0],
		Emitter->LifecycleSizes[1],
		Emitter->LifecycleSizes[2],
		Emitter->LifecycleColors[0].A,
		Emitter->LifecycleColors[1].A,
		Emitter->LifecycleColors[2].A,
		*Tuning.PositionOffset.ToString(),
		Tuning.EmissionRateScale,
		Tuning.EmissionSpeedScale,
		Tuning.LifespanScale,
		Tuning.AreaLengthScale,
		Tuning.AreaWidthScale,
		Tuning.VerticalRangeScale,
		Tuning.HorizontalRangeScale,
		Tuning.GravityScale,
		Tuning.Gravity2Scale,
		Tuning.SizeScale,
		Tuning.LifecycleSizeStartScale,
		Tuning.LifecycleSizeMidScale,
		Tuning.LifecycleSizeEndScale,
		Tuning.AlphaScale,
		Tuning.LifecycleAlphaStartScale,
		Tuning.LifecycleAlphaMidScale,
		Tuning.LifecycleAlphaEndScale,
		Tuning.AlphaCutoff,
		Tuning.BrightnessScale));
}

FText SWoWLoginSceneDebugPanel::GetSelectedBoneText() const
{
	const AWoWLoginGameMode* GameMode = LoginGameMode.Get();
	if (!GameMode)
	{
		return FText::FromString(TEXT("No login game mode"));
	}

	const TArray<int32> BoneIndices = GameMode->GetLoginBoneIndices();
	if (BoneIndices.IsEmpty())
	{
		return FText::FromString(TEXT("没有骨骼数据"));
	}

	const int32 BoneIndex = SelectedBoneIndex == INDEX_NONE ? BoneIndices[0] : SelectedBoneIndex;
	const AWoWLoginGameMode::FLoginBoneDebugSummary Summary = GameMode->GetLoginBoneDebugSummary(BoneIndex);
	return FText::FromString(FString::Printf(
		TEXT("当前 Bone[%d] parent=%d flags=%d geoset=%d samples=%d animated=%s"),
		BoneIndex,
		Summary.Parent,
		Summary.Flags,
		Summary.GeosetId,
		Summary.SampleCount,
		Summary.bHasAnimation ? TEXT("Yes") : TEXT("No")));
}

FText SWoWLoginSceneDebugPanel::GetSelectedBoneDetailsText() const
{
	const AWoWLoginGameMode* GameMode = LoginGameMode.Get();
	if (!GameMode)
	{
		return FText::FromString(TEXT(""));
	}

	const TArray<int32> BoneIndices = GameMode->GetLoginBoneIndices();
	if (BoneIndices.IsEmpty())
	{
		return FText::FromString(TEXT("M2 bones[] empty."));
	}

	const int32 BoneIndex = SelectedBoneIndex == INDEX_NONE ? BoneIndices[0] : SelectedBoneIndex;
	const AWoWLoginGameMode::FLoginBoneDebugSummary Summary = GameMode->GetLoginBoneDebugSummary(BoneIndex);
	const FVector Travel = Summary.MaxTranslation - Summary.MinTranslation;
	return FText::FromString(FString::Printf(
		TEXT("010 bones[%d]: parent=%d flags=%d geoset_id=%d pivot=%s\n")
		TEXT("animations[0].frames: samples=%d duration=%.1fms animated=%s\n")
		TEXT("frame0 translation=%s  last=%s\n")
		TEXT("translation min=%s  max=%s  range=%s\n")
		TEXT("说明: 这里直接来自导出的 M2/ANIM 骨骼矩阵。若粒子绑定此骨骼，粒子发射点会按 M2 flags 和 WMV/WotLK 粒子公式使用该矩阵。"),
		BoneIndex,
		Summary.Parent,
		Summary.Flags,
		Summary.GeosetId,
		*Summary.Pivot.ToString(),
		Summary.SampleCount,
		Summary.DurationMs,
		Summary.bHasAnimation ? TEXT("Yes") : TEXT("No"),
		*Summary.FirstTranslation.ToString(),
		*Summary.LastTranslation.ToString(),
		*Summary.MinTranslation.ToString(),
		*Summary.MaxTranslation.ToString(),
		*Travel.ToString()));
}

FText SWoWLoginSceneDebugPanel::GetSelectedTextureText() const
{
	const AWoWLoginGameMode* GameMode = LoginGameMode.Get();
	if (!GameMode)
	{
		return FText::FromString(TEXT("No login game mode"));
	}

	const TArray<int32> Indices = GetActiveTextureSectionIndices();
	if (Indices.IsEmpty())
	{
		return FText::FromString(TEXT("没有渲染 section"));
	}

	const int32 SectionIndex = SelectedTextureSectionIndex == INDEX_NONE ? Indices[0] : SelectedTextureSectionIndex;
	const FWoWLoginTextureSectionTuning Tuning = GameMode->GetLoginTextureSectionTuning(SectionIndex);
	const AWoWLoginGameMode::FSectionTextureTransformTrack* Track = GameMode->GetLoginRenderSectionByIndex(SectionIndex);
	const int32 EffectiveFlag = Tuning.RenderFlagOverride == INDEX_NONE && Track ? Track->MaterialFlags : Tuning.RenderFlagOverride;
	const int32 EffectiveBlend = Tuning.BlendModeOverride == INDEX_NONE && Track ? Track->RenderMode : Tuning.BlendModeOverride;
	return FText::FromString(FString::Printf(
		TEXT("当前 Section[%d]  备注=%s  Enabled=%s  M2Anim=%s  ForceVisible=%s  flag=%s(%d)  blendingmode=%s(%d)"),
		SectionIndex,
		Tuning.Note.IsEmpty() ? TEXT("-") : *Tuning.Note,
		Tuning.bEnabled ? TEXT("On") : TEXT("Off"),
		(Track && Track->Samples.Num() >= 2 && Tuning.bUseM2TextureAnimation) ? TEXT("On") : TEXT("None/Off"),
		Tuning.bForceVisibleDebug ? TEXT("On") : TEXT("Off"),
		*M2RenderFlagLabel(EffectiveFlag),
		EffectiveFlag,
		*M2RenderModeLabel(EffectiveBlend),
		EffectiveBlend));
}

FText SWoWLoginSceneDebugPanel::GetSelectedTextureDetailsText() const
{
	const AWoWLoginGameMode* GameMode = LoginGameMode.Get();
	if (!GameMode)
	{
		return FText::FromString(TEXT(""));
	}

	const TArray<int32> Indices = GetActiveTextureSectionIndices();
	if (Indices.IsEmpty())
	{
		return FText::FromString(TEXT("M2/SKIN render sections empty."));
	}

	const int32 SectionIndex = SelectedTextureSectionIndex == INDEX_NONE ? Indices[0] : SelectedTextureSectionIndex;
	const AWoWLoginGameMode::FSectionTextureTransformTrack* Track = GameMode->GetLoginRenderSectionByIndex(SectionIndex);
	if (!Track)
	{
		return FText::FromString(TEXT("请选择渲染 section。"));
	}

	const FWoWLoginTextureSectionTuning Tuning = GameMode->GetLoginTextureSectionTuning(SectionIndex);
	const int32 EffectiveFlag = Tuning.RenderFlagOverride == INDEX_NONE ? Track->MaterialFlags : Tuning.RenderFlagOverride;
	const int32 EffectiveBlend = Tuning.BlendModeOverride == INDEX_NONE ? Track->RenderMode : Tuning.BlendModeOverride;
	if (!bShowRawDetails)
	{
		return FText::FromString(FString::Printf(
			TEXT("Section %d  texture=%s  material=%d  flag=%s(%d)  blend=%s(%d)  UV samples=%d  note=%s"),
			Track->SlotIndex,
			*FPaths::GetCleanFilename(Track->PreviewPng),
			Track->MaterialIndex,
			*M2RenderFlagLabel(EffectiveFlag),
			EffectiveFlag,
			*M2RenderModeLabel(EffectiveBlend),
			EffectiveBlend,
			Track->Samples.Num(),
			Tuning.Note.IsEmpty() ? TEXT("-") : *Tuning.Note));
	}
	const FString FlagOverrideText = Tuning.RenderFlagOverride == INDEX_NONE
		? TEXT("使用M2原始")
		: FString::Printf(TEXT("%s(%d/0x%04X)"), *M2RenderFlagLabel(Tuning.RenderFlagOverride), Tuning.RenderFlagOverride, Tuning.RenderFlagOverride);
	const FString BlendOverrideText = Tuning.BlendModeOverride == INDEX_NONE
		? TEXT("使用M2原始")
		: FString::Printf(TEXT("%s(%d)"), *M2RenderModeLabel(Tuning.BlendModeOverride), Tuning.BlendModeOverride);
	return FText::FromString(FString::Printf(
		TEXT("M2/SKIN字段: section=%d submesh=%d submeshId=%d batchFlags=%d priorityPlane=%d shader_id=%d texture_unit=%d\n")
		TEXT("Materials[%d].flag=%s(%d/0x%04X) blendingmode=%s(%d)  Texture_Combiner[%d]=(%d,%d)\n")
		TEXT("SKIN TEXU: flags=%d flags2=%d order=%d submesh=%d submesh2=%d renderFlagsIndex=%d texunit=%d mode=%d textureLookupIndex=%d texunit2=%d transparencyLookupIndex=%d textureAnimLookupIndex=%d\n")
		TEXT("Lookups: textureIndex=%d textureType=%d textureFlags=%d textureTransformIndex=%d colorIndex=%d transparencyIndex=%d samples=%d duration=%.1fms texture=%s\n")
		TEXT("资产覆盖: Enabled=%s flag=%s blendingmode=%s；当前生效: flag=%s(%d/0x%04X) blendingmode=%s(%d)；%s；UE诊断覆盖=%s\nUV: Offset=%s Scale=%s Rotation=%.2f AlphaScale=%.2f Brightness=%.2f；没有 M2 TexAnimation 的 section 只使用手动 UV 参数。"),
		Track->SlotIndex,
		Track->SubmeshIndex,
		Track->SubmeshId,
		Track->BatchFlags,
		Track->PriorityPlane,
		Track->ShaderId,
		Track->TextureUnit,
		Track->MaterialIndex,
		*M2RenderFlagLabel(Track->MaterialFlags),
		Track->MaterialFlags,
		Track->MaterialFlags,
		*M2RenderModeLabel(Track->RenderMode),
		Track->RenderMode,
		Track->TextureCombinerIndex,
		Track->TextureCombinerRaw0,
		Track->TextureCombinerRaw1,
		Track->SkinTexUnitFlags,
		Track->SkinTexUnitFlags2,
		Track->SkinTexUnitOrder,
		Track->SkinSubmesh,
		Track->SkinSubmesh2,
		Track->SkinRenderFlagsIndex,
		Track->SkinTexUnit,
		Track->SkinTexUnitMode,
		Track->SkinTextureLookupIndex,
		Track->SkinTexUnit2,
		Track->SkinTransparencyLookupIndex,
		Track->SkinTextureAnimLookupIndex,
		Track->TextureIndex,
		Track->TextureType,
		Track->TextureFlags,
		Track->TextureTransformIndex,
		Track->ColorIndex,
		Track->TransparencyIndex,
		Track->Samples.Num(),
		Track->DurationMs,
		*FPaths::GetCleanFilename(Track->PreviewPng),
		Tuning.bEnabled ? TEXT("On") : TEXT("Off"),
		*FlagOverrideText,
		*BlendOverrideText,
		*M2RenderFlagLabel(EffectiveFlag),
		EffectiveFlag,
		EffectiveFlag,
		*M2RenderModeLabel(EffectiveBlend),
		EffectiveBlend,
		*M2RenderFlagEffectText(EffectiveFlag),
		*TextureRenderOverrideLabel(Tuning.RenderOverride),
		*Tuning.UVOffset.ToString(),
		*Tuning.UVScale.ToString(),
		Tuning.UVRotationDegrees,
		Tuning.AlphaScale,
		Tuning.BrightnessScale));
}

TArray<int32> SWoWLoginSceneDebugPanel::GetActiveTextureSectionIndices() const
{
	if (const AWoWLoginGameMode* GameMode = LoginGameMode.Get())
	{
		TArray<int32> Indices = GameMode->GetLoginRenderSectionIndices();
		if (Indices.IsEmpty())
		{
			Indices = GameMode->GetLoginTextureSectionIndices();
		}
		return Indices;
	}

	return {};
}

const AWoWLoginGameMode::FSectionTextureTransformTrack* SWoWLoginSceneDebugPanel::GetSelectedRenderSection() const
{
	const AWoWLoginGameMode* GameMode = LoginGameMode.Get();
	if (!GameMode)
	{
		return nullptr;
	}

	const TArray<int32> Indices = GetActiveTextureSectionIndices();
	const int32 SectionIndex = SelectedTextureSectionIndex == INDEX_NONE && !Indices.IsEmpty() ? Indices[0] : SelectedTextureSectionIndex;
	return GameMode->GetLoginRenderSectionByIndex(SectionIndex);
}

FText SWoWLoginSceneDebugPanel::GetTextureRenderFlagComboText() const
{
	const AWoWLoginGameMode* GameMode = LoginGameMode.Get();
	if (!GameMode)
	{
		return FText::FromString(TEXT("No GameMode"));
	}

	const TArray<int32> Indices = GetActiveTextureSectionIndices();
	const int32 SectionIndex = SelectedTextureSectionIndex == INDEX_NONE && !Indices.IsEmpty() ? Indices[0] : SelectedTextureSectionIndex;
	const FWoWLoginTextureSectionTuning Tuning = GameMode->GetLoginTextureSectionTuning(SectionIndex);
	if (Tuning.RenderFlagOverride == INDEX_NONE)
	{
		const AWoWLoginGameMode::FSectionTextureTransformTrack* Track = GameMode->GetLoginRenderSectionByIndex(SectionIndex);
		const int32 M2Value = Track ? Track->MaterialFlags : INDEX_NONE;
		return FText::FromString(FString::Printf(TEXT("M2原始: %s (%d)"), *M2RenderFlagLabel(M2Value), M2Value));
	}

	return FText::FromString(FString::Printf(TEXT("%s (%d)"), *M2RenderFlagLabel(Tuning.RenderFlagOverride), Tuning.RenderFlagOverride));
}

FText SWoWLoginSceneDebugPanel::GetTextureBlendModeComboText() const
{
	const AWoWLoginGameMode* GameMode = LoginGameMode.Get();
	if (!GameMode)
	{
		return FText::FromString(TEXT("No GameMode"));
	}

	const TArray<int32> Indices = GetActiveTextureSectionIndices();
	const int32 SectionIndex = SelectedTextureSectionIndex == INDEX_NONE && !Indices.IsEmpty() ? Indices[0] : SelectedTextureSectionIndex;
	const FWoWLoginTextureSectionTuning Tuning = GameMode->GetLoginTextureSectionTuning(SectionIndex);
	if (Tuning.BlendModeOverride == INDEX_NONE)
	{
		const AWoWLoginGameMode::FSectionTextureTransformTrack* Track = GameMode->GetLoginRenderSectionByIndex(SectionIndex);
		const int32 M2Value = Track ? Track->RenderMode : INDEX_NONE;
		return FText::FromString(FString::Printf(TEXT("M2原始: %s (%d)"), *M2RenderModeLabel(M2Value), M2Value));
	}

	return FText::FromString(FString::Printf(TEXT("%s (%d)"), *M2RenderModeLabel(Tuning.BlendModeOverride), Tuning.BlendModeOverride));
}

FText SWoWLoginSceneDebugPanel::GetTextureUERenderOverrideComboText() const
{
	const AWoWLoginGameMode* GameMode = LoginGameMode.Get();
	if (!GameMode)
	{
		return FText::FromString(TEXT("No GameMode"));
	}

	const TArray<int32> Indices = GetActiveTextureSectionIndices();
	const int32 SectionIndex = SelectedTextureSectionIndex == INDEX_NONE && !Indices.IsEmpty() ? Indices[0] : SelectedTextureSectionIndex;
	const FWoWLoginTextureSectionTuning Tuning = GameMode->GetLoginTextureSectionTuning(SectionIndex);
	return FText::FromString(TextureRenderOverrideLabel(Tuning.RenderOverride));
}

TSharedRef<SWidget> SWoWLoginSceneDebugPanel::MakeTextureUERenderOverrideCombo()
{
	struct FOption
	{
		EWoWLoginTextureRenderOverride Value;
		const TCHAR* Label;
	};
	const FOption Options[] =
	{
		{ EWoWLoginTextureRenderOverride::M2Original, TEXT("M2桥接: 按 Materials.flag + blendingmode") },
		{ EWoWLoginTextureRenderOverride::Opaque, TEXT("UE Opaque") },
		{ EWoWLoginTextureRenderOverride::Masked, TEXT("UE Masked / AlphaTest") },
		{ EWoWLoginTextureRenderOverride::AlphaBlend, TEXT("UE Translucent / AlphaBlend") },
		{ EWoWLoginTextureRenderOverride::Additive, TEXT("UE Additive") },
		{ EWoWLoginTextureRenderOverride::AdditiveAlpha, TEXT("WoW AdditiveAlpha 桥接") },
		{ EWoWLoginTextureRenderOverride::Modulate, TEXT("UE Modulate 近似") },
		{ EWoWLoginTextureRenderOverride::AlphaComposite, TEXT("UE AlphaComposite 近似") },
	};

	TSharedRef<SVerticalBox> Menu = SNew(SVerticalBox);
	for (const FOption& Option : Options)
	{
		Menu->AddSlot()
		.AutoHeight()
		[
			SNew(SButton)
			.Text(FText::FromString(Option.Label))
			.OnClicked_Lambda([this, Value = Option.Value]()
			{
				FSlateApplication::Get().DismissAllMenus();
				return OnTextureUERenderOverrideSelected(Value);
			})
		];
	}

	return SNew(SComboButton)
		.ButtonContent()
		[
			SNew(STextBlock)
			.ColorAndOpacity(TextColor)
			.Text(this, &SWoWLoginSceneDebugPanel::GetTextureUERenderOverrideComboText)
		]
		.MenuContent()
		[
			Menu
		];
}

TSharedRef<SWidget> SWoWLoginSceneDebugPanel::MakeLoginSceneCombo()
{
	TSharedRef<SVerticalBox> Menu = SNew(SVerticalBox);
	const AWoWLoginGameMode* GameMode = LoginGameMode.Get();
	if (GameMode)
	{
		const TArray<AWoWLoginGameMode::FLoginSceneDefinition>& Definitions = GameMode->GetLoginSceneDefinitions();
		for (int32 Index = 0; Index < Definitions.Num(); ++Index)
		{
			Menu->AddSlot()
			.AutoHeight()
			[
				SNew(SButton)
				.Text(FText::FromString(FString::Printf(TEXT("%d %s"), Index + 1, *Definitions[Index].DisplayName)))
				.OnClicked_Lambda([this, Index]()
				{
					FSlateApplication::Get().DismissAllMenus();
					return OnLoginSceneSelected(Index);
				})
			];
		}
	}

	return SNew(SComboButton)
		.ButtonContent()
		[
			SNew(STextBlock)
			.ColorAndOpacity(TextColor)
			.Text(this, &SWoWLoginSceneDebugPanel::GetLoginSceneComboText)
		]
		.MenuContent()
		[
			Menu
		];
}

TSharedRef<SWidget> SWoWLoginSceneDebugPanel::MakeTextureRenderFlagCombo()
{
	TSharedRef<SVerticalBox> Menu = SNew(SVerticalBox);
	Menu->AddSlot()
	.AutoHeight()
	[
		SNew(SButton)
		.Text(FText::FromString(TEXT("使用 M2 原始值")))
		.OnClicked_Lambda([this]()
		{
			FSlateApplication::Get().DismissAllMenus();
			FocusKeyboardEnumField(EKeyboardEnumField::TextureRenderFlag);
			return OnTextureRenderFlagSelected(INDEX_NONE);
		})
	];

	for (int32 FlagValue = 0; FlagValue <= 31; ++FlagValue)
	{
		Menu->AddSlot()
		.AutoHeight()
		[
			SNew(SButton)
			.Text(FText::FromString(FString::Printf(TEXT("%s (%d)"), *M2RenderFlagLabel(FlagValue), FlagValue)))
			.OnClicked_Lambda([this, FlagValue]()
			{
				FSlateApplication::Get().DismissAllMenus();
				FocusKeyboardEnumField(EKeyboardEnumField::TextureRenderFlag);
				return OnTextureRenderFlagSelected(FlagValue);
			})
		];
	}

	return SNew(SComboButton)
		.OnComboBoxOpened_Lambda([this]()
		{
			FocusKeyboardEnumField(EKeyboardEnumField::TextureRenderFlag);
		})
		.ButtonContent()
		[
			SNew(STextBlock)
			.ColorAndOpacity(TextColor)
			.Text(this, &SWoWLoginSceneDebugPanel::GetTextureRenderFlagComboText)
		]
		.MenuContent()
		[
			SNew(SScrollBox)
			+ SScrollBox::Slot()
			[
				Menu
			]
		];
}

TSharedRef<SWidget> SWoWLoginSceneDebugPanel::MakeTextureBlendModeCombo()
{
	TSharedRef<SVerticalBox> Menu = SNew(SVerticalBox);
	Menu->AddSlot()
	.AutoHeight()
	[
		SNew(SButton)
		.Text(FText::FromString(TEXT("使用 M2 原始值")))
		.OnClicked_Lambda([this]()
		{
			FSlateApplication::Get().DismissAllMenus();
			FocusKeyboardEnumField(EKeyboardEnumField::TextureBlendMode);
			return OnTextureBlendModeSelected(INDEX_NONE);
		})
	];

	for (int32 BlendMode = 0; BlendMode <= 6; ++BlendMode)
	{
		Menu->AddSlot()
		.AutoHeight()
		[
			SNew(SButton)
			.Text(FText::FromString(FString::Printf(TEXT("%s (%d)"), *M2RenderModeLabel(BlendMode), BlendMode)))
			.OnClicked_Lambda([this, BlendMode]()
			{
				FSlateApplication::Get().DismissAllMenus();
				FocusKeyboardEnumField(EKeyboardEnumField::TextureBlendMode);
				return OnTextureBlendModeSelected(BlendMode);
			})
		];
	}

	return SNew(SComboButton)
		.OnComboBoxOpened_Lambda([this]()
		{
			FocusKeyboardEnumField(EKeyboardEnumField::TextureBlendMode);
		})
		.ButtonContent()
		[
			SNew(STextBlock)
			.ColorAndOpacity(TextColor)
			.Text(this, &SWoWLoginSceneDebugPanel::GetTextureBlendModeComboText)
		]
		.MenuContent()
		[
			Menu
		];
}

TSharedRef<SWidget> SWoWLoginSceneDebugPanel::MakeCharacterPreviewAnimationCombo()
{
	return SAssignNew(CharacterAnimationMenuAnchor, SMenuAnchor)
		.Placement(MenuPlacement_ComboBox)
		.FitInWindow(true)
		.IsCollapsedByParent(true)
		.OnMenuOpenChanged_Lambda([this](bool bIsOpen)
		{
			if (bIsOpen)
			{
				CharacterAnimationFilterText.Empty();
				FocusKeyboardEnumField(EKeyboardEnumField::CharacterAnimation);
				if (CharacterAnimationMenuContentBox.IsValid())
				{
					CharacterAnimationMenuContentBox->RegisterActiveTimer(0.0f, FWidgetActiveTimerDelegate::CreateLambda([this](double, float)
					{
						if (!CharacterAnimationMenuAnchor.IsValid() || !CharacterAnimationMenuAnchor->IsOpen() || !LoginGameMode.IsValid())
						{
							return EActiveTimerReturnType::Stop;
						}
						RebuildCharacterAnimationMenuContent();
						const AWoWLoginGameMode::FCharacterPreviewAnimationCatalogState CatalogState = LoginGameMode.Get()->GetCharacterPreviewAnimationCatalogState();
						return CatalogState.bWarming ? EActiveTimerReturnType::Continue : EActiveTimerReturnType::Stop;
					}));
				}
				ScrollCharacterAnimationSelectionIntoView();
			}
			else
			{
				CharacterAnimationScrollBox.Reset();
				CharacterAnimationMenuContentBox.Reset();
				CharacterAnimationRowWidgets.Empty();
			}
		})
		.OnGetMenuContent_Lambda([this]() -> TSharedRef<SWidget>
		{
			CharacterAnimationRowWidgets.Empty();
			TSharedRef<SBox> Root = SNew(SBox)
				.WidthOverride(560.0f)
				.HeightOverride(420.0f)
				[
					SNew(SBorder)
					.BorderImage(FCoreStyle::Get().GetBrush(TEXT("WhiteBrush")))
					.BorderBackgroundColor(PopupBackgroundColor)
					.Padding(8.0f)
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot()
						[
							SAssignNew(CharacterAnimationScrollBox, SScrollBox)
							.ScrollBarAlwaysVisible(true)
							+ SScrollBox::Slot()
							[
								SAssignNew(CharacterAnimationMenuContentBox, SVerticalBox)
							]
						]
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0.0f, 6.0f, 0.0f, 0.0f)
						[
							SNew(STextBlock)
							.ColorAndOpacity(MutedTextColor)
							.AutoWrapText(true)
							.Text(FText::FromString(TEXT("输入过滤文字后，可继续用滚轮浏览；选择项只改变当前预览动作覆盖。")))
						]
					]
				];
			RebuildCharacterAnimationMenuContent();
			return Root;
		})
		[
			SNew(SButton)
			.ContentPadding(FMargin(8.0f, 2.0f))
			.OnClicked_Lambda([this]()
			{
				if (CharacterAnimationMenuAnchor.IsValid())
				{
					CharacterAnimationMenuAnchor->SetIsOpen(CharacterAnimationMenuAnchor->ShouldOpenDueToClick(), false);
				}
				return FReply::Handled();
			})
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.ColorAndOpacity(TextColor)
					.Text(this, &SWoWLoginSceneDebugPanel::GetCharacterPreviewAnimationComboText)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(8.0f, 0.0f, 0.0f, 0.0f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.ColorAndOpacity(MutedTextColor)
					.Text(FText::FromString(TEXT("v")))
				]
			]
		];
}

FText SWoWLoginSceneDebugPanel::GetFieldText(EDebugFloatField Field) const
{
	if (const AWoWLoginGameMode* GameMode = LoginGameMode.Get())
	{
		if (IsParticleField(Field))
		{
			const TArray<int32> Indices = GameMode->GetLoginParticleEmitterIndices();
			const int32 EmitterIndex = SelectedParticleEmitterIndex == INDEX_NONE && !Indices.IsEmpty() ? Indices[0] : SelectedParticleEmitterIndex;
			const FWoWLoginParticleEmitterTuning Tuning = GameMode->GetLoginParticleEmitterTuning(EmitterIndex);
			float Value = 0.0f;
			switch (Field)
			{
			case EDebugFloatField::ParticlePositionX: Value = Tuning.PositionOffset.X; break;
			case EDebugFloatField::ParticlePositionY: Value = Tuning.PositionOffset.Y; break;
			case EDebugFloatField::ParticlePositionZ: Value = Tuning.PositionOffset.Z; break;
			case EDebugFloatField::ParticleEmissionRateScale: Value = Tuning.EmissionRateScale; break;
			case EDebugFloatField::ParticleEmissionSpeedScale: Value = Tuning.EmissionSpeedScale; break;
			case EDebugFloatField::ParticleLifespanScale: Value = Tuning.LifespanScale; break;
			case EDebugFloatField::ParticleAreaLengthScale: Value = Tuning.AreaLengthScale; break;
			case EDebugFloatField::ParticleAreaWidthScale: Value = Tuning.AreaWidthScale; break;
			case EDebugFloatField::ParticleGravityScale: Value = Tuning.GravityScale; break;
			case EDebugFloatField::ParticleGravity2Scale: Value = Tuning.Gravity2Scale; break;
			case EDebugFloatField::ParticleVerticalRangeScale: Value = Tuning.VerticalRangeScale; break;
			case EDebugFloatField::ParticleHorizontalRangeScale: Value = Tuning.HorizontalRangeScale; break;
			case EDebugFloatField::ParticleSizeScale: Value = Tuning.SizeScale; break;
			case EDebugFloatField::ParticleLifecycleSizeStartScale: Value = Tuning.LifecycleSizeStartScale; break;
			case EDebugFloatField::ParticleLifecycleSizeMidScale: Value = Tuning.LifecycleSizeMidScale; break;
			case EDebugFloatField::ParticleLifecycleSizeEndScale: Value = Tuning.LifecycleSizeEndScale; break;
			case EDebugFloatField::ParticleAlphaScale: Value = Tuning.AlphaScale; break;
			case EDebugFloatField::ParticleLifecycleAlphaStartScale: Value = Tuning.LifecycleAlphaStartScale; break;
			case EDebugFloatField::ParticleLifecycleAlphaMidScale: Value = Tuning.LifecycleAlphaMidScale; break;
			case EDebugFloatField::ParticleLifecycleAlphaEndScale: Value = Tuning.LifecycleAlphaEndScale; break;
			case EDebugFloatField::ParticleAlphaCutoff: Value = Tuning.AlphaCutoff; break;
			case EDebugFloatField::ParticleBrightnessScale: Value = Tuning.BrightnessScale; break;
			default: break;
			}
			return FText::FromString(FString::Printf(TEXT("%s: %.3f"), *DebugFieldLabel(Field).ToString(), Value));
		}

		const FWoWLoginSceneM2CameraState Camera = GameMode->GetLoginSceneM2CameraState();
		switch (Field)
		{
		case EDebugFloatField::CameraPositionX: return FText::FromString(FString::Printf(TEXT("struct C3Vector Pos.x: %.6f"), Camera.Pos.X));
		case EDebugFloatField::CameraPositionY: return FText::FromString(FString::Printf(TEXT("struct C3Vector Pos.y: %.6f"), Camera.Pos.Y));
		case EDebugFloatField::CameraPositionZ: return FText::FromString(FString::Printf(TEXT("struct C3Vector Pos.z: %.6f"), Camera.Pos.Z));
		case EDebugFloatField::CameraTargetX: return FText::FromString(FString::Printf(TEXT("struct C3Vector Target.x: %.6f"), Camera.Target.X));
		case EDebugFloatField::CameraTargetY: return FText::FromString(FString::Printf(TEXT("struct C3Vector Target.y: %.6f"), Camera.Target.Y));
		case EDebugFloatField::CameraTargetZ: return FText::FromString(FString::Printf(TEXT("struct C3Vector Target.z: %.6f"), Camera.Target.Z));
		case EDebugFloatField::CameraFov: return FText::FromString(FString::Printf(TEXT("float FOV: %.6f  Deg %.6f  UEProjectionDeg %.2f"), Camera.FovRadians, FMath::RadiansToDegrees(Camera.FovRadians), Camera.FovRadians * 34.5f));
	case EDebugFloatField::CameraNearClipping: return FText::FromString(FString::Printf(TEXT("float NearClipping: %.6f"), Camera.NearClipping));
	case EDebugFloatField::CameraFarClipping: return FText::FromString(FString::Printf(TEXT("float FarClipping: %.6f"), Camera.FarClipping));
	case EDebugFloatField::CameraRollEffectX: return FText::FromString(FString::Printf(TEXT("struct ABlock_F RollEffect.val.x: %.6f"), Camera.RollRadians));
	case EDebugFloatField::GlobalBrightnessScale: return FText::FromString(FString::Printf(TEXT("GlobalBrightnessScale: %.6f"), GameMode->GetLoginSceneTuningState().GlobalBrightnessScale));
	default: break;
	}
	}

	const float Range = GetFieldRange(Field);
	const float Value = (GetSliderValue(Field) * 2.0f - 1.0f) * Range;
	return FText::FromString(FString::Printf(TEXT("%s: %.2f"), *DebugFieldLabel(Field).ToString(), Value));
}

TOptional<float> SWoWLoginSceneDebugPanel::GetFieldNumericValue(EDebugFloatField Field) const
{
	const AWoWLoginGameMode* GameMode = LoginGameMode.Get();
	if (!GameMode)
	{
		return TOptional<float>();
	}

	const FWoWLoginSceneTuningState State = GameMode->GetLoginSceneTuningState();
	const FWoWLoginSceneM2CameraState Camera = GameMode->GetLoginSceneM2CameraState();
	const TArray<int32> Indices = GameMode->GetLoginParticleEmitterIndices();
	const int32 EmitterIndex = SelectedParticleEmitterIndex == INDEX_NONE && !Indices.IsEmpty() ? Indices[0] : SelectedParticleEmitterIndex;
	const FWoWLoginParticleEmitterTuning ParticleTuning = GameMode->GetLoginParticleEmitterTuning(EmitterIndex);
	const TArray<int32> TextureIndices = GetActiveTextureSectionIndices();
	const int32 TextureSectionIndex = SelectedTextureSectionIndex == INDEX_NONE && !TextureIndices.IsEmpty() ? TextureIndices[0] : SelectedTextureSectionIndex;
	const FWoWLoginTextureSectionTuning TextureTuning = GameMode->GetLoginTextureSectionTuning(TextureSectionIndex);
	const TArray<AWoWLoginGameMode::FLoginSceneLight>& Lights = GameMode->GetLoginSceneLights();
	const int32 LightIndex = SelectedLightIndex == INDEX_NONE && !Lights.IsEmpty() ? Lights[0].Index : SelectedLightIndex;
	const FWoWLoginLightTuning LightTuning = GameMode->GetLoginSceneLightTuning(LightIndex);
	switch (Field)
	{
	case EDebugFloatField::SceneLocationX: return State.SceneLocationOffset.X;
	case EDebugFloatField::SceneLocationY: return State.SceneLocationOffset.Y;
	case EDebugFloatField::SceneLocationZ: return State.SceneLocationOffset.Z;
	case EDebugFloatField::SceneRotationPitch: return State.SceneRotationOffset.Pitch;
	case EDebugFloatField::SceneRotationYaw: return State.SceneRotationOffset.Yaw;
	case EDebugFloatField::SceneRotationRoll: return State.SceneRotationOffset.Roll;
	case EDebugFloatField::SceneScaleX: return State.SceneScaleMultiplier.X;
	case EDebugFloatField::SceneScaleY: return State.SceneScaleMultiplier.Y;
	case EDebugFloatField::SceneScaleZ: return State.SceneScaleMultiplier.Z;
	case EDebugFloatField::CharacterPreviewLocationX: return State.CharacterPreviewLocationOffset.X;
	case EDebugFloatField::CharacterPreviewLocationY: return State.CharacterPreviewLocationOffset.Y;
	case EDebugFloatField::CharacterPreviewLocationZ: return State.CharacterPreviewLocationOffset.Z;
	case EDebugFloatField::CharacterPreviewRotationPitch: return State.CharacterPreviewRotationOffset.Pitch;
	case EDebugFloatField::CharacterPreviewRotationYaw: return State.CharacterPreviewRotationOffset.Yaw;
	case EDebugFloatField::CharacterPreviewRotationRoll: return State.CharacterPreviewRotationOffset.Roll;
	case EDebugFloatField::CharacterPreviewUniformScale: return State.CharacterPreviewUniformScale;
	case EDebugFloatField::CharacterPreviewScaleX: return State.CharacterPreviewScaleMultiplier.X;
	case EDebugFloatField::CharacterPreviewScaleY: return State.CharacterPreviewScaleMultiplier.Y;
	case EDebugFloatField::CharacterPreviewScaleZ: return State.CharacterPreviewScaleMultiplier.Z;
	case EDebugFloatField::CameraPositionX: return Camera.Pos.X;
	case EDebugFloatField::CameraPositionY: return Camera.Pos.Y;
	case EDebugFloatField::CameraPositionZ: return Camera.Pos.Z;
	case EDebugFloatField::CameraTargetX: return Camera.Target.X;
	case EDebugFloatField::CameraTargetY: return Camera.Target.Y;
	case EDebugFloatField::CameraTargetZ: return Camera.Target.Z;
	case EDebugFloatField::CameraFov: return Camera.FovRadians;
	case EDebugFloatField::CameraNearClipping: return Camera.NearClipping;
	case EDebugFloatField::CameraFarClipping: return Camera.FarClipping;
	case EDebugFloatField::CameraRollEffectX: return Camera.RollRadians;
	case EDebugFloatField::GlobalBrightnessScale: return State.GlobalBrightnessScale;
	case EDebugFloatField::M2PhysicalLightIntensityScale: return State.M2PhysicalLightIntensityScale;
	case EDebugFloatField::LightDiffuseIntensityScale: return LightTuning.DiffuseIntensityScale;
	case EDebugFloatField::LightAmbientIntensityScale: return LightTuning.AmbientIntensityScale;
	case EDebugFloatField::LightDiffuseColorScaleR: return LightTuning.DiffuseColorScale.R;
	case EDebugFloatField::LightDiffuseColorScaleG: return LightTuning.DiffuseColorScale.G;
	case EDebugFloatField::LightDiffuseColorScaleB: return LightTuning.DiffuseColorScale.B;
	case EDebugFloatField::LightAmbientColorScaleR: return LightTuning.AmbientColorScale.R;
	case EDebugFloatField::LightAmbientColorScaleG: return LightTuning.AmbientColorScale.G;
	case EDebugFloatField::LightAmbientColorScaleB: return LightTuning.AmbientColorScale.B;
	case EDebugFloatField::LightAttenuationRadiusScale: return LightTuning.AttenuationRadiusScale;
	case EDebugFloatField::ParticlePositionX: return ParticleTuning.PositionOffset.X;
	case EDebugFloatField::ParticlePositionY: return ParticleTuning.PositionOffset.Y;
	case EDebugFloatField::ParticlePositionZ: return ParticleTuning.PositionOffset.Z;
	case EDebugFloatField::ParticleEmissionRateScale: return ParticleTuning.EmissionRateScale;
	case EDebugFloatField::ParticleEmissionSpeedScale: return ParticleTuning.EmissionSpeedScale;
	case EDebugFloatField::ParticleLifespanScale: return ParticleTuning.LifespanScale;
	case EDebugFloatField::ParticleAreaLengthScale: return ParticleTuning.AreaLengthScale;
	case EDebugFloatField::ParticleAreaWidthScale: return ParticleTuning.AreaWidthScale;
	case EDebugFloatField::ParticleGravityScale: return ParticleTuning.GravityScale;
	case EDebugFloatField::ParticleGravity2Scale: return ParticleTuning.Gravity2Scale;
	case EDebugFloatField::ParticleVerticalRangeScale: return ParticleTuning.VerticalRangeScale;
	case EDebugFloatField::ParticleHorizontalRangeScale: return ParticleTuning.HorizontalRangeScale;
	case EDebugFloatField::ParticleSizeScale: return ParticleTuning.SizeScale;
	case EDebugFloatField::ParticleLifecycleSizeStartScale: return ParticleTuning.LifecycleSizeStartScale;
	case EDebugFloatField::ParticleLifecycleSizeMidScale: return ParticleTuning.LifecycleSizeMidScale;
	case EDebugFloatField::ParticleLifecycleSizeEndScale: return ParticleTuning.LifecycleSizeEndScale;
	case EDebugFloatField::ParticleAlphaScale: return ParticleTuning.AlphaScale;
	case EDebugFloatField::ParticleLifecycleAlphaStartScale: return ParticleTuning.LifecycleAlphaStartScale;
	case EDebugFloatField::ParticleLifecycleAlphaMidScale: return ParticleTuning.LifecycleAlphaMidScale;
	case EDebugFloatField::ParticleLifecycleAlphaEndScale: return ParticleTuning.LifecycleAlphaEndScale;
	case EDebugFloatField::ParticleAlphaCutoff: return ParticleTuning.AlphaCutoff;
	case EDebugFloatField::ParticleBrightnessScale: return ParticleTuning.BrightnessScale;
	case EDebugFloatField::TextureUVOffsetX: return TextureTuning.UVOffset.X;
	case EDebugFloatField::TextureUVOffsetY: return TextureTuning.UVOffset.Y;
	case EDebugFloatField::TextureUVScaleX: return TextureTuning.UVScale.X;
	case EDebugFloatField::TextureUVScaleY: return TextureTuning.UVScale.Y;
	case EDebugFloatField::TextureUVRotationDegrees: return TextureTuning.UVRotationDegrees;
	case EDebugFloatField::TextureAlphaScale: return TextureTuning.AlphaScale;
	case EDebugFloatField::TextureBrightnessScale: return TextureTuning.BrightnessScale;
	default:
		return TOptional<float>();
	}
}

float SWoWLoginSceneDebugPanel::GetSliderValue(EDebugFloatField Field) const
{
	const AWoWLoginGameMode* GameMode = LoginGameMode.Get();
	if (!GameMode)
	{
		return 0.5f;
	}

	const FWoWLoginSceneTuningState State = GameMode->GetLoginSceneTuningState();
	const FWoWLoginSceneM2CameraState Base = GameMode->GetLoginSceneBaseM2CameraState();
	const FWoWLoginSceneM2CameraState Current = GameMode->GetLoginSceneM2CameraState();
	const TArray<int32> Indices = GameMode->GetLoginParticleEmitterIndices();
	const int32 EmitterIndex = SelectedParticleEmitterIndex == INDEX_NONE && !Indices.IsEmpty() ? Indices[0] : SelectedParticleEmitterIndex;
	const FWoWLoginParticleEmitterTuning ParticleTuning = GameMode->GetLoginParticleEmitterTuning(EmitterIndex);
	const TArray<int32> TextureIndices = GetActiveTextureSectionIndices();
	const int32 TextureSectionIndex = SelectedTextureSectionIndex == INDEX_NONE && !TextureIndices.IsEmpty() ? TextureIndices[0] : SelectedTextureSectionIndex;
	const FWoWLoginTextureSectionTuning TextureTuning = GameMode->GetLoginTextureSectionTuning(TextureSectionIndex);
	const TArray<AWoWLoginGameMode::FLoginSceneLight>& Lights = GameMode->GetLoginSceneLights();
	const int32 LightIndex = SelectedLightIndex == INDEX_NONE && !Lights.IsEmpty() ? Lights[0].Index : SelectedLightIndex;
	const FWoWLoginLightTuning LightTuning = GameMode->GetLoginSceneLightTuning(LightIndex);
	float RawValue = 0.0f;
	float BaseValue = 0.0f;
	switch (Field)
	{
	case EDebugFloatField::SceneLocationX: RawValue = State.SceneLocationOffset.X; break;
	case EDebugFloatField::SceneLocationY: RawValue = State.SceneLocationOffset.Y; break;
	case EDebugFloatField::SceneLocationZ: RawValue = State.SceneLocationOffset.Z; break;
	case EDebugFloatField::SceneRotationPitch: RawValue = State.SceneRotationOffset.Pitch; break;
	case EDebugFloatField::SceneRotationYaw: RawValue = State.SceneRotationOffset.Yaw; break;
	case EDebugFloatField::SceneRotationRoll: RawValue = State.SceneRotationOffset.Roll; break;
	case EDebugFloatField::SceneScaleX: RawValue = State.SceneScaleMultiplier.X - 1.0f; break;
	case EDebugFloatField::SceneScaleY: RawValue = State.SceneScaleMultiplier.Y - 1.0f; break;
	case EDebugFloatField::SceneScaleZ: RawValue = State.SceneScaleMultiplier.Z - 1.0f; break;
	case EDebugFloatField::CharacterPreviewLocationX: RawValue = State.CharacterPreviewLocationOffset.X; break;
	case EDebugFloatField::CharacterPreviewLocationY: RawValue = State.CharacterPreviewLocationOffset.Y; break;
	case EDebugFloatField::CharacterPreviewLocationZ: RawValue = State.CharacterPreviewLocationOffset.Z; break;
	case EDebugFloatField::CharacterPreviewRotationPitch: RawValue = State.CharacterPreviewRotationOffset.Pitch; break;
	case EDebugFloatField::CharacterPreviewRotationYaw: RawValue = State.CharacterPreviewRotationOffset.Yaw; break;
	case EDebugFloatField::CharacterPreviewRotationRoll: RawValue = State.CharacterPreviewRotationOffset.Roll; break;
	case EDebugFloatField::CharacterPreviewUniformScale: RawValue = State.CharacterPreviewUniformScale - 1.0f; break;
	case EDebugFloatField::CharacterPreviewScaleX: RawValue = State.CharacterPreviewScaleMultiplier.X - 1.0f; break;
	case EDebugFloatField::CharacterPreviewScaleY: RawValue = State.CharacterPreviewScaleMultiplier.Y - 1.0f; break;
	case EDebugFloatField::CharacterPreviewScaleZ: RawValue = State.CharacterPreviewScaleMultiplier.Z - 1.0f; break;
	case EDebugFloatField::CameraPositionX: RawValue = Current.Pos.X; BaseValue = Base.Pos.X; break;
	case EDebugFloatField::CameraPositionY: RawValue = Current.Pos.Y; BaseValue = Base.Pos.Y; break;
	case EDebugFloatField::CameraPositionZ: RawValue = Current.Pos.Z; BaseValue = Base.Pos.Z; break;
	case EDebugFloatField::CameraTargetX: RawValue = Current.Target.X; BaseValue = Base.Target.X; break;
	case EDebugFloatField::CameraTargetY: RawValue = Current.Target.Y; BaseValue = Base.Target.Y; break;
	case EDebugFloatField::CameraTargetZ: RawValue = Current.Target.Z; BaseValue = Base.Target.Z; break;
	case EDebugFloatField::CameraFov: RawValue = Current.FovRadians; BaseValue = Base.FovRadians; break;
	case EDebugFloatField::CameraNearClipping: RawValue = Current.NearClipping; BaseValue = Base.NearClipping; break;
	case EDebugFloatField::CameraFarClipping: RawValue = Current.FarClipping; BaseValue = Base.FarClipping; break;
	case EDebugFloatField::CameraRollEffectX: RawValue = Current.RollRadians; BaseValue = Base.RollRadians; break;
	case EDebugFloatField::GlobalBrightnessScale: RawValue = State.GlobalBrightnessScale - 1.0f; break;
	case EDebugFloatField::M2PhysicalLightIntensityScale: RawValue = State.M2PhysicalLightIntensityScale - 80.0f; break;
	case EDebugFloatField::LightDiffuseIntensityScale: RawValue = LightTuning.DiffuseIntensityScale - 1.0f; break;
	case EDebugFloatField::LightAmbientIntensityScale: RawValue = LightTuning.AmbientIntensityScale - 1.0f; break;
	case EDebugFloatField::LightDiffuseColorScaleR: RawValue = LightTuning.DiffuseColorScale.R - 1.0f; break;
	case EDebugFloatField::LightDiffuseColorScaleG: RawValue = LightTuning.DiffuseColorScale.G - 1.0f; break;
	case EDebugFloatField::LightDiffuseColorScaleB: RawValue = LightTuning.DiffuseColorScale.B - 1.0f; break;
	case EDebugFloatField::LightAmbientColorScaleR: RawValue = LightTuning.AmbientColorScale.R - 1.0f; break;
	case EDebugFloatField::LightAmbientColorScaleG: RawValue = LightTuning.AmbientColorScale.G - 1.0f; break;
	case EDebugFloatField::LightAmbientColorScaleB: RawValue = LightTuning.AmbientColorScale.B - 1.0f; break;
	case EDebugFloatField::LightAttenuationRadiusScale: RawValue = LightTuning.AttenuationRadiusScale - 1.0f; break;
	case EDebugFloatField::ParticlePositionX: RawValue = ParticleTuning.PositionOffset.X; break;
	case EDebugFloatField::ParticlePositionY: RawValue = ParticleTuning.PositionOffset.Y; break;
	case EDebugFloatField::ParticlePositionZ: RawValue = ParticleTuning.PositionOffset.Z; break;
	case EDebugFloatField::ParticleEmissionRateScale: RawValue = ParticleTuning.EmissionRateScale - 1.0f; break;
	case EDebugFloatField::ParticleEmissionSpeedScale: RawValue = ParticleTuning.EmissionSpeedScale - 1.0f; break;
	case EDebugFloatField::ParticleLifespanScale: RawValue = ParticleTuning.LifespanScale - 1.0f; break;
	case EDebugFloatField::ParticleAreaLengthScale: RawValue = ParticleTuning.AreaLengthScale - 1.0f; break;
	case EDebugFloatField::ParticleAreaWidthScale: RawValue = ParticleTuning.AreaWidthScale - 1.0f; break;
	case EDebugFloatField::ParticleGravityScale: RawValue = ParticleTuning.GravityScale - 1.0f; break;
	case EDebugFloatField::ParticleGravity2Scale: RawValue = ParticleTuning.Gravity2Scale - 1.0f; break;
	case EDebugFloatField::ParticleVerticalRangeScale: RawValue = ParticleTuning.VerticalRangeScale - 1.0f; break;
	case EDebugFloatField::ParticleHorizontalRangeScale: RawValue = ParticleTuning.HorizontalRangeScale - 1.0f; break;
	case EDebugFloatField::ParticleSizeScale: RawValue = ParticleTuning.SizeScale - 1.0f; break;
	case EDebugFloatField::ParticleLifecycleSizeStartScale: RawValue = ParticleTuning.LifecycleSizeStartScale - 1.0f; break;
	case EDebugFloatField::ParticleLifecycleSizeMidScale: RawValue = ParticleTuning.LifecycleSizeMidScale - 1.0f; break;
	case EDebugFloatField::ParticleLifecycleSizeEndScale: RawValue = ParticleTuning.LifecycleSizeEndScale - 1.0f; break;
	case EDebugFloatField::ParticleAlphaScale: RawValue = ParticleTuning.AlphaScale - 1.0f; break;
	case EDebugFloatField::ParticleLifecycleAlphaStartScale: RawValue = ParticleTuning.LifecycleAlphaStartScale - 1.0f; break;
	case EDebugFloatField::ParticleLifecycleAlphaMidScale: RawValue = ParticleTuning.LifecycleAlphaMidScale - 1.0f; break;
	case EDebugFloatField::ParticleLifecycleAlphaEndScale: RawValue = ParticleTuning.LifecycleAlphaEndScale - 1.0f; break;
	case EDebugFloatField::ParticleAlphaCutoff: RawValue = ParticleTuning.AlphaCutoff; break;
	case EDebugFloatField::ParticleBrightnessScale: RawValue = ParticleTuning.BrightnessScale - 1.0f; break;
	case EDebugFloatField::TextureUVOffsetX: RawValue = TextureTuning.UVOffset.X; break;
	case EDebugFloatField::TextureUVOffsetY: RawValue = TextureTuning.UVOffset.Y; break;
	case EDebugFloatField::TextureUVScaleX: RawValue = TextureTuning.UVScale.X - 1.0f; break;
	case EDebugFloatField::TextureUVScaleY: RawValue = TextureTuning.UVScale.Y - 1.0f; break;
	case EDebugFloatField::TextureUVRotationDegrees: RawValue = TextureTuning.UVRotationDegrees; break;
	case EDebugFloatField::TextureAlphaScale: RawValue = TextureTuning.AlphaScale - 1.0f; break;
	case EDebugFloatField::TextureBrightnessScale: RawValue = TextureTuning.BrightnessScale - 1.0f; break;
	default: break;
	}

	if (IsTextureField(Field) || IsLightField(Field))
	{
		const float Range = GetFieldRange(Field);
		return FMath::Clamp((RawValue / Range + 1.0f) * 0.5f, 0.0f, 1.0f);
	}
	if (IsParticleField(Field))
	{
		const float Range = GetFieldRange(Field);
		return FMath::Clamp((RawValue / Range + 1.0f) * 0.5f, 0.0f, 1.0f);
	}

	if (IsCameraPositionField(Field) || IsCameraTargetField(Field) || Field == EDebugFloatField::CameraFov || Field == EDebugFloatField::CameraNearClipping || Field == EDebugFloatField::CameraFarClipping || Field == EDebugFloatField::CameraRollEffectX)
	{
		const float Range = GetFieldRange(Field);
		return FMath::Clamp(((RawValue - BaseValue) / Range + 1.0f) * 0.5f, 0.0f, 1.0f);
	}

	const float Range = GetFieldRange(Field);
	return FMath::Clamp((RawValue / Range + 1.0f) * 0.5f, 0.0f, 1.0f);
}

void SWoWLoginSceneDebugPanel::SetFieldDisplayValue(float Value, EDebugFloatField Field)
{
	AWoWLoginGameMode* GameMode = LoginGameMode.Get();
	if (!GameMode)
	{
		return;
	}

	FWoWLoginSceneTuningState State = GameMode->GetLoginSceneTuningState();
	FWoWLoginSceneM2CameraState Camera = GameMode->GetLoginSceneM2CameraState();
	if (IsLightField(Field))
	{
		const TArray<AWoWLoginGameMode::FLoginSceneLight>& Lights = GameMode->GetLoginSceneLights();
		const int32 LightIndex = SelectedLightIndex == INDEX_NONE && !Lights.IsEmpty() ? Lights[0].Index : SelectedLightIndex;
		FWoWLoginLightTuning Tuning = GameMode->GetLoginSceneLightTuning(LightIndex);
		switch (Field)
		{
		case EDebugFloatField::LightDiffuseIntensityScale: Tuning.DiffuseIntensityScale = FMath::Max(0.0f, Value); break;
		case EDebugFloatField::LightAmbientIntensityScale: Tuning.AmbientIntensityScale = FMath::Max(0.0f, Value); break;
		case EDebugFloatField::LightDiffuseColorScaleR: Tuning.DiffuseColorScale.R = FMath::Max(0.0f, Value); break;
		case EDebugFloatField::LightDiffuseColorScaleG: Tuning.DiffuseColorScale.G = FMath::Max(0.0f, Value); break;
		case EDebugFloatField::LightDiffuseColorScaleB: Tuning.DiffuseColorScale.B = FMath::Max(0.0f, Value); break;
		case EDebugFloatField::LightAmbientColorScaleR: Tuning.AmbientColorScale.R = FMath::Max(0.0f, Value); break;
		case EDebugFloatField::LightAmbientColorScaleG: Tuning.AmbientColorScale.G = FMath::Max(0.0f, Value); break;
		case EDebugFloatField::LightAmbientColorScaleB: Tuning.AmbientColorScale.B = FMath::Max(0.0f, Value); break;
		case EDebugFloatField::LightAttenuationRadiusScale: Tuning.AttenuationRadiusScale = FMath::Max(0.01f, Value); break;
		default: break;
		}
		GameMode->SetLoginSceneLightTuning(Tuning);
		return;
	}
	if (IsParticleField(Field))
	{
		const TArray<int32> Indices = GameMode->GetLoginParticleEmitterIndices();
		const int32 EmitterIndex = SelectedParticleEmitterIndex == INDEX_NONE && !Indices.IsEmpty() ? Indices[0] : SelectedParticleEmitterIndex;
		FWoWLoginParticleEmitterTuning Tuning = GameMode->GetLoginParticleEmitterTuning(EmitterIndex);
		switch (Field)
		{
		case EDebugFloatField::ParticlePositionX: Tuning.PositionOffset.X = Value; break;
		case EDebugFloatField::ParticlePositionY: Tuning.PositionOffset.Y = Value; break;
		case EDebugFloatField::ParticlePositionZ: Tuning.PositionOffset.Z = Value; break;
		case EDebugFloatField::ParticleEmissionRateScale: Tuning.EmissionRateScale = FMath::Max(0.0f, Value); break;
		case EDebugFloatField::ParticleEmissionSpeedScale: Tuning.EmissionSpeedScale = FMath::Max(0.0f, Value); break;
		case EDebugFloatField::ParticleLifespanScale: Tuning.LifespanScale = FMath::Max(0.01f, Value); break;
		case EDebugFloatField::ParticleAreaLengthScale: Tuning.AreaLengthScale = FMath::Max(0.0f, Value); break;
		case EDebugFloatField::ParticleAreaWidthScale: Tuning.AreaWidthScale = FMath::Max(0.0f, Value); break;
		case EDebugFloatField::ParticleGravityScale: Tuning.GravityScale = FMath::Max(0.0f, Value); break;
		case EDebugFloatField::ParticleGravity2Scale: Tuning.Gravity2Scale = FMath::Max(0.0f, Value); break;
		case EDebugFloatField::ParticleVerticalRangeScale: Tuning.VerticalRangeScale = FMath::Max(0.0f, Value); break;
		case EDebugFloatField::ParticleHorizontalRangeScale: Tuning.HorizontalRangeScale = FMath::Max(0.0f, Value); break;
		case EDebugFloatField::ParticleSizeScale: Tuning.SizeScale = FMath::Max(0.0f, Value); break;
		case EDebugFloatField::ParticleLifecycleSizeStartScale: Tuning.LifecycleSizeStartScale = FMath::Max(0.0f, Value); break;
		case EDebugFloatField::ParticleLifecycleSizeMidScale: Tuning.LifecycleSizeMidScale = FMath::Max(0.0f, Value); break;
		case EDebugFloatField::ParticleLifecycleSizeEndScale: Tuning.LifecycleSizeEndScale = FMath::Max(0.0f, Value); break;
		case EDebugFloatField::ParticleAlphaScale: Tuning.AlphaScale = FMath::Max(0.0f, Value); break;
		case EDebugFloatField::ParticleLifecycleAlphaStartScale: Tuning.LifecycleAlphaStartScale = FMath::Max(0.0f, Value); break;
		case EDebugFloatField::ParticleLifecycleAlphaMidScale: Tuning.LifecycleAlphaMidScale = FMath::Max(0.0f, Value); break;
		case EDebugFloatField::ParticleLifecycleAlphaEndScale: Tuning.LifecycleAlphaEndScale = FMath::Max(0.0f, Value); break;
		case EDebugFloatField::ParticleAlphaCutoff: Tuning.AlphaCutoff = FMath::Clamp(Value, 0.0f, 0.95f); break;
		case EDebugFloatField::ParticleBrightnessScale: Tuning.BrightnessScale = FMath::Max(0.0f, Value); break;
		default: break;
		}
		GameMode->SetLoginParticleEmitterTuning(Tuning);
		return;
	}
	if (IsTextureField(Field))
	{
		const TArray<int32> Indices = GetActiveTextureSectionIndices();
		const int32 SectionIndex = SelectedTextureSectionIndex == INDEX_NONE && !Indices.IsEmpty() ? Indices[0] : SelectedTextureSectionIndex;
		FWoWLoginTextureSectionTuning Tuning = GameMode->GetLoginTextureSectionTuning(SectionIndex);
		switch (Field)
		{
		case EDebugFloatField::TextureUVOffsetX: Tuning.UVOffset.X = Value; break;
		case EDebugFloatField::TextureUVOffsetY: Tuning.UVOffset.Y = Value; break;
		case EDebugFloatField::TextureUVScaleX: Tuning.UVScale.X = FMath::Max(0.001f, Value); break;
		case EDebugFloatField::TextureUVScaleY: Tuning.UVScale.Y = FMath::Max(0.001f, Value); break;
		case EDebugFloatField::TextureUVRotationDegrees: Tuning.UVRotationDegrees = Value; break;
		case EDebugFloatField::TextureAlphaScale: Tuning.AlphaScale = FMath::Max(0.0f, Value); break;
		case EDebugFloatField::TextureBrightnessScale: Tuning.BrightnessScale = FMath::Max(0.0f, Value); break;
		default: break;
		}
		GameMode->SetLoginTextureSectionTuning(Tuning);
		return;
	}

	switch (Field)
	{
	case EDebugFloatField::SceneLocationX: State.SceneLocationOffset.X = Value; break;
	case EDebugFloatField::SceneLocationY: State.SceneLocationOffset.Y = Value; break;
	case EDebugFloatField::SceneLocationZ: State.SceneLocationOffset.Z = Value; break;
	case EDebugFloatField::SceneRotationPitch: State.SceneRotationOffset.Pitch = Value; break;
	case EDebugFloatField::SceneRotationYaw: State.SceneRotationOffset.Yaw = Value; break;
	case EDebugFloatField::SceneRotationRoll: State.SceneRotationOffset.Roll = Value; break;
	case EDebugFloatField::SceneScaleX: State.SceneScaleMultiplier.X = FMath::Max(0.01f, Value); break;
	case EDebugFloatField::SceneScaleY: State.SceneScaleMultiplier.Y = FMath::Max(0.01f, Value); break;
	case EDebugFloatField::SceneScaleZ: State.SceneScaleMultiplier.Z = FMath::Max(0.01f, Value); break;
	case EDebugFloatField::CharacterPreviewLocationX: State.CharacterPreviewLocationOffset.X = Value; break;
	case EDebugFloatField::CharacterPreviewLocationY: State.CharacterPreviewLocationOffset.Y = Value; break;
	case EDebugFloatField::CharacterPreviewLocationZ: State.CharacterPreviewLocationOffset.Z = Value; break;
	case EDebugFloatField::CharacterPreviewRotationPitch: State.CharacterPreviewRotationOffset.Pitch = Value; break;
	case EDebugFloatField::CharacterPreviewRotationYaw: State.CharacterPreviewRotationOffset.Yaw = Value; break;
	case EDebugFloatField::CharacterPreviewRotationRoll: State.CharacterPreviewRotationOffset.Roll = Value; break;
	case EDebugFloatField::CharacterPreviewUniformScale: State.CharacterPreviewUniformScale = FMath::Max(0.01f, Value); break;
	case EDebugFloatField::CharacterPreviewScaleX: State.CharacterPreviewScaleMultiplier.X = FMath::Max(0.01f, Value); break;
	case EDebugFloatField::CharacterPreviewScaleY: State.CharacterPreviewScaleMultiplier.Y = FMath::Max(0.01f, Value); break;
	case EDebugFloatField::CharacterPreviewScaleZ: State.CharacterPreviewScaleMultiplier.Z = FMath::Max(0.01f, Value); break;
	case EDebugFloatField::GlobalBrightnessScale: State.GlobalBrightnessScale = FMath::Max(0.0f, Value); break;
	case EDebugFloatField::M2PhysicalLightIntensityScale: State.M2PhysicalLightIntensityScale = FMath::Max(0.0f, Value); break;
	case EDebugFloatField::CameraPositionX: Camera.Pos.X = Value; break;
	case EDebugFloatField::CameraPositionY: Camera.Pos.Y = Value; break;
	case EDebugFloatField::CameraPositionZ: Camera.Pos.Z = Value; break;
	case EDebugFloatField::CameraTargetX: Camera.Target.X = Value; break;
	case EDebugFloatField::CameraTargetY: Camera.Target.Y = Value; break;
	case EDebugFloatField::CameraTargetZ: Camera.Target.Z = Value; break;
	case EDebugFloatField::CameraFov: Camera.FovRadians = Value; break;
	case EDebugFloatField::CameraNearClipping: Camera.NearClipping = FMath::Max(0.001f, Value); break;
	case EDebugFloatField::CameraFarClipping: Camera.FarClipping = FMath::Max(0.001f, Value); break;
	case EDebugFloatField::CameraRollEffectX: Camera.RollRadians = Value; break;
	default: break;
	}

	if (IsCameraPositionField(Field) || IsCameraTargetField(Field) || Field == EDebugFloatField::CameraFov || Field == EDebugFloatField::CameraNearClipping || Field == EDebugFloatField::CameraFarClipping || Field == EDebugFloatField::CameraRollEffectX)
	{
		GameMode->SetLoginSceneM2CameraState(Camera);
	}
	else
	{
		GameMode->SetLoginSceneTuningState(State);
	}
}

void SWoWLoginSceneDebugPanel::ResetFieldValue(EDebugFloatField Field)
{
	AWoWLoginGameMode* GameMode = LoginGameMode.Get();
	if (!GameMode)
	{
		return;
	}

	FWoWLoginSceneTuningState State = GameMode->GetLoginSceneTuningState();
	FWoWLoginSceneM2CameraState Camera = GameMode->GetLoginSceneM2CameraState();
	const FWoWLoginSceneM2CameraState Base = GameMode->GetLoginSceneBaseM2CameraState();
	if (IsLightField(Field))
	{
		const TArray<AWoWLoginGameMode::FLoginSceneLight>& Lights = GameMode->GetLoginSceneLights();
		const int32 LightIndex = SelectedLightIndex == INDEX_NONE && !Lights.IsEmpty() ? Lights[0].Index : SelectedLightIndex;
		FWoWLoginLightTuning Tuning = GameMode->GetLoginSceneLightTuning(LightIndex);
		switch (Field)
		{
		case EDebugFloatField::LightDiffuseIntensityScale: Tuning.DiffuseIntensityScale = 1.0f; break;
		case EDebugFloatField::LightAmbientIntensityScale: Tuning.AmbientIntensityScale = 1.0f; break;
		case EDebugFloatField::LightDiffuseColorScaleR: Tuning.DiffuseColorScale.R = 1.0f; break;
		case EDebugFloatField::LightDiffuseColorScaleG: Tuning.DiffuseColorScale.G = 1.0f; break;
		case EDebugFloatField::LightDiffuseColorScaleB: Tuning.DiffuseColorScale.B = 1.0f; break;
		case EDebugFloatField::LightAmbientColorScaleR: Tuning.AmbientColorScale.R = 1.0f; break;
		case EDebugFloatField::LightAmbientColorScaleG: Tuning.AmbientColorScale.G = 1.0f; break;
		case EDebugFloatField::LightAmbientColorScaleB: Tuning.AmbientColorScale.B = 1.0f; break;
		case EDebugFloatField::LightAttenuationRadiusScale: Tuning.AttenuationRadiusScale = 1.0f; break;
		default: break;
		}
		GameMode->SetLoginSceneLightTuning(Tuning);
		return;
	}
	if (IsParticleField(Field))
	{
		const TArray<int32> Indices = GameMode->GetLoginParticleEmitterIndices();
		const int32 EmitterIndex = SelectedParticleEmitterIndex == INDEX_NONE && !Indices.IsEmpty() ? Indices[0] : SelectedParticleEmitterIndex;
		FWoWLoginParticleEmitterTuning Tuning = GameMode->GetLoginParticleEmitterTuning(EmitterIndex);
		switch (Field)
		{
		case EDebugFloatField::ParticlePositionX: Tuning.PositionOffset.X = 0.0f; break;
		case EDebugFloatField::ParticlePositionY: Tuning.PositionOffset.Y = 0.0f; break;
		case EDebugFloatField::ParticlePositionZ: Tuning.PositionOffset.Z = 0.0f; break;
		case EDebugFloatField::ParticleEmissionRateScale: Tuning.EmissionRateScale = 1.0f; break;
		case EDebugFloatField::ParticleEmissionSpeedScale: Tuning.EmissionSpeedScale = 1.0f; break;
		case EDebugFloatField::ParticleLifespanScale: Tuning.LifespanScale = 1.0f; break;
		case EDebugFloatField::ParticleAreaLengthScale: Tuning.AreaLengthScale = 1.0f; break;
		case EDebugFloatField::ParticleAreaWidthScale: Tuning.AreaWidthScale = 1.0f; break;
		case EDebugFloatField::ParticleGravityScale: Tuning.GravityScale = 1.0f; break;
		case EDebugFloatField::ParticleGravity2Scale: Tuning.Gravity2Scale = 1.0f; break;
		case EDebugFloatField::ParticleVerticalRangeScale: Tuning.VerticalRangeScale = 1.0f; break;
		case EDebugFloatField::ParticleHorizontalRangeScale: Tuning.HorizontalRangeScale = 1.0f; break;
		case EDebugFloatField::ParticleSizeScale: Tuning.SizeScale = 1.0f; break;
		case EDebugFloatField::ParticleLifecycleSizeStartScale: Tuning.LifecycleSizeStartScale = 1.0f; break;
		case EDebugFloatField::ParticleLifecycleSizeMidScale: Tuning.LifecycleSizeMidScale = 1.0f; break;
		case EDebugFloatField::ParticleLifecycleSizeEndScale: Tuning.LifecycleSizeEndScale = 1.0f; break;
		case EDebugFloatField::ParticleAlphaScale: Tuning.AlphaScale = 1.0f; break;
		case EDebugFloatField::ParticleLifecycleAlphaStartScale: Tuning.LifecycleAlphaStartScale = 1.0f; break;
		case EDebugFloatField::ParticleLifecycleAlphaMidScale: Tuning.LifecycleAlphaMidScale = 1.0f; break;
		case EDebugFloatField::ParticleLifecycleAlphaEndScale: Tuning.LifecycleAlphaEndScale = 1.0f; break;
		case EDebugFloatField::ParticleAlphaCutoff: Tuning.AlphaCutoff = 0.0f; break;
		case EDebugFloatField::ParticleBrightnessScale: Tuning.BrightnessScale = 1.0f; break;
		default: break;
		}
		GameMode->SetLoginParticleEmitterTuning(Tuning);
		return;
	}
	if (IsTextureField(Field))
	{
		const TArray<int32> Indices = GetActiveTextureSectionIndices();
		const int32 SectionIndex = SelectedTextureSectionIndex == INDEX_NONE && !Indices.IsEmpty() ? Indices[0] : SelectedTextureSectionIndex;
		FWoWLoginTextureSectionTuning Tuning = GameMode->GetLoginTextureSectionTuning(SectionIndex);
		switch (Field)
		{
		case EDebugFloatField::TextureUVOffsetX: Tuning.UVOffset.X = 0.0f; break;
		case EDebugFloatField::TextureUVOffsetY: Tuning.UVOffset.Y = 0.0f; break;
		case EDebugFloatField::TextureUVScaleX: Tuning.UVScale.X = 1.0f; break;
		case EDebugFloatField::TextureUVScaleY: Tuning.UVScale.Y = 1.0f; break;
		case EDebugFloatField::TextureUVRotationDegrees: Tuning.UVRotationDegrees = 0.0f; break;
		case EDebugFloatField::TextureAlphaScale: Tuning.AlphaScale = 1.0f; break;
		case EDebugFloatField::TextureBrightnessScale: Tuning.BrightnessScale = 1.0f; break;
		default: break;
		}
		GameMode->SetLoginTextureSectionTuning(Tuning);
		return;
	}

	switch (Field)
	{
	case EDebugFloatField::SceneLocationX: State.SceneLocationOffset.X = 0.0f; break;
	case EDebugFloatField::SceneLocationY: State.SceneLocationOffset.Y = 0.0f; break;
	case EDebugFloatField::SceneLocationZ: State.SceneLocationOffset.Z = 0.0f; break;
	case EDebugFloatField::SceneRotationPitch: State.SceneRotationOffset.Pitch = 0.0f; break;
	case EDebugFloatField::SceneRotationYaw: State.SceneRotationOffset.Yaw = 0.0f; break;
	case EDebugFloatField::SceneRotationRoll: State.SceneRotationOffset.Roll = 0.0f; break;
	case EDebugFloatField::SceneScaleX: State.SceneScaleMultiplier.X = 1.0f; break;
	case EDebugFloatField::SceneScaleY: State.SceneScaleMultiplier.Y = 1.0f; break;
	case EDebugFloatField::SceneScaleZ: State.SceneScaleMultiplier.Z = 1.0f; break;
	case EDebugFloatField::CharacterPreviewLocationX: State.CharacterPreviewLocationOffset.X = 0.0f; break;
	case EDebugFloatField::CharacterPreviewLocationY: State.CharacterPreviewLocationOffset.Y = 0.0f; break;
	case EDebugFloatField::CharacterPreviewLocationZ: State.CharacterPreviewLocationOffset.Z = 0.0f; break;
	case EDebugFloatField::CharacterPreviewRotationPitch: State.CharacterPreviewRotationOffset.Pitch = 0.0f; break;
	case EDebugFloatField::CharacterPreviewRotationYaw: State.CharacterPreviewRotationOffset.Yaw = 0.0f; break;
	case EDebugFloatField::CharacterPreviewRotationRoll: State.CharacterPreviewRotationOffset.Roll = 0.0f; break;
	case EDebugFloatField::CharacterPreviewUniformScale: State.CharacterPreviewUniformScale = 1.0f; break;
	case EDebugFloatField::CharacterPreviewScaleX: State.CharacterPreviewScaleMultiplier.X = 1.0f; break;
	case EDebugFloatField::CharacterPreviewScaleY: State.CharacterPreviewScaleMultiplier.Y = 1.0f; break;
	case EDebugFloatField::CharacterPreviewScaleZ: State.CharacterPreviewScaleMultiplier.Z = 1.0f; break;
	case EDebugFloatField::GlobalBrightnessScale: State.GlobalBrightnessScale = 1.0f; break;
	case EDebugFloatField::M2PhysicalLightIntensityScale: State.M2PhysicalLightIntensityScale = 80.0f; break;
	case EDebugFloatField::CameraPositionX: Camera.Pos.X = Base.Pos.X; break;
	case EDebugFloatField::CameraPositionY: Camera.Pos.Y = Base.Pos.Y; break;
	case EDebugFloatField::CameraPositionZ: Camera.Pos.Z = Base.Pos.Z; break;
	case EDebugFloatField::CameraTargetX: Camera.Target.X = Base.Target.X; break;
	case EDebugFloatField::CameraTargetY: Camera.Target.Y = Base.Target.Y; break;
	case EDebugFloatField::CameraTargetZ: Camera.Target.Z = Base.Target.Z; break;
	case EDebugFloatField::CameraFov: Camera.FovRadians = Base.FovRadians; break;
	case EDebugFloatField::CameraNearClipping: Camera.NearClipping = Base.NearClipping; break;
	case EDebugFloatField::CameraFarClipping: Camera.FarClipping = Base.FarClipping; break;
	case EDebugFloatField::CameraRollEffectX: Camera.RollRadians = Base.RollRadians; break;
	default: break;
	}

	if (IsCameraPositionField(Field) || IsCameraTargetField(Field) || Field == EDebugFloatField::CameraFov || Field == EDebugFloatField::CameraNearClipping || Field == EDebugFloatField::CameraFarClipping || Field == EDebugFloatField::CameraRollEffectX)
	{
		GameMode->SetLoginSceneM2CameraState(Camera);
	}
	else
	{
		GameMode->SetLoginSceneTuningState(State);
	}
}

void SWoWLoginSceneDebugPanel::OnSliderChanged(float Value, EDebugFloatField Field)
{
	AWoWLoginGameMode* GameMode = LoginGameMode.Get();
	if (!GameMode)
	{
		return;
	}

	FWoWLoginSceneTuningState State = GameMode->GetLoginSceneTuningState();
	FWoWLoginSceneM2CameraState Camera = GameMode->GetLoginSceneM2CameraState();
	const FWoWLoginSceneM2CameraState Base = GameMode->GetLoginSceneBaseM2CameraState();
	const float Range = GetFieldRange(Field);
	const float RawValue = (Value * 2.0f - 1.0f) * Range;
	if (IsLightField(Field))
	{
		const TArray<AWoWLoginGameMode::FLoginSceneLight>& Lights = GameMode->GetLoginSceneLights();
		const int32 LightIndex = SelectedLightIndex == INDEX_NONE && !Lights.IsEmpty() ? Lights[0].Index : SelectedLightIndex;
		FWoWLoginLightTuning Tuning = GameMode->GetLoginSceneLightTuning(LightIndex);
		switch (Field)
		{
		case EDebugFloatField::LightDiffuseIntensityScale: Tuning.DiffuseIntensityScale = FMath::Max(0.0f, 1.0f + RawValue); break;
		case EDebugFloatField::LightAmbientIntensityScale: Tuning.AmbientIntensityScale = FMath::Max(0.0f, 1.0f + RawValue); break;
		case EDebugFloatField::LightDiffuseColorScaleR: Tuning.DiffuseColorScale.R = FMath::Max(0.0f, 1.0f + RawValue); break;
		case EDebugFloatField::LightDiffuseColorScaleG: Tuning.DiffuseColorScale.G = FMath::Max(0.0f, 1.0f + RawValue); break;
		case EDebugFloatField::LightDiffuseColorScaleB: Tuning.DiffuseColorScale.B = FMath::Max(0.0f, 1.0f + RawValue); break;
		case EDebugFloatField::LightAmbientColorScaleR: Tuning.AmbientColorScale.R = FMath::Max(0.0f, 1.0f + RawValue); break;
		case EDebugFloatField::LightAmbientColorScaleG: Tuning.AmbientColorScale.G = FMath::Max(0.0f, 1.0f + RawValue); break;
		case EDebugFloatField::LightAmbientColorScaleB: Tuning.AmbientColorScale.B = FMath::Max(0.0f, 1.0f + RawValue); break;
		case EDebugFloatField::LightAttenuationRadiusScale: Tuning.AttenuationRadiusScale = FMath::Max(0.01f, 1.0f + RawValue); break;
		default: break;
		}
		GameMode->SetLoginSceneLightTuning(Tuning);
		return;
	}
	if (IsParticleField(Field))
	{
		const TArray<int32> Indices = GameMode->GetLoginParticleEmitterIndices();
		const int32 EmitterIndex = SelectedParticleEmitterIndex == INDEX_NONE && !Indices.IsEmpty() ? Indices[0] : SelectedParticleEmitterIndex;
		FWoWLoginParticleEmitterTuning Tuning = GameMode->GetLoginParticleEmitterTuning(EmitterIndex);
		switch (Field)
		{
		case EDebugFloatField::ParticlePositionX: Tuning.PositionOffset.X = RawValue; break;
		case EDebugFloatField::ParticlePositionY: Tuning.PositionOffset.Y = RawValue; break;
		case EDebugFloatField::ParticlePositionZ: Tuning.PositionOffset.Z = RawValue; break;
		case EDebugFloatField::ParticleEmissionRateScale: Tuning.EmissionRateScale = FMath::Max(0.0f, 1.0f + RawValue); break;
		case EDebugFloatField::ParticleEmissionSpeedScale: Tuning.EmissionSpeedScale = FMath::Max(0.0f, 1.0f + RawValue); break;
		case EDebugFloatField::ParticleLifespanScale: Tuning.LifespanScale = FMath::Max(0.01f, 1.0f + RawValue); break;
		case EDebugFloatField::ParticleAreaLengthScale: Tuning.AreaLengthScale = FMath::Max(0.0f, 1.0f + RawValue); break;
		case EDebugFloatField::ParticleAreaWidthScale: Tuning.AreaWidthScale = FMath::Max(0.0f, 1.0f + RawValue); break;
		case EDebugFloatField::ParticleGravityScale: Tuning.GravityScale = FMath::Max(0.0f, 1.0f + RawValue); break;
		case EDebugFloatField::ParticleGravity2Scale: Tuning.Gravity2Scale = FMath::Max(0.0f, 1.0f + RawValue); break;
		case EDebugFloatField::ParticleVerticalRangeScale: Tuning.VerticalRangeScale = FMath::Max(0.0f, 1.0f + RawValue); break;
		case EDebugFloatField::ParticleHorizontalRangeScale: Tuning.HorizontalRangeScale = FMath::Max(0.0f, 1.0f + RawValue); break;
		case EDebugFloatField::ParticleSizeScale: Tuning.SizeScale = FMath::Max(0.0f, 1.0f + RawValue); break;
		case EDebugFloatField::ParticleLifecycleSizeStartScale: Tuning.LifecycleSizeStartScale = FMath::Max(0.0f, 1.0f + RawValue); break;
		case EDebugFloatField::ParticleLifecycleSizeMidScale: Tuning.LifecycleSizeMidScale = FMath::Max(0.0f, 1.0f + RawValue); break;
		case EDebugFloatField::ParticleLifecycleSizeEndScale: Tuning.LifecycleSizeEndScale = FMath::Max(0.0f, 1.0f + RawValue); break;
		case EDebugFloatField::ParticleAlphaScale: Tuning.AlphaScale = FMath::Max(0.0f, 1.0f + RawValue); break;
		case EDebugFloatField::ParticleLifecycleAlphaStartScale: Tuning.LifecycleAlphaStartScale = FMath::Max(0.0f, 1.0f + RawValue); break;
		case EDebugFloatField::ParticleLifecycleAlphaMidScale: Tuning.LifecycleAlphaMidScale = FMath::Max(0.0f, 1.0f + RawValue); break;
		case EDebugFloatField::ParticleLifecycleAlphaEndScale: Tuning.LifecycleAlphaEndScale = FMath::Max(0.0f, 1.0f + RawValue); break;
		case EDebugFloatField::ParticleAlphaCutoff: Tuning.AlphaCutoff = FMath::Clamp(RawValue, 0.0f, 0.95f); break;
		case EDebugFloatField::ParticleBrightnessScale: Tuning.BrightnessScale = FMath::Max(0.0f, 1.0f + RawValue); break;
		default: break;
		}
		GameMode->SetLoginParticleEmitterTuning(Tuning);
		return;
	}
	if (IsTextureField(Field))
	{
		const TArray<int32> Indices = GetActiveTextureSectionIndices();
		const int32 SectionIndex = SelectedTextureSectionIndex == INDEX_NONE && !Indices.IsEmpty() ? Indices[0] : SelectedTextureSectionIndex;
		FWoWLoginTextureSectionTuning Tuning = GameMode->GetLoginTextureSectionTuning(SectionIndex);
		switch (Field)
		{
		case EDebugFloatField::TextureUVOffsetX: Tuning.UVOffset.X = RawValue; break;
		case EDebugFloatField::TextureUVOffsetY: Tuning.UVOffset.Y = RawValue; break;
		case EDebugFloatField::TextureUVScaleX: Tuning.UVScale.X = FMath::Max(0.001f, 1.0f + RawValue); break;
		case EDebugFloatField::TextureUVScaleY: Tuning.UVScale.Y = FMath::Max(0.001f, 1.0f + RawValue); break;
		case EDebugFloatField::TextureUVRotationDegrees: Tuning.UVRotationDegrees = RawValue; break;
		case EDebugFloatField::TextureAlphaScale: Tuning.AlphaScale = FMath::Max(0.0f, 1.0f + RawValue); break;
		case EDebugFloatField::TextureBrightnessScale: Tuning.BrightnessScale = FMath::Max(0.0f, 1.0f + RawValue); break;
		default: break;
		}
		GameMode->SetLoginTextureSectionTuning(Tuning);
		return;
	}

	switch (Field)
	{
	case EDebugFloatField::SceneLocationX: State.SceneLocationOffset.X = RawValue; break;
	case EDebugFloatField::SceneLocationY: State.SceneLocationOffset.Y = RawValue; break;
	case EDebugFloatField::SceneLocationZ: State.SceneLocationOffset.Z = RawValue; break;
	case EDebugFloatField::SceneRotationPitch: State.SceneRotationOffset.Pitch = RawValue; break;
	case EDebugFloatField::SceneRotationYaw: State.SceneRotationOffset.Yaw = RawValue; break;
	case EDebugFloatField::SceneRotationRoll: State.SceneRotationOffset.Roll = RawValue; break;
	case EDebugFloatField::SceneScaleX: State.SceneScaleMultiplier.X = FMath::Max(0.01f, 1.0f + RawValue); break;
	case EDebugFloatField::SceneScaleY: State.SceneScaleMultiplier.Y = FMath::Max(0.01f, 1.0f + RawValue); break;
	case EDebugFloatField::SceneScaleZ: State.SceneScaleMultiplier.Z = FMath::Max(0.01f, 1.0f + RawValue); break;
	case EDebugFloatField::CharacterPreviewLocationX: State.CharacterPreviewLocationOffset.X = RawValue; break;
	case EDebugFloatField::CharacterPreviewLocationY: State.CharacterPreviewLocationOffset.Y = RawValue; break;
	case EDebugFloatField::CharacterPreviewLocationZ: State.CharacterPreviewLocationOffset.Z = RawValue; break;
	case EDebugFloatField::CharacterPreviewRotationPitch: State.CharacterPreviewRotationOffset.Pitch = RawValue; break;
	case EDebugFloatField::CharacterPreviewRotationYaw: State.CharacterPreviewRotationOffset.Yaw = RawValue; break;
	case EDebugFloatField::CharacterPreviewRotationRoll: State.CharacterPreviewRotationOffset.Roll = RawValue; break;
	case EDebugFloatField::CharacterPreviewUniformScale: State.CharacterPreviewUniformScale = FMath::Max(0.01f, 1.0f + RawValue); break;
	case EDebugFloatField::CharacterPreviewScaleX: State.CharacterPreviewScaleMultiplier.X = FMath::Max(0.01f, 1.0f + RawValue); break;
	case EDebugFloatField::CharacterPreviewScaleY: State.CharacterPreviewScaleMultiplier.Y = FMath::Max(0.01f, 1.0f + RawValue); break;
	case EDebugFloatField::CharacterPreviewScaleZ: State.CharacterPreviewScaleMultiplier.Z = FMath::Max(0.01f, 1.0f + RawValue); break;
	case EDebugFloatField::GlobalBrightnessScale: State.GlobalBrightnessScale = FMath::Max(0.0f, 1.0f + RawValue); break;
	case EDebugFloatField::M2PhysicalLightIntensityScale: State.M2PhysicalLightIntensityScale = FMath::Max(0.0f, 80.0f + RawValue); break;
	case EDebugFloatField::CameraPositionX: Camera.Pos.X = Base.Pos.X + RawValue; break;
	case EDebugFloatField::CameraPositionY: Camera.Pos.Y = Base.Pos.Y + RawValue; break;
	case EDebugFloatField::CameraPositionZ: Camera.Pos.Z = Base.Pos.Z + RawValue; break;
	case EDebugFloatField::CameraTargetX: Camera.Target.X = Base.Target.X + RawValue; break;
	case EDebugFloatField::CameraTargetY: Camera.Target.Y = Base.Target.Y + RawValue; break;
	case EDebugFloatField::CameraTargetZ: Camera.Target.Z = Base.Target.Z + RawValue; break;
	case EDebugFloatField::CameraFov: Camera.FovRadians = Base.FovRadians + RawValue; break;
	case EDebugFloatField::CameraNearClipping: Camera.NearClipping = Base.NearClipping + RawValue; break;
	case EDebugFloatField::CameraFarClipping: Camera.FarClipping = Base.FarClipping + RawValue; break;
	case EDebugFloatField::CameraRollEffectX: Camera.RollRadians = Base.RollRadians + RawValue; break;
	default: break;
	}

	if (IsCameraPositionField(Field) || IsCameraTargetField(Field) || Field == EDebugFloatField::CameraFov || Field == EDebugFloatField::CameraNearClipping || Field == EDebugFloatField::CameraFarClipping || Field == EDebugFloatField::CameraRollEffectX)
	{
		GameMode->SetLoginSceneM2CameraState(Camera);
	}
	else
	{
		GameMode->SetLoginSceneTuningState(State);
	}
}

void SWoWLoginSceneDebugPanel::OnSliderWheelDelta(float WheelDelta, EDebugFloatField Field)
{
	const TOptional<float> CurrentValue = GetFieldNumericValue(Field);
	if (!CurrentValue.IsSet() || FMath::IsNearlyZero(WheelDelta))
	{
		return;
	}

	const float Direction = WheelDelta > 0.0f ? 1.0f : -1.0f;
	SetFieldDisplayValue(CurrentValue.GetValue() + Direction * GetFieldWheelStep(Field), Field);
}

void SWoWLoginSceneDebugPanel::OnFieldNumericCommitted(float Value, ETextCommit::Type CommitType, EDebugFloatField Field)
{
	SetFieldDisplayValue(Value, Field);
}

int32 SWoWLoginSceneDebugPanel::GetActiveTabIndex() const
{
	switch (ActiveTab)
	{
	case EDebugTab::Scene: return 0;
	case EDebugTab::Camera: return 1;
	case EDebugTab::Lighting: return 2;
	case EDebugTab::Particles: return 3;
	case EDebugTab::Textures: return 4;
	case EDebugTab::Bones: return 5;
	case EDebugTab::Character: return 6;
	case EDebugTab::GlueLog: return 7;
	default: return 0;
	}
}

FReply SWoWLoginSceneDebugPanel::OnCopyParamsClicked()
{
	if (const AWoWLoginGameMode* GameMode = LoginGameMode.Get())
	{
		const FString Text =
			GameMode->ExportLoginSceneBaseCameraState() + TEXT("\r\n") +
			GameMode->ExportLoginSceneFinalCameraState() + TEXT("\r\n") +
			GameMode->ExportLoginSceneTuningState();
		FPlatformApplicationMisc::ClipboardCopy(*Text);
	}
	return FReply::Handled();
}

FReply SWoWLoginSceneDebugPanel::OnCopyCharacterDebugClicked()
{
	const FString Text = GetCharacterPreviewDebugText().ToString();
	FPlatformApplicationMisc::ClipboardCopy(*Text);
	return FReply::Handled();
}

FReply SWoWLoginSceneDebugPanel::OnExportCharacterDebugLogClicked()
{
	const AWoWLoginGameMode* GameMode = LoginGameMode.Get();
	if (!GameMode)
	{
		return FReply::Handled();
	}

	const AWoWLoginGameMode::FCharacterSelectPreviewDebugState State = GameMode->GetCharacterSelectPreviewDebugState();
	const FString LogDirectory = FPaths::ProjectSavedDir() / TEXT("Logs/WoWRuntime/CharacterPreview");
	IFileManager::Get().MakeDirectory(*LogDirectory, true);

	const FString Timestamp = FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S"));
	const FString FileName = FString::Printf(
		TEXT("CharacterPreview_%s_guid%d_race%d_gender%d.log"),
		*Timestamp,
		State.SelectedGuid,
		State.Race,
		State.Gender);
	const FString ExportedLogPath = LogDirectory / FileName;
	const FString Text = GetCharacterPreviewDebugText().ToString();
	if (FFileHelper::SaveStringToFile(Text, *ExportedLogPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		FPlatformApplicationMisc::ClipboardCopy(*ExportedLogPath);
		UE_LOG(LogTemp, Display, TEXT("角色预览调试日志已导出: %s"), *ExportedLogPath);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("角色预览调试日志导出失败: %s"), *ExportedLogPath);
	}

	return FReply::Handled();
}

FReply SWoWLoginSceneDebugPanel::OnSaveAssetClicked()
{
	if (AWoWLoginGameMode* GameMode = LoginGameMode.Get())
	{
		GameMode->SaveLoginSceneTuningAsset();
	}
	return FReply::Handled();
}

FReply SWoWLoginSceneDebugPanel::OnBakeScenePresetClicked()
{
	if (AWoWLoginGameMode* GameMode = LoginGameMode.Get())
	{
		FString StatusMessage;
		GameMode->SaveLoginScenePresetAsset(&StatusMessage);
		PresetBakeStatusText = StatusMessage;
	}
	return FReply::Handled();
}

FReply SWoWLoginSceneDebugPanel::OnBakeAllScenePresetsClicked()
{
	if (AWoWLoginGameMode* GameMode = LoginGameMode.Get())
	{
		FString StatusMessage;
		GameMode->SaveAllLoginScenePresetAssets(&StatusMessage);
		PresetBakeStatusText = StatusMessage;
	}
	return FReply::Handled();
}

FReply SWoWLoginSceneDebugPanel::OnClearGlueDebugLogClicked()
{
	if (AWoWLoginGameMode* GameMode = LoginGameMode.Get())
	{
		GameMode->ClearGlueDebugLog();
	}
	return FReply::Handled();
}

void SWoWLoginSceneDebugPanel::OnCharacterPreviewAnimationOverrideCommitted(const FText& Text, ETextCommit::Type CommitType)
{
	if (AWoWLoginGameMode* GameMode = LoginGameMode.Get())
	{
		GameMode->SetCharacterPreviewAnimationOverridePath(Text.ToString());
	}
}

FReply SWoWLoginSceneDebugPanel::OnCloseClicked()
{
	if (OnCloseRequested.IsBound())
	{
		OnCloseRequested.Execute();
	}
	return FReply::Handled();
}

FReply SWoWLoginSceneDebugPanel::OnTabClicked(EDebugTab NewTab)
{
	ActiveTab = NewTab;
	bShowRawDetails = false;
	if (AWoWLoginGameMode* GameMode = LoginGameMode.Get())
	{
		GameMode->SetLoginTextureDebugHighlightSection(INDEX_NONE);
		if (ActiveTab == EDebugTab::Bones)
		{
			const TArray<int32> EmitterIndices = GameMode->GetLoginParticleEmitterIndices();
			const int32 EmitterIndex = SelectedParticleEmitterIndex == INDEX_NONE && !EmitterIndices.IsEmpty() ? EmitterIndices[0] : SelectedParticleEmitterIndex;
			if (const AWoWLoginGameMode::FLoginParticleEmitter* Emitter = GameMode->GetLoginParticleEmitterByIndex(EmitterIndex))
			{
				SelectedBoneIndex = Emitter->BoneIndex;
			}
		}
	}
	return FReply::Handled();
}

void SWoWLoginSceneDebugPanel::OnNoteCommitted(const FText& Text, ETextCommit::Type CommitType)
{
	AWoWLoginGameMode* GameMode = LoginGameMode.Get();
	if (!GameMode)
	{
		return;
	}

	const FString Note = Text.ToString();
	switch (ActiveTab)
	{
	case EDebugTab::Lighting:
	{
		const TArray<AWoWLoginGameMode::FLoginSceneLight>& Lights = GameMode->GetLoginSceneLights();
		const int32 LightIndex = SelectedLightIndex == INDEX_NONE && !Lights.IsEmpty() ? Lights[0].Index : SelectedLightIndex;
		if (LightIndex == INDEX_NONE)
		{
			return;
		}
		FWoWLoginLightTuning Tuning = GameMode->GetLoginSceneLightTuning(LightIndex);
		Tuning.Note = Note;
		GameMode->SetLoginSceneLightTuning(Tuning);
		break;
	}
	case EDebugTab::Particles:
	{
		const TArray<int32> Indices = GameMode->GetLoginParticleEmitterIndices();
		const int32 EmitterIndex = SelectedParticleEmitterIndex == INDEX_NONE && !Indices.IsEmpty() ? Indices[0] : SelectedParticleEmitterIndex;
		if (EmitterIndex == INDEX_NONE)
		{
			return;
		}
		FWoWLoginParticleEmitterTuning Tuning = GameMode->GetLoginParticleEmitterTuning(EmitterIndex);
		Tuning.Note = Note;
		GameMode->SetLoginParticleEmitterTuning(Tuning);
		break;
	}
	case EDebugTab::Textures:
	{
		const TArray<int32> Indices = GetActiveTextureSectionIndices();
		const int32 SectionIndex = SelectedTextureSectionIndex == INDEX_NONE && !Indices.IsEmpty() ? Indices[0] : SelectedTextureSectionIndex;
		if (SectionIndex == INDEX_NONE)
		{
			return;
		}
		FWoWLoginTextureSectionTuning Tuning = GameMode->GetLoginTextureSectionTuning(SectionIndex);
		Tuning.Note = Note;
		GameMode->SetLoginTextureSectionTuning(Tuning);
		break;
	}
	default:
		break;
	}
}

FReply SWoWLoginSceneDebugPanel::OnToggleRawDetailsClicked()
{
	bShowRawDetails = !bShowRawDetails;
	return FReply::Handled();
}

FReply SWoWLoginSceneDebugPanel::OnResetFieldClicked(EDebugFloatField Field)
{
	ResetFieldValue(Field);
	return FReply::Handled();
}

FReply SWoWLoginSceneDebugPanel::OnResetCurrentClicked()
{
	if (AWoWLoginGameMode* GameMode = LoginGameMode.Get())
	{
		switch (ActiveTab)
		{
		case EDebugTab::Lighting:
		{
			const TArray<AWoWLoginGameMode::FLoginSceneLight>& Lights = GameMode->GetLoginSceneLights();
			const int32 LightIndex = SelectedLightIndex == INDEX_NONE && !Lights.IsEmpty() ? Lights[0].Index : SelectedLightIndex;
			GameMode->ResetLoginSceneLightTuning(LightIndex);
			break;
		}
		case EDebugTab::Particles:
		{
			const TArray<int32> Indices = GameMode->GetLoginParticleEmitterIndices();
			const int32 EmitterIndex = SelectedParticleEmitterIndex == INDEX_NONE && !Indices.IsEmpty() ? Indices[0] : SelectedParticleEmitterIndex;
			GameMode->ResetLoginParticleEmitterTuning(EmitterIndex);
			break;
		}
		case EDebugTab::Textures:
		{
			const TArray<int32> Indices = GetActiveTextureSectionIndices();
			const int32 SectionIndex = SelectedTextureSectionIndex == INDEX_NONE && !Indices.IsEmpty() ? Indices[0] : SelectedTextureSectionIndex;
			GameMode->ResetLoginTextureSectionTuning(SectionIndex);
			break;
		}
		default:
			OnResetAllInTabClicked();
			break;
		}
	}
	return FReply::Handled();
}

FReply SWoWLoginSceneDebugPanel::OnResetAllInTabClicked()
{
	if (AWoWLoginGameMode* GameMode = LoginGameMode.Get())
	{
		FWoWLoginSceneTuningState State = GameMode->GetLoginSceneTuningState();
		switch (ActiveTab)
		{
		case EDebugTab::Scene:
			State.SceneLocationOffset = FVector::ZeroVector;
			State.SceneRotationOffset = FRotator::ZeroRotator;
			State.SceneScaleMultiplier = FVector::OneVector;
			GameMode->SetLoginSceneTuningState(State);
			break;
		case EDebugTab::Camera:
		{
			FWoWLoginSceneM2CameraState Camera = GameMode->GetLoginSceneBaseM2CameraState();
			GameMode->SetLoginSceneM2CameraState(Camera);
			break;
		}
		case EDebugTab::Lighting:
			GameMode->ResetAllLoginSceneLightTunings();
			break;
		case EDebugTab::Particles:
			GameMode->ResetAllLoginParticleEmitterTunings();
			break;
		case EDebugTab::Textures:
			GameMode->ResetAllLoginTextureSectionTunings();
			break;
		case EDebugTab::Character:
			State.CharacterPreviewLocationOffset = FVector::ZeroVector;
			State.CharacterPreviewRotationOffset = FRotator::ZeroRotator;
			State.CharacterPreviewScaleMultiplier = FVector::OneVector;
			State.CharacterPreviewUniformScale = 1.0f;
			State.CharacterPreviewAnimationOverrideId = INDEX_NONE;
			State.CharacterPreviewAnimationOverridePath.Empty();
			GameMode->SetLoginSceneTuningState(State);
			break;
		default:
			break;
		}
	}
	return FReply::Handled();
}

FReply SWoWLoginSceneDebugPanel::OnPrevParticleClicked()
{
	if (const AWoWLoginGameMode* GameMode = LoginGameMode.Get())
	{
		const TArray<int32> Indices = GameMode->GetLoginParticleEmitterIndices();
		if (!Indices.IsEmpty())
		{
			int32 CurrentArrayIndex = Indices.IndexOfByKey(SelectedParticleEmitterIndex);
			if (CurrentArrayIndex == INDEX_NONE)
			{
				CurrentArrayIndex = 0;
			}
			SelectedParticleEmitterIndex = Indices[(CurrentArrayIndex - 1 + Indices.Num()) % Indices.Num()];
			if (const AWoWLoginGameMode::FLoginParticleEmitter* Emitter = GameMode->GetLoginParticleEmitterByIndex(SelectedParticleEmitterIndex))
			{
				SelectedBoneIndex = Emitter->BoneIndex;
			}
		}
	}
	return FReply::Handled();
}

FReply SWoWLoginSceneDebugPanel::OnNextParticleClicked()
{
	if (const AWoWLoginGameMode* GameMode = LoginGameMode.Get())
	{
		const TArray<int32> Indices = GameMode->GetLoginParticleEmitterIndices();
		if (!Indices.IsEmpty())
		{
			int32 CurrentArrayIndex = Indices.IndexOfByKey(SelectedParticleEmitterIndex);
			if (CurrentArrayIndex == INDEX_NONE)
			{
				CurrentArrayIndex = -1;
			}
			SelectedParticleEmitterIndex = Indices[(CurrentArrayIndex + 1) % Indices.Num()];
			if (const AWoWLoginGameMode::FLoginParticleEmitter* Emitter = GameMode->GetLoginParticleEmitterByIndex(SelectedParticleEmitterIndex))
			{
				SelectedBoneIndex = Emitter->BoneIndex;
			}
		}
	}
	return FReply::Handled();
}

FReply SWoWLoginSceneDebugPanel::OnToggleParticleEnabledClicked()
{
	if (AWoWLoginGameMode* GameMode = LoginGameMode.Get())
	{
		const TArray<int32> Indices = GameMode->GetLoginParticleEmitterIndices();
		const int32 EmitterIndex = SelectedParticleEmitterIndex == INDEX_NONE && !Indices.IsEmpty() ? Indices[0] : SelectedParticleEmitterIndex;
		FWoWLoginParticleEmitterTuning Tuning = GameMode->GetLoginParticleEmitterTuning(EmitterIndex);
		Tuning.bEnabled = !Tuning.bEnabled;
		GameMode->SetLoginParticleEmitterTuning(Tuning);
	}
	return FReply::Handled();
}

FReply SWoWLoginSceneDebugPanel::OnClearParticlePositionOffsetClicked()
{
	if (AWoWLoginGameMode* GameMode = LoginGameMode.Get())
	{
		const TArray<int32> Indices = GameMode->GetLoginParticleEmitterIndices();
		const int32 EmitterIndex = SelectedParticleEmitterIndex == INDEX_NONE && !Indices.IsEmpty() ? Indices[0] : SelectedParticleEmitterIndex;
		if (EmitterIndex == INDEX_NONE)
		{
			return FReply::Handled();
		}

		FWoWLoginParticleEmitterTuning Tuning = GameMode->GetLoginParticleEmitterTuning(EmitterIndex);
		Tuning.PositionOffset = FVector::ZeroVector;
		GameMode->SetLoginParticleEmitterTuning(Tuning);
	}
	return FReply::Handled();
}

FReply SWoWLoginSceneDebugPanel::OnPrevTextureClicked()
{
	if (const AWoWLoginGameMode* GameMode = LoginGameMode.Get())
	{
		const TArray<int32> Indices = GetActiveTextureSectionIndices();
		if (!Indices.IsEmpty())
		{
			int32 CurrentArrayIndex = Indices.IndexOfByKey(SelectedTextureSectionIndex);
			if (CurrentArrayIndex == INDEX_NONE)
			{
				CurrentArrayIndex = 0;
			}
			SelectedTextureSectionIndex = Indices[(CurrentArrayIndex - 1 + Indices.Num()) % Indices.Num()];
		}
	}
	return FReply::Handled();
}

FReply SWoWLoginSceneDebugPanel::OnNextTextureClicked()
{
	if (const AWoWLoginGameMode* GameMode = LoginGameMode.Get())
	{
		const TArray<int32> Indices = GetActiveTextureSectionIndices();
		if (!Indices.IsEmpty())
		{
			int32 CurrentArrayIndex = Indices.IndexOfByKey(SelectedTextureSectionIndex);
			if (CurrentArrayIndex == INDEX_NONE)
			{
				CurrentArrayIndex = -1;
			}
			SelectedTextureSectionIndex = Indices[(CurrentArrayIndex + 1) % Indices.Num()];
		}
	}
	return FReply::Handled();
}

FReply SWoWLoginSceneDebugPanel::OnToggleTextureM2AnimationClicked()
{
	if (AWoWLoginGameMode* GameMode = LoginGameMode.Get())
	{
		const TArray<int32> Indices = GetActiveTextureSectionIndices();
		const int32 SectionIndex = SelectedTextureSectionIndex == INDEX_NONE && !Indices.IsEmpty() ? Indices[0] : SelectedTextureSectionIndex;
		FWoWLoginTextureSectionTuning Tuning = GameMode->GetLoginTextureSectionTuning(SectionIndex);
		Tuning.bUseM2TextureAnimation = !Tuning.bUseM2TextureAnimation;
		GameMode->SetLoginTextureSectionTuning(Tuning);
	}
	return FReply::Handled();
}

FReply SWoWLoginSceneDebugPanel::OnToggleTextureEnabledClicked()
{
	if (AWoWLoginGameMode* GameMode = LoginGameMode.Get())
	{
		const TArray<int32> Indices = GetActiveTextureSectionIndices();
		const int32 SectionIndex = SelectedTextureSectionIndex == INDEX_NONE && !Indices.IsEmpty() ? Indices[0] : SelectedTextureSectionIndex;
		FWoWLoginTextureSectionTuning Tuning = GameMode->GetLoginTextureSectionTuning(SectionIndex);
		Tuning.bEnabled = !Tuning.bEnabled;
		GameMode->SetLoginTextureSectionTuning(Tuning);
	}
	return FReply::Handled();
}

FReply SWoWLoginSceneDebugPanel::OnToggleTextureForceVisibleClicked()
{
	if (AWoWLoginGameMode* GameMode = LoginGameMode.Get())
	{
		const TArray<int32> Indices = GetActiveTextureSectionIndices();
		const int32 SectionIndex = SelectedTextureSectionIndex == INDEX_NONE && !Indices.IsEmpty() ? Indices[0] : SelectedTextureSectionIndex;
		FWoWLoginTextureSectionTuning Tuning = GameMode->GetLoginTextureSectionTuning(SectionIndex);
		Tuning.bForceVisibleDebug = !Tuning.bForceVisibleDebug;
		if (Tuning.bForceVisibleDebug)
		{
			Tuning.AlphaScale = FMath::Max(Tuning.AlphaScale, 8.0f);
			Tuning.BrightnessScale = FMath::Max(Tuning.BrightnessScale, 12.0f);
		}
		GameMode->SetLoginTextureSectionTuning(Tuning);
	}
	return FReply::Handled();
}

FReply SWoWLoginSceneDebugPanel::OnPrevLightClicked()
{
	if (const AWoWLoginGameMode* GameMode = LoginGameMode.Get())
	{
		const TArray<AWoWLoginGameMode::FLoginSceneLight>& Lights = GameMode->GetLoginSceneLights();
		if (!Lights.IsEmpty())
		{
			int32 CurrentArrayIndex = Lights.IndexOfByPredicate([this](const AWoWLoginGameMode::FLoginSceneLight& Light)
			{
				return Light.Index == SelectedLightIndex;
			});
			if (CurrentArrayIndex == INDEX_NONE)
			{
				CurrentArrayIndex = 0;
			}
			SelectedLightIndex = Lights[(CurrentArrayIndex - 1 + Lights.Num()) % Lights.Num()].Index;
		}
	}
	return FReply::Handled();
}

FReply SWoWLoginSceneDebugPanel::OnNextLightClicked()
{
	if (const AWoWLoginGameMode* GameMode = LoginGameMode.Get())
	{
		const TArray<AWoWLoginGameMode::FLoginSceneLight>& Lights = GameMode->GetLoginSceneLights();
		if (!Lights.IsEmpty())
		{
			int32 CurrentArrayIndex = Lights.IndexOfByPredicate([this](const AWoWLoginGameMode::FLoginSceneLight& Light)
			{
				return Light.Index == SelectedLightIndex;
			});
			if (CurrentArrayIndex == INDEX_NONE)
			{
				CurrentArrayIndex = -1;
			}
			SelectedLightIndex = Lights[(CurrentArrayIndex + 1) % Lights.Num()].Index;
		}
	}
	return FReply::Handled();
}

FReply SWoWLoginSceneDebugPanel::OnToggleLightBridgeClicked()
{
	if (AWoWLoginGameMode* GameMode = LoginGameMode.Get())
	{
		FWoWLoginSceneTuningState State = GameMode->GetLoginSceneTuningState();
		State.bUseM2PhysicalLightBridge = !State.bUseM2PhysicalLightBridge;
		GameMode->SetLoginSceneTuningState(State);
	}
	return FReply::Handled();
}

FReply SWoWLoginSceneDebugPanel::OnToggleCurrentLightClicked()
{
	if (AWoWLoginGameMode* GameMode = LoginGameMode.Get())
	{
		const TArray<AWoWLoginGameMode::FLoginSceneLight>& Lights = GameMode->GetLoginSceneLights();
		const int32 LightIndex = SelectedLightIndex == INDEX_NONE && !Lights.IsEmpty() ? Lights[0].Index : SelectedLightIndex;
		FWoWLoginLightTuning Tuning = GameMode->GetLoginSceneLightTuning(LightIndex);
		Tuning.bEnabled = !Tuning.bEnabled;
		GameMode->SetLoginSceneLightTuning(Tuning);
	}
	return FReply::Handled();
}

FReply SWoWLoginSceneDebugPanel::OnPrevBoneClicked()
{
	if (const AWoWLoginGameMode* GameMode = LoginGameMode.Get())
	{
		const TArray<int32> Indices = GameMode->GetLoginBoneIndices();
		if (!Indices.IsEmpty())
		{
			int32 CurrentArrayIndex = Indices.IndexOfByKey(SelectedBoneIndex);
			if (CurrentArrayIndex == INDEX_NONE)
			{
				CurrentArrayIndex = 0;
			}
			SelectedBoneIndex = Indices[(CurrentArrayIndex - 1 + Indices.Num()) % Indices.Num()];
		}
	}
	return FReply::Handled();
}

FReply SWoWLoginSceneDebugPanel::OnNextBoneClicked()
{
	if (const AWoWLoginGameMode* GameMode = LoginGameMode.Get())
	{
		const TArray<int32> Indices = GameMode->GetLoginBoneIndices();
		if (!Indices.IsEmpty())
		{
			int32 CurrentArrayIndex = Indices.IndexOfByKey(SelectedBoneIndex);
			if (CurrentArrayIndex == INDEX_NONE)
			{
				CurrentArrayIndex = -1;
			}
			SelectedBoneIndex = Indices[(CurrentArrayIndex + 1) % Indices.Num()];
		}
	}
	return FReply::Handled();
}

FReply SWoWLoginSceneDebugPanel::OnPrevCharacterPreviewAnimationClicked()
{
	FocusKeyboardEnumField(EKeyboardEnumField::CharacterAnimation);
	StepKeyboardEnumField(-1);
	return FReply::Handled();
}

FReply SWoWLoginSceneDebugPanel::OnNextCharacterPreviewAnimationClicked()
{
	FocusKeyboardEnumField(EKeyboardEnumField::CharacterAnimation);
	StepKeyboardEnumField(1);
	return FReply::Handled();
}

FReply SWoWLoginSceneDebugPanel::OnClearCharacterPreviewAnimationClicked()
{
	if (AWoWLoginGameMode* GameMode = LoginGameMode.Get())
	{
		GameMode->ClearCharacterPreviewAnimationOverridePath();
	}
	FocusKeyboardEnumField(EKeyboardEnumField::CharacterAnimation);
	return FReply::Handled();
}

FReply SWoWLoginSceneDebugPanel::OnTextureUERenderOverrideSelected(EWoWLoginTextureRenderOverride NewValue)
{
	if (AWoWLoginGameMode* GameMode = LoginGameMode.Get())
	{
		const TArray<int32> Indices = GetActiveTextureSectionIndices();
		const int32 SectionIndex = SelectedTextureSectionIndex == INDEX_NONE && !Indices.IsEmpty() ? Indices[0] : SelectedTextureSectionIndex;
		FWoWLoginTextureSectionTuning Tuning = GameMode->GetLoginTextureSectionTuning(SectionIndex);
		Tuning.RenderOverride = NewValue;
		GameMode->SetLoginTextureSectionTuning(Tuning);
	}
	return FReply::Handled();
}

FReply SWoWLoginSceneDebugPanel::OnTextureRenderFlagSelected(int32 NewValue)
{
	if (AWoWLoginGameMode* GameMode = LoginGameMode.Get())
	{
		const TArray<int32> Indices = GetActiveTextureSectionIndices();
		const int32 SectionIndex = SelectedTextureSectionIndex == INDEX_NONE && !Indices.IsEmpty() ? Indices[0] : SelectedTextureSectionIndex;
		FWoWLoginTextureSectionTuning Tuning = GameMode->GetLoginTextureSectionTuning(SectionIndex);
		Tuning.RenderFlagOverride = NewValue;
		Tuning.RenderOverride = EWoWLoginTextureRenderOverride::M2Original;
		GameMode->SetLoginTextureSectionTuning(Tuning);
	}
	return FReply::Handled();
}

FReply SWoWLoginSceneDebugPanel::OnTextureBlendModeSelected(int32 NewValue)
{
	if (AWoWLoginGameMode* GameMode = LoginGameMode.Get())
	{
		const TArray<int32> Indices = GetActiveTextureSectionIndices();
		const int32 SectionIndex = SelectedTextureSectionIndex == INDEX_NONE && !Indices.IsEmpty() ? Indices[0] : SelectedTextureSectionIndex;
		FWoWLoginTextureSectionTuning Tuning = GameMode->GetLoginTextureSectionTuning(SectionIndex);
		Tuning.BlendModeOverride = NewValue;
		Tuning.RenderOverride = EWoWLoginTextureRenderOverride::M2Original;
		GameMode->SetLoginTextureSectionTuning(Tuning);
	}
	return FReply::Handled();
}

FReply SWoWLoginSceneDebugPanel::OnCharacterPreviewLogicalAnimationSelected(int32 AnimationId)
{
	if (AWoWLoginGameMode* GameMode = LoginGameMode.Get())
	{
		GameMode->SetCharacterPreviewAnimationOverrideById(AnimationId);
	}
	return FReply::Handled();
}

FReply SWoWLoginSceneDebugPanel::OnCharacterPreviewAnimationSelected(const FString& ObjectPath)
{
	if (AWoWLoginGameMode* GameMode = LoginGameMode.Get())
	{
		int32 AnimationId = INDEX_NONE;
		if (ObjectPath.IsEmpty())
		{
			GameMode->ClearCharacterPreviewAnimationOverridePath();
		}
		else if (TryParsePreviewAnimationIdFromObjectPath(ObjectPath, AnimationId))
		{
			GameMode->SetCharacterPreviewAnimationOverrideById(AnimationId);
		}
	}
	return FReply::Handled();
}

FReply SWoWLoginSceneDebugPanel::OnLoginSceneSelected(int32 NewIndex)
{
	if (AWoWLoginGameMode* GameMode = LoginGameMode.Get())
	{
		SelectedParticleEmitterIndex = INDEX_NONE;
		SelectedTextureSectionIndex = INDEX_NONE;
		ActiveKeyboardEnumField = EKeyboardEnumField::None;
		GameMode->SetActiveLoginSceneIndex(NewIndex);
	}
	return FReply::Handled();
}

int32 SWoWLoginSceneDebugPanel::GetCurrentCharacterPreviewAnimationId() const
{
	int32 CurrentAnimationId = INDEX_NONE;
	if (const AWoWLoginGameMode* GameMode = LoginGameMode.Get())
	{
		const int32 CurrentOverrideId = GameMode->GetLoginSceneTuningState().CharacterPreviewAnimationOverrideId;
		const AWoWLoginGameMode::FCharacterSelectPreviewDebugState State = GameMode->GetCharacterSelectPreviewDebugState();
		if (CurrentOverrideId != INDEX_NONE)
		{
			CurrentAnimationId = CurrentOverrideId;
		}
		else
		{
			TryParsePreviewAnimationIdFromObjectPath(State.ActiveAnimationPath, CurrentAnimationId);
		}
	}
	return CurrentAnimationId;
}

void SWoWLoginSceneDebugPanel::ScrollCharacterAnimationSelectionIntoView()
{
	if (!CharacterAnimationScrollBox.IsValid())
	{
		return;
	}

	const int32 CurrentAnimationId = GetCurrentCharacterPreviewAnimationId();
	const TWeakPtr<SWidget>* RowWidget = CharacterAnimationRowWidgets.Find(CurrentAnimationId);
	if (!RowWidget || !RowWidget->IsValid())
	{
		return;
	}

	CharacterAnimationScrollBox->ScrollDescendantIntoView(
		RowWidget->Pin(),
		false,
		EDescendantScrollDestination::Center,
		4.0f);
}

void SWoWLoginSceneDebugPanel::FocusKeyboardEnumField(EKeyboardEnumField NewField)
{
	ActiveKeyboardEnumField = NewField;
	FSlateApplication::Get().SetKeyboardFocus(AsShared(), EFocusCause::SetDirectly);
}

bool SWoWLoginSceneDebugPanel::StepKeyboardEnumField(int32 Direction)
{
	AWoWLoginGameMode* GameMode = LoginGameMode.Get();
	if (!GameMode || ActiveKeyboardEnumField == EKeyboardEnumField::None || Direction == 0)
	{
		return false;
	}

	if (ActiveKeyboardEnumField == EKeyboardEnumField::CharacterAnimation)
	{
		TArray<AWoWLoginGameMode::FCharacterPreviewAnimationOption> LogicalOptions = GameMode->GetCharacterPreviewLogicalAnimationOptions();
		LogicalOptions.RemoveAll([](const AWoWLoginGameMode::FCharacterPreviewAnimationOption& Option)
		{
			return !Option.bAvailable;
		});
		if (!LogicalOptions.IsEmpty())
		{
			const int32 CurrentOverrideId = GameMode->GetLoginSceneTuningState().CharacterPreviewAnimationOverrideId;
			const AWoWLoginGameMode::FCharacterSelectPreviewDebugState State = GameMode->GetCharacterSelectPreviewDebugState();
			int32 CurrentCycleIndex = 0;
			int32 CurrentAnimationId = INDEX_NONE;
			if (CurrentOverrideId != INDEX_NONE || TryParsePreviewAnimationIdFromObjectPath(State.ActiveAnimationPath, CurrentAnimationId))
			{
				if (CurrentOverrideId != INDEX_NONE)
				{
					CurrentAnimationId = CurrentOverrideId;
				}
				const int32 CurrentArrayIndex = LogicalOptions.IndexOfByPredicate([CurrentAnimationId](const AWoWLoginGameMode::FCharacterPreviewAnimationOption& Option)
				{
					return Option.AnimationId == CurrentAnimationId;
				});
				CurrentCycleIndex = CurrentArrayIndex == INDEX_NONE ? 0 : CurrentArrayIndex + 1;
			}

			const int32 CycleCount = LogicalOptions.Num() + 1;
			const int32 NextCycleIndex = (CurrentCycleIndex + Direction + CycleCount) % CycleCount;
			if (NextCycleIndex == 0)
			{
				GameMode->ClearCharacterPreviewAnimationOverridePath();
			}
			else
			{
				GameMode->SetCharacterPreviewAnimationOverrideById(LogicalOptions[NextCycleIndex - 1].AnimationId);
			}
			ScrollCharacterAnimationSelectionIntoView();
			return true;
		}
		return false;
	}

	const TArray<int32> Indices = GetActiveTextureSectionIndices();
	if (Indices.IsEmpty())
	{
		return false;
	}

	const int32 SectionIndex = SelectedTextureSectionIndex == INDEX_NONE ? Indices[0] : SelectedTextureSectionIndex;
	const AWoWLoginGameMode::FSectionTextureTransformTrack* Track = GameMode->GetLoginRenderSectionByIndex(SectionIndex);
	FWoWLoginTextureSectionTuning Tuning = GameMode->GetLoginTextureSectionTuning(SectionIndex);

	if (ActiveKeyboardEnumField == EKeyboardEnumField::TextureRenderFlag)
	{
		const int32 BaseValue = Tuning.RenderFlagOverride == INDEX_NONE ? (Track ? Track->MaterialFlags : 0) : Tuning.RenderFlagOverride;
		Tuning.RenderFlagOverride = (BaseValue + Direction + 32) % 32;
		Tuning.RenderOverride = EWoWLoginTextureRenderOverride::M2Original;
		GameMode->SetLoginTextureSectionTuning(Tuning);
		return true;
	}

	if (ActiveKeyboardEnumField == EKeyboardEnumField::TextureBlendMode)
	{
		const int32 BaseValue = Tuning.BlendModeOverride == INDEX_NONE ? (Track ? Track->RenderMode : 0) : Tuning.BlendModeOverride;
		Tuning.BlendModeOverride = (BaseValue + Direction + 7) % 7;
		Tuning.RenderOverride = EWoWLoginTextureRenderOverride::M2Original;
		GameMode->SetLoginTextureSectionTuning(Tuning);
		return true;
	}

	return false;
}

void SWoWLoginSceneDebugPanel::UpdateTextureSectionHighlight() const
{
	AWoWLoginGameMode* GameMode = LoginGameMode.Get();
	if (!GameMode || ActiveTab != EDebugTab::Textures)
	{
		return;
	}

	const TArray<int32> Indices = GetActiveTextureSectionIndices();
	const int32 SectionIndex = SelectedTextureSectionIndex == INDEX_NONE && !Indices.IsEmpty() ? Indices[0] : SelectedTextureSectionIndex;
	GameMode->SetLoginTextureDebugHighlightSection(SectionIndex);
}
