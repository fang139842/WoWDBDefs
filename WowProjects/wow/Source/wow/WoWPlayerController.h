#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "WoWPlayerController.generated.h"

class UWoWCharacterPreviewComponent;
class AWoWPreviewTargetCharacter;
class SWidget;

UCLASS()
class WOW_API AWoWPlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	AWoWPlayerController();
	static void ConfigureWoWHardwareCursors(UGameViewportClient* ViewportClient);

	virtual void BeginPlay() override;
	virtual void OnPossess(APawn* InPawn) override;
	virtual void SetupInputComponent() override;
	virtual void PlayerTick(float DeltaTime) override;

	UFUNCTION(Exec)
	void WoWSpeed(float NewSpeedModifier);

	UFUNCTION(Exec)
	void WoWSpeedReset();

private:
	void HandleLeftMousePressed();
	void HandleLeftMouseReleased();
	void HandleRightMousePressed();
	void HandleRightMouseReleased();
	void HandleMouseX(float Value);
	void HandleMouseY(float Value);
	void HandleMouseWheel(float Value);
	void HandleEscapePressed();
	void HandleMoveForwardPressed();
	void HandleMoveForwardReleased();
	void HandleMoveBackwardPressed();
	void HandleMoveBackwardReleased();
	void HandleTurnLeftPressed();
	void HandleTurnLeftReleased();
	void HandleTurnRightPressed();
	void HandleTurnRightReleased();
	void HandleStrafeLeftPressed();
	void HandleStrafeLeftReleased();
	void HandleStrafeRightPressed();
	void HandleStrafeRightReleased();
	void HandleSitTogglePressed();
	void HandleWeaponSheatheTogglePressed();
	void HandleJumpPressed();
	void HandleJumpReleased();
	void HandleDebugPreviousAnimationPressed();
	void HandleDebugNextAnimationPressed();
	void HandleDebugClearAnimationPressed();
	void HandleDebugEquipWeaponRightHandPressed();
	void HandleDebugEquipWeaponLeftHandPressed();
	void HandleDebugUnequipWeaponPressed();
	void HandleDebugCycleSheathedAttachmentPressed();
	void HandleDebugCycleSheathedRotationPressed();
	void HandleDebugCycleSheathedNudgeModePressed();
	void HandleDebugSheathedNudgeDecreasePressed();
	void HandleDebugSheathedNudgeIncreasePressed();
	void HandleToggleSheathedWeaponDebugPanelPressed();
	void HandleSpeedIncreasePressed();
	void HandleSpeedDecreasePressed();
	void HandleSpeedResetPressed();
	void TryRightClickAutoAttack();
	AWoWPreviewTargetCharacter* FindPreviewTargetUnderCursor(bool bAllowAimFallback) const;
	void UpdateAutoAttack(float DeltaTime);
	void CancelAutoAttack(float LingerSeconds);
	void UpdateHoverCursor();
	void ApplyCursorMode();
	void CaptureMouseLookOriginIfNeeded();
	void RestoreMouseLookOriginIfNeeded();
	void ConfigureHardwareCursor();
	void ConfigureControlledPawn();
	void ConfigurePreviewRendering();
	void EnsureCharacterPreviewComponent();
	void UpdateCharacterPreviewMovementState();
	void FaceControlledPawnToControlYaw();
	void ApplyKeyboardMovement(float DeltaTime);
	void StartCameraReturnToPawnYaw();
	void UpdateCameraReturnToPawnYaw(float DeltaTime);
	void SetSittingRequested(bool bInSitting, bool bPlayTransition = true);
	void CancelSittingForMovement();
	bool CanStartSitting() const;
	void SetLocalSpeedModifier(float NewSpeedModifier, bool bShowFeedback);
	float GetLocalMovementMaxWalkSpeed(float ForwardInput) const;
	void ZoomCamera(float WheelValue);
	void ToggleSheathedWeaponDebugPanel();

	UPROPERTY(EditDefaultsOnly, Category = "WoW Input")
	float MouseLookYawScale = 1.0f;

	UPROPERTY(EditDefaultsOnly, Category = "WoW Input")
	float MouseLookPitchScale = 1.0f;

	UPROPERTY(EditDefaultsOnly, Category = "WoW Input")
	float KeyboardTurnRateDegrees = 120.0f;

	// 左键锁镜头转身后，松开左键时镜头追到角色朝向的角速度；数值越大，回正越快但越接近瞬间切换。
	UPROPERTY(EditDefaultsOnly, Category = "WoW Input")
	float CameraReturnToPawnYawSpeedDegrees = 540.0f;

	// ============================================
	// === 相机/镜头调整参数 ===
	// 可以在蓝图Defaults中手动微调这些值, 或在此处修改默认值:
	//   CameraZoomStep     : 滚轮每格缩放距离 (默认80 UE单位)
	//   MinCameraDistance  : 最近镜头距离 (默认180, 调小=更贴近角色)
	//   MaxCameraDistance  : 最远镜头距离 (默认900, 调大=拉更远)
	//   另外 SpringArm 的 TargetOffset 在角色蓝图中控制镜头垂直偏移
	//   如果要调整FOV: 在 BP_ThirdPersonCharacter 蓝图的 CameraComponent 中设置
	// ============================================
	UPROPERTY(EditDefaultsOnly, Category = "WoW Input", meta = (ClampMin = "40.0", ClampMax = "300.0"))
	float CameraZoomStep = 80.0f;

	UPROPERTY(EditDefaultsOnly, Category = "WoW Input", meta = (ClampMin = "60.0", ClampMax = "2000.0"))
	float MinCameraDistance = 180.0f;

	UPROPERTY(EditDefaultsOnly, Category = "WoW Input", meta = (ClampMin = "200.0", ClampMax = "5000.0"))
	float MaxCameraDistance = 900.0f;

	UPROPERTY(EditDefaultsOnly, Category = "WoW Input")
	float RightClickAttackMaxSeconds = 0.25f;

	UPROPERTY(EditDefaultsOnly, Category = "WoW Input")
	float RightClickDragThreshold = 0.35f;

	UPROPERTY(EditDefaultsOnly, Category = "WoW Combat")
	float MeleeAttackRange = 320.0f;

	UPROPERTY(EditDefaultsOnly, Category = "WoW Combat")
	float AutoAttackIntervalSeconds = 2.0f;

	UPROPERTY(EditDefaultsOnly, Category = "WoW Combat")
	float CombatReadyLingerSeconds = 0.75f;

	bool bLeftMouseHeld = false;
	bool bRightMouseHeld = false;
	bool bLeftMouseDragged = false;
	bool bRightMouseDragged = false;
	bool bLastAppliedMouseLookHeld = false;
	bool bHasMouseLookOrigin = false;
	int32 MouseLookOriginX = 0;
	int32 MouseLookOriginY = 0;
	float LeftMousePressedTimeSeconds = 0.0f;
	float RightMousePressedTimeSeconds = 0.0f;
	float MouseTurnAnimationSeconds = 0.0f;
	float LastMouseTurnDirection = 0.0f;
	bool bCameraReturnToPawnYawActive = false;
	bool bLeftMouseKeyboardTurnedPawn = false;
	bool bMoveForwardHeld = false;
	bool bMoveBackwardHeld = false;
	bool bTurnLeftHeld = false;
	bool bTurnRightHeld = false;
	bool bStrafeLeftHeld = false;
	bool bStrafeRightHeld = false;
	bool bSittingRequested = false;
	float CurrentForwardInput = 0.0f;
	float CurrentStrafeInput = 0.0f;
	// 本地移动速度倍率：用于客户端测试，语义对齐AzerothCore `.mod speed 1-50`，但不代表服务端授权速度。
	float LocalSpeedModifier = 1.0f;
	// 空中惯性：离地时记录水平速度，松开移动键也不让UE在空中把水平速度刹掉。
	FVector AirborneInertiaVelocity = FVector::ZeroVector;
	bool bHasAirborneInertiaVelocity = false;
	// 空中姿态：原地跳后点按W/S/Q/E也要保持对应空中姿态，直到落地才回正。
	float AirborneVisualForwardInput = 0.0f;
	float AirborneVisualStrafeInput = 0.0f;
	bool bHasAirborneVisualInput = false;
	// 本次跳跃是否从地面移动状态起跳；原地起跳后空中补方向，落地仍应走常规蹲下缓冲。
	bool bJumpStartedWithGroundMovement = false;
	TWeakObjectPtr<AWoWPreviewTargetCharacter> SelectedAutoAttackTarget;
	float AutoAttackTimerSeconds = 0.0f;
	bool bSelectedTargetEngaged = false;
	bool bHoveringAttackTarget = false;
	TWeakObjectPtr<UWoWCharacterPreviewComponent> CharacterPreviewComponent;
	TSharedPtr<SWidget> SheathedWeaponDebugPanelWidget;
};
