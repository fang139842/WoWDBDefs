#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "WoWLoginGameMode.h"

class SWoWLoginSceneDebugPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SWoWLoginSceneDebugPanel) {}
		SLATE_ARGUMENT(TWeakObjectPtr<AWoWLoginGameMode>, LoginGameMode)
		SLATE_EVENT(FSimpleDelegate, OnCloseRequested)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual bool SupportsKeyboardFocus() const override;
	virtual FReply OnPreviewKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;
	virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;
	virtual FReply OnMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnMouseMove(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;

	enum class EDebugFloatField : uint8
	{
		SceneLocationX,
		SceneLocationY,
		SceneLocationZ,
		SceneRotationPitch,
		SceneRotationYaw,
		SceneRotationRoll,
		SceneScaleX,
		SceneScaleY,
		SceneScaleZ,
		CameraPositionX,
		CameraPositionY,
		CameraPositionZ,
		CameraTargetX,
		CameraTargetY,
		CameraTargetZ,
		CameraFov,
		CameraNearClipping,
		CameraFarClipping,
		CameraRollEffectX,
		GlobalBrightnessScale,
		M2PhysicalLightIntensityScale,
		LightDiffuseIntensityScale,
		LightAmbientIntensityScale,
		LightDiffuseColorScaleR,
		LightDiffuseColorScaleG,
		LightDiffuseColorScaleB,
		LightAmbientColorScaleR,
		LightAmbientColorScaleG,
		LightAmbientColorScaleB,
		LightAttenuationRadiusScale,
		ParticlePositionX,
		ParticlePositionY,
		ParticlePositionZ,
		ParticleEmissionRateScale,
		ParticleEmissionSpeedScale,
		ParticleLifespanScale,
		ParticleAreaLengthScale,
		ParticleAreaWidthScale,
		ParticleGravityScale,
		ParticleGravity2Scale,
		ParticleVerticalRangeScale,
		ParticleHorizontalRangeScale,
		ParticleSizeScale,
		ParticleLifecycleSizeStartScale,
		ParticleLifecycleSizeMidScale,
		ParticleLifecycleSizeEndScale,
		ParticleAlphaScale,
		ParticleLifecycleAlphaStartScale,
		ParticleLifecycleAlphaMidScale,
		ParticleLifecycleAlphaEndScale,
		ParticleAlphaCutoff,
		ParticleBrightnessScale,
		TextureUVOffsetX,
		TextureUVOffsetY,
		TextureUVScaleX,
		TextureUVScaleY,
		TextureUVRotationDegrees,
		TextureAlphaScale,
		TextureBrightnessScale,
		CharacterPreviewLocationX,
		CharacterPreviewLocationY,
		CharacterPreviewLocationZ,
		CharacterPreviewRotationPitch,
		CharacterPreviewRotationYaw,
		CharacterPreviewRotationRoll,
		CharacterPreviewUniformScale,
		CharacterPreviewScaleX,
		CharacterPreviewScaleY,
		CharacterPreviewScaleZ
	};

	enum class EKeyboardEnumField : uint8
	{
		None,
		TextureRenderFlag,
		TextureBlendMode,
		CharacterAnimation
	};

private:
	enum class EDebugTab : uint8
	{
		Scene,
		Camera,
		Lighting,
		Particles,
		Textures,
		Bones,
		Character,
		GlueLog
	};

	FText GetMappingHelpText() const;
	FText GetSummaryText() const;
	FText GetPresetBakeStatusText() const;
	FText GetExportText() const;
	FText GetSceneRawDetailsText() const;
	FText GetCameraRawDetailsText() const;
	FText GetCameraAnimBlockSummaryText() const;
	FText GetLightingSummaryText() const;
	FText GetM2LightsDetailsText() const;
	FText GetSelectedLightText() const;
	FText GetSelectedNoteText() const;
	FText GetLoginSceneComboText() const;
	FText GetFieldText(EDebugFloatField Field) const;
	FText GetSelectedParticleText() const;
	FText GetSelectedParticleDetailsText() const;
	FText GetSelectedBoneText() const;
	FText GetSelectedBoneDetailsText() const;
	FText GetFieldDescriptionText(EDebugFloatField Field) const;
	FText GetSelectedTextureText() const;
	FText GetSelectedTextureDetailsText() const;
	FText GetCharacterPreviewDebugText() const;
	FText GetCharacterPreviewAnimationOverrideText() const;
	FText GetCharacterPreviewAnimationComboText() const;
	FText GetGlueDebugLogText() const;
	FText GetGlueCharacterInfoDebugText() const;
	TArray<int32> GetActiveTextureSectionIndices() const;
	const AWoWLoginGameMode::FSectionTextureTransformTrack* GetSelectedRenderSection() const;
	FText GetTextureRenderFlagComboText() const;
	FText GetTextureBlendModeComboText() const;
	FText GetTextureUERenderOverrideComboText() const;
	TOptional<FSlateRenderTransform> GetPanelRenderTransform() const;
	TSharedRef<SWidget> MakeTextureRenderFlagCombo();
	TSharedRef<SWidget> MakeTextureBlendModeCombo();
	TSharedRef<SWidget> MakeTextureUERenderOverrideCombo();
	TSharedRef<SWidget> MakeCharacterPreviewAnimationCombo();
	TSharedRef<SWidget> MakeLoginSceneCombo();
	float GetSliderValue(EDebugFloatField Field) const;
	TOptional<float> GetFieldNumericValue(EDebugFloatField Field) const;
	void OnSliderChanged(float Value, EDebugFloatField Field);
	void OnSliderWheelDelta(float WheelDelta, EDebugFloatField Field);
	void OnFieldNumericCommitted(float Value, ETextCommit::Type CommitType, EDebugFloatField Field);
	void SetFieldDisplayValue(float Value, EDebugFloatField Field);
	void ResetFieldValue(EDebugFloatField Field);
	int32 GetActiveTabIndex() const;
	FReply OnCopyParamsClicked();
	FReply OnCopyCharacterDebugClicked();
	FReply OnExportCharacterDebugLogClicked();
	FReply OnSaveAssetClicked();
	FReply OnBakeScenePresetClicked();
	FReply OnBakeAllScenePresetsClicked();
	FReply OnCloseClicked();
	FReply OnTabClicked(EDebugTab NewTab);
	FReply OnResetFieldClicked(EDebugFloatField Field);
	FReply OnResetCurrentClicked();
	FReply OnResetAllInTabClicked();
	FReply OnPrevParticleClicked();
	FReply OnNextParticleClicked();
	FReply OnToggleParticleEnabledClicked();
	FReply OnClearParticlePositionOffsetClicked();
	FReply OnPrevTextureClicked();
	FReply OnNextTextureClicked();
	FReply OnToggleTextureEnabledClicked();
	FReply OnToggleTextureM2AnimationClicked();
	FReply OnToggleTextureForceVisibleClicked();
	FReply OnPrevLightClicked();
	FReply OnNextLightClicked();
	FReply OnToggleLightBridgeClicked();
	FReply OnToggleCurrentLightClicked();
	FReply OnPrevBoneClicked();
	FReply OnNextBoneClicked();
	FReply OnPrevCharacterPreviewAnimationClicked();
	FReply OnNextCharacterPreviewAnimationClicked();
	FReply OnClearCharacterPreviewAnimationClicked();
	FReply OnClearGlueDebugLogClicked();
	void OnCharacterPreviewAnimationOverrideCommitted(const FText& Text, ETextCommit::Type CommitType);
	void OnNoteCommitted(const FText& Text, ETextCommit::Type CommitType);
	FReply OnToggleRawDetailsClicked();
	FReply OnTextureUERenderOverrideSelected(EWoWLoginTextureRenderOverride NewValue);
	FReply OnTextureRenderFlagSelected(int32 NewValue);
	FReply OnTextureBlendModeSelected(int32 NewValue);
	FReply OnCharacterPreviewLogicalAnimationSelected(int32 AnimationId);
	FReply OnCharacterPreviewAnimationSelected(const FString& ObjectPath);
	FReply OnLoginSceneSelected(int32 NewIndex);
	FReply OnHeaderMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent);
	int32 GetCurrentCharacterPreviewAnimationId() const;
	void ScrollCharacterAnimationSelectionIntoView();
	void FocusKeyboardEnumField(EKeyboardEnumField NewField);
	bool StepKeyboardEnumField(int32 Direction);
	void UpdateTextureSectionHighlight() const;
	void RebuildCharacterAnimationMenuContent();

	TWeakObjectPtr<AWoWLoginGameMode> LoginGameMode;
	FSimpleDelegate OnCloseRequested;
	TSharedPtr<class SMenuAnchor> CharacterAnimationMenuAnchor;
	TSharedPtr<class SScrollBox> CharacterAnimationScrollBox;
	TSharedPtr<class SVerticalBox> CharacterAnimationMenuContentBox;
	TMap<int32, TWeakPtr<class SWidget>> CharacterAnimationRowWidgets;
	FString CharacterAnimationFilterText;
	EDebugTab ActiveTab = EDebugTab::Scene;
	int32 SelectedParticleEmitterIndex = INDEX_NONE;
	int32 SelectedTextureSectionIndex = INDEX_NONE;
	int32 SelectedLightIndex = INDEX_NONE;
	int32 SelectedBoneIndex = INDEX_NONE;
	FVector2D PanelDragOffset = FVector2D::ZeroVector;
	FVector2D PanelDragStartScreen = FVector2D::ZeroVector;
	FVector2D PanelDragStartOffset = FVector2D::ZeroVector;
	EKeyboardEnumField ActiveKeyboardEnumField = EKeyboardEnumField::None;
	bool bDraggingPanel = false;
	bool bShowRawDetails = false;
	FString PresetBakeStatusText;
};
