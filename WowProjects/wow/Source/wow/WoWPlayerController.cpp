#include "WoWPlayerController.h"

#include "WoWCharacterPreviewComponent.h"
#include "WoWGameInstance.h"
#include "WoWPreviewTargetCharacter.h"
#include "WoWSheathedWeaponDebugPanel.h"

#include "Engine/GameViewportClient.h"
#include "Engine/Engine.h"
#include "Engine/HitResult.h"
#include "EngineUtils.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/SpringArmComponent.h"
#include "HAL/IConsoleManager.h"
#include "InputCoreTypes.h"
#include "Widgets/SOverlay.h"

namespace
{
constexpr float WoWForwardRunSpeed = 600.0f;
constexpr float WoWBackpedalSpeedScale = 0.64f;
// 本地速度调试范围：对应AzerothCore `.mod speed 1-50` 的测试输入范围，默认1为正常速度。
constexpr float WoWMinLocalSpeedModifier = 0.1f;
constexpr float WoWMaxLocalSpeedModifier = 50.0f;
constexpr float WoWLocalSpeedStep = 0.1f;
// WoW跳跃中松开移动键仍保留离地时的水平惯性；UE默认空中控制/制动必须关闭。
constexpr float WoWAirControl = 0.0f;
constexpr float WoWFallingLateralFriction = 0.0f;
constexpr float WoWBrakingDecelerationFalling = 0.0f;
// 原地起跳后才按W/S/Q/E时生成的空中水平速度比例；只影响站立跳后的空中补方向距离，不影响跑动起跳惯性。
constexpr float WoWStandingJumpAirInputSpeedScale = 0.42f;

FVector BuildWoWKeyboardMovementDirection(const FRotator& YawOnlyRotation, float ForwardInput, float StrafeInput)
{
	FVector Direction = FVector::ZeroVector;
	if (!FMath::IsNearlyZero(ForwardInput))
	{
		Direction += YawOnlyRotation.Vector() * ForwardInput;
	}
	if (!FMath::IsNearlyZero(StrafeInput))
	{
		Direction += FRotationMatrix(YawOnlyRotation).GetUnitAxis(EAxis::Y) * StrafeInput;
	}
	return Direction.GetSafeNormal2D();
}
}

AWoWPlayerController::AWoWPlayerController()
{
	bShowMouseCursor = true;
	bEnableClickEvents = true;
	bEnableMouseOverEvents = true;
	DefaultMouseCursor = EMouseCursor::Default;
}

void AWoWPlayerController::ConfigureWoWHardwareCursors(UGameViewportClient* ViewportClient)
{
	if (!ViewportClient)
	{
		return;
	}

	ViewportClient->SetHardwareCursor(EMouseCursor::Default, TEXT("Interface/CURSOR/Point"), FVector2D::ZeroVector);
	ViewportClient->SetHardwareCursor(EMouseCursor::Crosshairs, TEXT("Interface/CURSOR/attack"), FVector2D::ZeroVector);
	UE_LOG(LogTemp, Display, TEXT("Configured WoW hardware cursors: Interface/CURSOR/Point and Interface/CURSOR/attack"));
}

void AWoWPlayerController::BeginPlay()
{
	Super::BeginPlay();

	UE_LOG(LogTemp, Display, TEXT("AWoWPlayerController BeginPlay: applying WoW cursor/input mode"));
	ConfigurePreviewRendering();
	ConfigureHardwareCursor();
	ConfigureControlledPawn();
	EnsureCharacterPreviewComponent();
	bLastAppliedMouseLookHeld = !(bLeftMouseHeld || bRightMouseHeld);
	ApplyCursorMode();
}

void AWoWPlayerController::OnPossess(APawn* InPawn)
{
	Super::OnPossess(InPawn);
	ConfigureControlledPawn();
	EnsureCharacterPreviewComponent();
}

void AWoWPlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();

	check(InputComponent);
	UE_LOG(LogTemp, Display, TEXT("AWoWPlayerController SetupInputComponent: binding WoW mouse and movement controls"));
	InputComponent->BindKey(EKeys::LeftMouseButton, IE_Pressed, this, &AWoWPlayerController::HandleLeftMousePressed);
	InputComponent->BindKey(EKeys::LeftMouseButton, IE_Released, this, &AWoWPlayerController::HandleLeftMouseReleased);
	InputComponent->BindKey(EKeys::RightMouseButton, IE_Pressed, this, &AWoWPlayerController::HandleRightMousePressed);
	InputComponent->BindKey(EKeys::RightMouseButton, IE_Released, this, &AWoWPlayerController::HandleRightMouseReleased);
	InputComponent->BindAxisKey(EKeys::MouseX, this, &AWoWPlayerController::HandleMouseX);
	InputComponent->BindAxisKey(EKeys::MouseY, this, &AWoWPlayerController::HandleMouseY);
	InputComponent->BindAxisKey(EKeys::MouseWheelAxis, this, &AWoWPlayerController::HandleMouseWheel);

	InputComponent->BindKey(EKeys::W, IE_Pressed, this, &AWoWPlayerController::HandleMoveForwardPressed);
	InputComponent->BindKey(EKeys::W, IE_Released, this, &AWoWPlayerController::HandleMoveForwardReleased);
	InputComponent->BindKey(EKeys::S, IE_Pressed, this, &AWoWPlayerController::HandleMoveBackwardPressed);
	InputComponent->BindKey(EKeys::S, IE_Released, this, &AWoWPlayerController::HandleMoveBackwardReleased);
	InputComponent->BindKey(EKeys::A, IE_Pressed, this, &AWoWPlayerController::HandleTurnLeftPressed);
	InputComponent->BindKey(EKeys::A, IE_Released, this, &AWoWPlayerController::HandleTurnLeftReleased);
	InputComponent->BindKey(EKeys::D, IE_Pressed, this, &AWoWPlayerController::HandleTurnRightPressed);
	InputComponent->BindKey(EKeys::D, IE_Released, this, &AWoWPlayerController::HandleTurnRightReleased);
	InputComponent->BindKey(EKeys::Q, IE_Pressed, this, &AWoWPlayerController::HandleStrafeLeftPressed);
	InputComponent->BindKey(EKeys::Q, IE_Released, this, &AWoWPlayerController::HandleStrafeLeftReleased);
	InputComponent->BindKey(EKeys::E, IE_Pressed, this, &AWoWPlayerController::HandleStrafeRightPressed);
	InputComponent->BindKey(EKeys::E, IE_Released, this, &AWoWPlayerController::HandleStrafeRightReleased);
	InputComponent->BindKey(EKeys::X, IE_Pressed, this, &AWoWPlayerController::HandleSitTogglePressed);
	InputComponent->BindKey(EKeys::Z, IE_Pressed, this, &AWoWPlayerController::HandleWeaponSheatheTogglePressed);
	InputComponent->BindKey(EKeys::SpaceBar, IE_Pressed, this, &AWoWPlayerController::HandleJumpPressed);
	InputComponent->BindKey(EKeys::SpaceBar, IE_Released, this, &AWoWPlayerController::HandleJumpReleased);
	InputComponent->BindKey(EKeys::Escape, IE_Pressed, this, &AWoWPlayerController::HandleEscapePressed);
	InputComponent->BindKey(EKeys::PageUp, IE_Pressed, this, &AWoWPlayerController::HandleDebugPreviousAnimationPressed);
	InputComponent->BindKey(EKeys::PageDown, IE_Pressed, this, &AWoWPlayerController::HandleDebugNextAnimationPressed);
	InputComponent->BindKey(EKeys::Zero, IE_Pressed, this, &AWoWPlayerController::HandleDebugClearAnimationPressed);
	InputComponent->BindKey(EKeys::F6, IE_Pressed, this, &AWoWPlayerController::HandleDebugEquipWeaponRightHandPressed);
	InputComponent->BindKey(EKeys::F7, IE_Pressed, this, &AWoWPlayerController::HandleDebugEquipWeaponLeftHandPressed);
	InputComponent->BindKey(EKeys::F10, IE_Pressed, this, &AWoWPlayerController::HandleDebugUnequipWeaponPressed);
	InputComponent->BindKey(EKeys::C, IE_Pressed, this, &AWoWPlayerController::HandleDebugCycleSheathedAttachmentPressed);
	InputComponent->BindKey(EKeys::V, IE_Pressed, this, &AWoWPlayerController::HandleDebugCycleSheathedRotationPressed);
	InputComponent->BindKey(EKeys::B, IE_Pressed, this, &AWoWPlayerController::HandleDebugCycleSheathedNudgeModePressed);
	InputComponent->BindKey(EKeys::N, IE_Pressed, this, &AWoWPlayerController::HandleDebugSheathedNudgeDecreasePressed);
	InputComponent->BindKey(EKeys::M, IE_Pressed, this, &AWoWPlayerController::HandleDebugSheathedNudgeIncreasePressed);
	InputComponent->BindKey(EKeys::F4, IE_Pressed, this, &AWoWPlayerController::HandleToggleSheathedWeaponDebugPanelPressed);
	InputComponent->BindKey(EKeys::Add, IE_Pressed, this, &AWoWPlayerController::HandleSpeedIncreasePressed);
	InputComponent->BindKey(EKeys::Subtract, IE_Pressed, this, &AWoWPlayerController::HandleSpeedDecreasePressed);
	InputComponent->BindKey(EKeys::Multiply, IE_Pressed, this, &AWoWPlayerController::HandleSpeedResetPressed);
}

void AWoWPlayerController::WoWSpeed(float NewSpeedModifier)
{
	SetLocalSpeedModifier(NewSpeedModifier, true);
}

void AWoWPlayerController::WoWSpeedReset()
{
	SetLocalSpeedModifier(1.0f, true);
}

void AWoWPlayerController::PlayerTick(float DeltaTime)
{
	Super::PlayerTick(DeltaTime);

	const bool bMouseLookHeld = bLeftMouseHeld || bRightMouseHeld;
	if (bLastAppliedMouseLookHeld != bMouseLookHeld)
	{
		ApplyCursorMode();
	}
	else if (!bMouseLookHeld)
	{
		bShowMouseCursor = true;
	}

	MouseTurnAnimationSeconds = FMath::Max(MouseTurnAnimationSeconds - DeltaTime, 0.0f);
	ApplyKeyboardMovement(DeltaTime);
	UpdateCameraReturnToPawnYaw(DeltaTime);
	UpdateAutoAttack(DeltaTime);
	UpdateCharacterPreviewMovementState();
	UpdateHoverCursor();
}

void AWoWPlayerController::HandleLeftMousePressed()
{
	CaptureMouseLookOriginIfNeeded();
	bLeftMouseHeld = true;
	bLeftMouseDragged = false;
	bCameraReturnToPawnYawActive = false;
	bLeftMouseKeyboardTurnedPawn = false;
	LeftMousePressedTimeSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
	ApplyCursorMode();
}

void AWoWPlayerController::HandleLeftMouseReleased()
{
	const float ReleasedTimeSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : LeftMousePressedTimeSeconds;
	const bool bWasShortClick = (ReleasedTimeSeconds - LeftMousePressedTimeSeconds) <= RightClickAttackMaxSeconds;
	bLeftMouseHeld = false;
	if (!bRightMouseHeld && !bLeftMouseDragged && bWasShortClick)
	{
		const bool bClickedTarget = FindPreviewTargetUnderCursor(false) != nullptr;
		if (!bClickedTarget)
		{
			CancelAutoAttack(CombatReadyLingerSeconds);
		}
	}
	bLeftMouseDragged = false;
	if (bLeftMouseKeyboardTurnedPawn)
	{
		StartCameraReturnToPawnYaw();
	}
	bLeftMouseKeyboardTurnedPawn = false;
	ApplyCursorMode();
	RestoreMouseLookOriginIfNeeded();
}

void AWoWPlayerController::HandleRightMousePressed()
{
	CaptureMouseLookOriginIfNeeded();
	bRightMouseHeld = true;
	bRightMouseDragged = false;
	bCameraReturnToPawnYawActive = false;
	RightMousePressedTimeSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
	ApplyCursorMode();
}

void AWoWPlayerController::HandleRightMouseReleased()
{
	const float ReleasedTimeSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : RightMousePressedTimeSeconds;
	const bool bWasShortClick = (ReleasedTimeSeconds - RightMousePressedTimeSeconds) <= RightClickAttackMaxSeconds;
	if (!bRightMouseDragged && bWasShortClick)
	{
		TryRightClickAutoAttack();
	}

	bRightMouseHeld = false;
	bRightMouseDragged = false;
	ApplyCursorMode();
	RestoreMouseLookOriginIfNeeded();
}

void AWoWPlayerController::HandleMouseX(float Value)
{
	if ((bLeftMouseHeld || bRightMouseHeld) && !FMath::IsNearlyZero(Value))
	{
		bCameraReturnToPawnYawActive = false;
		if (bRightMouseHeld && FMath::Abs(Value) > RightClickDragThreshold)
		{
			bRightMouseDragged = true;
		}
		if (bLeftMouseHeld && FMath::Abs(Value) > RightClickDragThreshold)
		{
			bLeftMouseDragged = true;
		}
		if (bRightMouseHeld)
		{
			MouseTurnAnimationSeconds = 0.12f;
			LastMouseTurnDirection = FMath::Sign(Value);
		}
		AddYawInput(Value * MouseLookYawScale);
	}
}

void AWoWPlayerController::HandleMouseY(float Value)
{
	if ((bLeftMouseHeld || bRightMouseHeld) && !FMath::IsNearlyZero(Value))
	{
		if (bRightMouseHeld && FMath::Abs(Value) > RightClickDragThreshold)
		{
			bRightMouseDragged = true;
		}
		if (bLeftMouseHeld && FMath::Abs(Value) > RightClickDragThreshold)
		{
			bLeftMouseDragged = true;
		}
		AddPitchInput(-Value * MouseLookPitchScale);
	}
}

void AWoWPlayerController::HandleMouseWheel(float Value)
{
	if (!FMath::IsNearlyZero(Value))
	{
		ZoomCamera(Value);
	}
}

void AWoWPlayerController::HandleMoveForwardPressed()
{
	CancelSittingForMovement();
	bMoveForwardHeld = true;
}

void AWoWPlayerController::HandleMoveForwardReleased()
{
	bMoveForwardHeld = false;
}

void AWoWPlayerController::HandleMoveBackwardPressed()
{
	CancelSittingForMovement();
	bMoveBackwardHeld = true;
}

void AWoWPlayerController::HandleMoveBackwardReleased()
{
	bMoveBackwardHeld = false;
}

void AWoWPlayerController::HandleTurnLeftPressed()
{
	CancelSittingForMovement();
	bTurnLeftHeld = true;
}

void AWoWPlayerController::HandleTurnLeftReleased()
{
	bTurnLeftHeld = false;
}

void AWoWPlayerController::HandleTurnRightPressed()
{
	CancelSittingForMovement();
	bTurnRightHeld = true;
}

void AWoWPlayerController::HandleTurnRightReleased()
{
	bTurnRightHeld = false;
}

void AWoWPlayerController::HandleStrafeLeftPressed()
{
	CancelSittingForMovement();
	bStrafeLeftHeld = true;
	UE_LOG(LogTemp, Display, TEXT("WoW input: Q/StrafeLeft pressed"));
}

void AWoWPlayerController::HandleStrafeLeftReleased()
{
	bStrafeLeftHeld = false;
	UE_LOG(LogTemp, Display, TEXT("WoW input: Q/StrafeLeft released"));
}

void AWoWPlayerController::HandleStrafeRightPressed()
{
	CancelSittingForMovement();
	bStrafeRightHeld = true;
	UE_LOG(LogTemp, Display, TEXT("WoW input: E/StrafeRight pressed"));
}

void AWoWPlayerController::HandleStrafeRightReleased()
{
	bStrafeRightHeld = false;
	UE_LOG(LogTemp, Display, TEXT("WoW input: E/StrafeRight released"));
}

void AWoWPlayerController::HandleSitTogglePressed()
{
	if (bSittingRequested)
	{
		SetSittingRequested(false);
		return;
	}

	if (CanStartSitting())
	{
		SetSittingRequested(true);
	}
}

void AWoWPlayerController::HandleWeaponSheatheTogglePressed()
{
	if (UWoWCharacterPreviewComponent* PreviewComponent = CharacterPreviewComponent.Get())
	{
		PreviewComponent->ToggleWeaponSheathed();
	}
}

void AWoWPlayerController::HandleJumpPressed()
{
	CancelSittingForMovement();
	const bool bGroundMovementInput = bMoveForwardHeld || bMoveBackwardHeld || bStrafeLeftHeld || bStrafeRightHeld || (bLeftMouseHeld && bRightMouseHeld);
	bJumpStartedWithGroundMovement = bGroundMovementInput;
	if (ACharacter* ControlledCharacter = Cast<ACharacter>(GetPawn()))
	{
		ControlledCharacter->Jump();
	}

	if (UWoWCharacterPreviewComponent* PreviewComponent = CharacterPreviewComponent.Get())
	{
		PreviewComponent->SetJumpStartedWithGroundMovement(bJumpStartedWithGroundMovement);
		PreviewComponent->TriggerJumpStart();
	}
}

void AWoWPlayerController::HandleJumpReleased()
{
	if (ACharacter* ControlledCharacter = Cast<ACharacter>(GetPawn()))
	{
		ControlledCharacter->StopJumping();
	}
}

void AWoWPlayerController::HandleEscapePressed()
{
	CancelAutoAttack(CombatReadyLingerSeconds);
}

void AWoWPlayerController::TryRightClickAutoAttack()
{
	AWoWPreviewTargetCharacter* Target = FindPreviewTargetUnderCursor(false);
	if (!Target)
	{
		return;
	}

	SelectedAutoAttackTarget = Target;
	AutoAttackTimerSeconds = 0.0f;
	bSelectedTargetEngaged = false;

	if (UWoWCharacterPreviewComponent* PreviewComponent = CharacterPreviewComponent.Get())
	{
		PreviewComponent->SetCombatReady(true, 0.0f);
	}
}

AWoWPreviewTargetCharacter* AWoWPlayerController::FindPreviewTargetUnderCursor(bool bAllowAimFallback) const
{
	FHitResult HitResult;
	if (GetHitResultUnderCursorByChannel(UEngineTypes::ConvertToTraceType(ECC_Visibility), false, HitResult))
	{
		for (AActor* Actor = HitResult.GetActor(); Actor; Actor = Actor->GetOwner())
		{
			if (AWoWPreviewTargetCharacter* Target = Cast<AWoWPreviewTargetCharacter>(Actor))
			{
				return Target;
			}
		}
	}

	if (!bAllowAimFallback)
	{
		return nullptr;
	}

	FVector RayOrigin = FVector::ZeroVector;
	FVector RayDirection = FVector::ForwardVector;
	if (!DeprojectMousePositionToWorld(RayOrigin, RayDirection))
	{
		return nullptr;
	}

	AWoWPreviewTargetCharacter* BestTarget = nullptr;
	float BestRayDistance = TNumericLimits<float>::Max();
	for (TActorIterator<AWoWPreviewTargetCharacter> It(GetWorld()); It; ++It)
	{
		AWoWPreviewTargetCharacter* Target = *It;
		if (!IsValid(Target))
		{
			continue;
		}

		const FVector ToTarget = Target->GetActorLocation() - RayOrigin;
		const float AlongRay = FVector::DotProduct(ToTarget, RayDirection);
		if (AlongRay < 0.0f || AlongRay > 1500.0f)
		{
			continue;
		}

		const FVector ClosestPoint = RayOrigin + RayDirection * AlongRay;
		const float RayDistance = FVector::Dist(ClosestPoint, Target->GetActorLocation());
		if (RayDistance < 140.0f && RayDistance < BestRayDistance)
		{
			BestTarget = Target;
			BestRayDistance = RayDistance;
		}
	}

	return BestTarget;
}

void AWoWPlayerController::UpdateAutoAttack(float DeltaTime)
{
	AWoWPreviewTargetCharacter* Target = SelectedAutoAttackTarget.Get();
	ACharacter* ControlledCharacter = Cast<ACharacter>(GetPawn());
	UWoWCharacterPreviewComponent* PreviewComponent = CharacterPreviewComponent.Get();
	if (!Target || !ControlledCharacter || !PreviewComponent)
	{
		return;
	}

	PreviewComponent->SetCombatReady(true, 0.0f);

	const FVector ToTarget = Target->GetActorLocation() - ControlledCharacter->GetActorLocation();
	const float Distance2D = FVector(ToTarget.X, ToTarget.Y, 0.0f).Size();
	if (Distance2D > MeleeAttackRange)
	{
		AutoAttackTimerSeconds = 0.0f;
		return;
	}

	if (!bSelectedTargetEngaged)
	{
		Target->BeginAutoAttackPlayer(ControlledCharacter);
		bSelectedTargetEngaged = true;
	}

	if (!ToTarget.IsNearlyZero())
	{
		ControlledCharacter->SetActorRotation(FRotator(0.0f, ToTarget.Rotation().Yaw, 0.0f));
	}

	AutoAttackTimerSeconds -= DeltaTime;
	if (AutoAttackTimerSeconds <= 0.0f)
	{
		PreviewComponent->TriggerAttack();
		AutoAttackTimerSeconds = AutoAttackIntervalSeconds;
	}
}

void AWoWPlayerController::CancelAutoAttack(float LingerSeconds)
{
	const bool bHadAutoAttackTarget = SelectedAutoAttackTarget.IsValid();
	const bool bHadEngagedTarget = bSelectedTargetEngaged;
	if (bHadEngagedTarget)
	{
		if (AWoWPreviewTargetCharacter* Target = SelectedAutoAttackTarget.Get())
		{
			Target->StopAutoAttack(LingerSeconds);
		}
	}
	SelectedAutoAttackTarget.Reset();
	AutoAttackTimerSeconds = 0.0f;
	bSelectedTargetEngaged = false;

	if (UWoWCharacterPreviewComponent* PreviewComponent = CharacterPreviewComponent.Get())
	{
		PreviewComponent->SetCombatReady(false, bHadAutoAttackTarget ? LingerSeconds : 0.0f);
	}
}

void AWoWPlayerController::UpdateHoverCursor()
{
	if (bLeftMouseHeld || bRightMouseHeld)
	{
		return;
	}

	const bool bNowHoveringAttackTarget = FindPreviewTargetUnderCursor(false) != nullptr;
	if (bHoveringAttackTarget == bNowHoveringAttackTarget)
	{
		return;
	}

	bHoveringAttackTarget = bNowHoveringAttackTarget;
	CurrentMouseCursor = bHoveringAttackTarget ? EMouseCursor::Crosshairs : EMouseCursor::Default;
}

void AWoWPlayerController::HandleDebugPreviousAnimationPressed()
{
	if (UWoWCharacterPreviewComponent* PreviewComponent = CharacterPreviewComponent.Get())
	{
		PreviewComponent->DebugPreviousAnimation();
	}
}

void AWoWPlayerController::HandleDebugNextAnimationPressed()
{
	if (UWoWCharacterPreviewComponent* PreviewComponent = CharacterPreviewComponent.Get())
	{
		PreviewComponent->DebugNextAnimation();
	}
}

void AWoWPlayerController::HandleDebugClearAnimationPressed()
{
	if (UWoWCharacterPreviewComponent* PreviewComponent = CharacterPreviewComponent.Get())
	{
		PreviewComponent->DebugClearAnimation();
	}
}

void AWoWPlayerController::HandleDebugEquipWeaponRightHandPressed()
{
	if (UWoWCharacterPreviewComponent* PreviewComponent = CharacterPreviewComponent.Get())
	{
		PreviewComponent->DebugEquipWeaponRightHand();
	}
}

void AWoWPlayerController::HandleDebugEquipWeaponLeftHandPressed()
{
	if (UWoWCharacterPreviewComponent* PreviewComponent = CharacterPreviewComponent.Get())
	{
		PreviewComponent->DebugEquipWeaponLeftHand();
	}
}

void AWoWPlayerController::HandleDebugUnequipWeaponPressed()
{
	if (UWoWCharacterPreviewComponent* PreviewComponent = CharacterPreviewComponent.Get())
	{
		PreviewComponent->DebugUnequipWeapon();
	}
}

void AWoWPlayerController::HandleDebugCycleSheathedAttachmentPressed()
{
	if (UWoWCharacterPreviewComponent* PreviewComponent = CharacterPreviewComponent.Get())
	{
		PreviewComponent->DebugCycleSheathedAttachment();
	}
}

void AWoWPlayerController::HandleDebugCycleSheathedRotationPressed()
{
	if (UWoWCharacterPreviewComponent* PreviewComponent = CharacterPreviewComponent.Get())
	{
		PreviewComponent->DebugCycleSheathedRotation();
	}
}

void AWoWPlayerController::HandleDebugCycleSheathedNudgeModePressed()
{
	if (UWoWCharacterPreviewComponent* PreviewComponent = CharacterPreviewComponent.Get())
	{
		PreviewComponent->DebugCycleSheathedNudgeMode();
	}
}

void AWoWPlayerController::HandleDebugSheathedNudgeDecreasePressed()
{
	if (UWoWCharacterPreviewComponent* PreviewComponent = CharacterPreviewComponent.Get())
	{
		PreviewComponent->DebugAdjustSheathedNudge(-1.0f);
	}
}

void AWoWPlayerController::HandleDebugSheathedNudgeIncreasePressed()
{
	if (UWoWCharacterPreviewComponent* PreviewComponent = CharacterPreviewComponent.Get())
	{
		PreviewComponent->DebugAdjustSheathedNudge(1.0f);
	}
}

void AWoWPlayerController::HandleToggleSheathedWeaponDebugPanelPressed()
{
	ToggleSheathedWeaponDebugPanel();
}

void AWoWPlayerController::HandleSpeedIncreasePressed()
{
	SetLocalSpeedModifier(LocalSpeedModifier + WoWLocalSpeedStep, true);
}

void AWoWPlayerController::HandleSpeedDecreasePressed()
{
	SetLocalSpeedModifier(LocalSpeedModifier - WoWLocalSpeedStep, true);
}

void AWoWPlayerController::HandleSpeedResetPressed()
{
	SetLocalSpeedModifier(1.0f, true);
}

void AWoWPlayerController::ApplyCursorMode()
{
	const bool bMouseLookHeld = bLeftMouseHeld || bRightMouseHeld;
	bLastAppliedMouseLookHeld = bMouseLookHeld;

	if (bMouseLookHeld)
	{
		bShowMouseCursor = false;
		ResetIgnoreLookInput();

		FInputModeGameOnly InputMode;
		SetInputMode(InputMode);
		return;
	}

	bShowMouseCursor = true;
	ResetIgnoreLookInput();
	SetIgnoreLookInput(true);

	FInputModeGameAndUI InputMode;
	InputMode.SetWidgetToFocus(nullptr);
	InputMode.SetHideCursorDuringCapture(false);
	InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
	SetInputMode(InputMode);
}

void AWoWPlayerController::CaptureMouseLookOriginIfNeeded()
{
	if (bLeftMouseHeld || bRightMouseHeld || bHasMouseLookOrigin)
	{
		return;
	}

	float MouseX = 0.0f;
	float MouseY = 0.0f;
	if (GetMousePosition(MouseX, MouseY))
	{
		MouseLookOriginX = FMath::RoundToInt(MouseX);
		MouseLookOriginY = FMath::RoundToInt(MouseY);
		bHasMouseLookOrigin = true;
	}
}

void AWoWPlayerController::RestoreMouseLookOriginIfNeeded()
{
	if (bLeftMouseHeld || bRightMouseHeld || !bHasMouseLookOrigin)
	{
		return;
	}

	SetMouseLocation(MouseLookOriginX, MouseLookOriginY);
	bHasMouseLookOrigin = false;
}

void AWoWPlayerController::ConfigureHardwareCursor()
{
	if (UGameViewportClient* ViewportClient = GetWorld() ? GetWorld()->GetGameViewport() : nullptr)
	{
		ConfigureWoWHardwareCursors(ViewportClient);
	}
}

void AWoWPlayerController::ConfigureControlledPawn()
{
	ACharacter* ControlledCharacter = Cast<ACharacter>(GetPawn());
	if (!ControlledCharacter)
	{
		return;
	}

	ControlledCharacter->bUseControllerRotationYaw = false;
	if (UCharacterMovementComponent* Movement = ControlledCharacter->GetCharacterMovement())
	{
		Movement->bOrientRotationToMovement = false;
		Movement->bUseControllerDesiredRotation = false;
		Movement->MaxWalkSpeed = WoWForwardRunSpeed;
		Movement->MaxAcceleration = 100000.0f;
		Movement->GroundFriction = 16.0f;
		Movement->BrakingDecelerationWalking = 12000.0f;
		Movement->AirControl = WoWAirControl;
		Movement->FallingLateralFriction = WoWFallingLateralFriction;
		Movement->BrakingDecelerationFalling = WoWBrakingDecelerationFalling;
		Movement->bMaintainHorizontalGroundVelocity = true;
		Movement->bUseSeparateBrakingFriction = true;
		Movement->BrakingFriction = 24.0f;
		Movement->BrakingFrictionFactor = 1.0f;
		Movement->MaxWalkSpeed = GetLocalMovementMaxWalkSpeed(1.0f);
	}
}

void AWoWPlayerController::ConfigurePreviewRendering()
{
	auto SetConsoleVariable = [](const TCHAR* Name, const TCHAR* Value)
	{
		if (IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Name))
		{
			Variable->Set(Value, ECVF_SetByCode);
		}
	};

	SetConsoleVariable(TEXT("r.MotionBlurQuality"), TEXT("0"));
	SetConsoleVariable(TEXT("r.DefaultFeature.MotionBlur"), TEXT("0"));
	SetConsoleVariable(TEXT("r.AntiAliasingMethod"), TEXT("1"));
	SetConsoleVariable(TEXT("r.TemporalAA.Algorithm"), TEXT("0"));
	SetConsoleVariable(TEXT("r.TSR.ShadingRejection.Flickering"), TEXT("0"));
	UE_LOG(LogTemp, Display, TEXT("Configured WoW preview rendering: motion blur disabled, anti-aliasing set to FXAA"));
}

void AWoWPlayerController::EnsureCharacterPreviewComponent()
{
	ACharacter* ControlledCharacter = Cast<ACharacter>(GetPawn());
	if (!ControlledCharacter)
	{
		return;
	}

	UWoWCharacterPreviewComponent* PreviewComponent = CharacterPreviewComponent.Get();
	if (!PreviewComponent)
	{
		PreviewComponent = ControlledCharacter->FindComponentByClass<UWoWCharacterPreviewComponent>();
	}

	if (!PreviewComponent)
	{
		PreviewComponent = NewObject<UWoWCharacterPreviewComponent>(ControlledCharacter, TEXT("WoWCharacterPreview"));
		ControlledCharacter->AddInstanceComponent(PreviewComponent);
		PreviewComponent->RegisterComponent();
	}

	CharacterPreviewComponent = PreviewComponent;
	const UWoWGameInstance* WoWGameInstance = GetGameInstance<UWoWGameInstance>();
	if (!WoWGameInstance || !WoWGameInstance->HasSelectedCharacterRuntimeAppearance())
	{
		UE_LOG(LogTemp, Error, TEXT("WoW world entered without a selected character runtime appearance. This is a login flow error; character preview will not initialize."));
		return;
	}
	PreviewComponent->SetRuntimeAppearance(WoWGameInstance->GetSelectedCharacterRuntimeAppearance());
	PreviewComponent->InitializeForCharacter(ControlledCharacter);
	UpdateCharacterPreviewMovementState();
}

void AWoWPlayerController::UpdateCharacterPreviewMovementState()
{
	if (UWoWCharacterPreviewComponent* PreviewComponent = CharacterPreviewComponent.Get())
	{
		const float PreviewForwardInput = bHasAirborneVisualInput ? AirborneVisualForwardInput : CurrentForwardInput;
		const float PreviewStrafeInput = bHasAirborneVisualInput ? AirborneVisualStrafeInput : CurrentStrafeInput;
		PreviewComponent->SetMovementInputState(
			bMoveForwardHeld,
			bMoveBackwardHeld,
			bStrafeLeftHeld,
			bStrafeRightHeld,
			bTurnLeftHeld,
			bTurnRightHeld,
			MouseTurnAnimationSeconds > 0.0f && LastMouseTurnDirection < 0.0f,
			MouseTurnAnimationSeconds > 0.0f && LastMouseTurnDirection > 0.0f,
			bLeftMouseHeld,
			bRightMouseHeld,
			PreviewForwardInput,
			PreviewStrafeInput);
	}
}

void AWoWPlayerController::FaceControlledPawnToControlYaw()
{
	APawn* ControlledPawn = GetPawn();
	if (!ControlledPawn)
	{
		CurrentForwardInput = 0.0f;
		CurrentStrafeInput = 0.0f;
		return;
	}

	const FRotator CurrentControlRotation = GetControlRotation();
	ControlledPawn->SetActorRotation(FRotator(0.0f, CurrentControlRotation.Yaw, 0.0f));
}

void AWoWPlayerController::ApplyKeyboardMovement(float DeltaTime)
{
	APawn* ControlledPawn = GetPawn();
	if (!ControlledPawn)
	{
		return;
	}

	float ForwardInput = 0.0f;
	ForwardInput += bMoveForwardHeld ? 1.0f : 0.0f;
	ForwardInput -= bMoveBackwardHeld ? 1.0f : 0.0f;
	ForwardInput += (bLeftMouseHeld && bRightMouseHeld) ? 1.0f : 0.0f;
	ForwardInput = FMath::Clamp(ForwardInput, -1.0f, 1.0f);

	float StrafeInput = 0.0f;
	StrafeInput -= bStrafeLeftHeld ? 1.0f : 0.0f;
	StrafeInput += bStrafeRightHeld ? 1.0f : 0.0f;

	float TurnInput = (bTurnRightHeld ? 1.0f : 0.0f) - (bTurnLeftHeld ? 1.0f : 0.0f);
	if (bSittingRequested && (!FMath::IsNearlyZero(ForwardInput) || !FMath::IsNearlyZero(StrafeInput) || !FMath::IsNearlyZero(TurnInput)))
	{
		CancelSittingForMovement();
	}
	if (bRightMouseHeld)
	{
		StrafeInput += TurnInput;
	}
	else if (!FMath::IsNearlyZero(TurnInput))
	{
		if (bLeftMouseHeld || bCameraReturnToPawnYawActive)
		{
			// WoW左键按住时锁定镜头：A/D只让角色自身转向，不推进ControlRotation带动相机。
			FRotator NewPawnRotation = ControlledPawn->GetActorRotation();
			NewPawnRotation.Yaw += TurnInput * KeyboardTurnRateDegrees * DeltaTime;
			ControlledPawn->SetActorRotation(FRotator(0.0f, NewPawnRotation.Yaw, 0.0f));
			if (bLeftMouseHeld)
			{
				bLeftMouseKeyboardTurnedPawn = true;
			}
		}
		else
		{
			FRotator NewControlRotation = GetControlRotation();
			NewControlRotation.Yaw += TurnInput * KeyboardTurnRateDegrees * DeltaTime;
			SetControlRotation(NewControlRotation);
			FaceControlledPawnToControlYaw();
		}
	}

	if (bRightMouseHeld)
	{
		FaceControlledPawnToControlYaw();
	}

	StrafeInput = FMath::Clamp(StrafeInput, -1.0f, 1.0f);
	CurrentForwardInput = ForwardInput;
	CurrentStrafeInput = StrafeInput;
	if (ACharacter* ControlledCharacter = Cast<ACharacter>(ControlledPawn))
	{
		if (UCharacterMovementComponent* Movement = ControlledCharacter->GetCharacterMovement())
		{
			Movement->MaxWalkSpeed = GetLocalMovementMaxWalkSpeed(ForwardInput);
		}
	}

	const FRotator YawOnlyRotation(0.0f, ControlledPawn->GetActorRotation().Yaw, 0.0f);
	const bool bHasMovementInput = !FMath::IsNearlyZero(ForwardInput) || !FMath::IsNearlyZero(StrafeInput);
	if (ACharacter* ControlledCharacter = Cast<ACharacter>(ControlledPawn))
	{
		if (UCharacterMovementComponent* Movement = ControlledCharacter->GetCharacterMovement())
		{
			if (Movement->IsMovingOnGround())
			{
				bHasAirborneInertiaVelocity = false;
				bJumpStartedWithGroundMovement = false;
				bHasAirborneVisualInput = false;
				AirborneVisualForwardInput = 0.0f;
				AirborneVisualStrafeInput = 0.0f;
				FVector Velocity = Movement->Velocity;
				if (bHasMovementInput)
				{
					const FVector MoveDirection = BuildWoWKeyboardMovementDirection(YawOnlyRotation, ForwardInput, StrafeInput);
					const float TargetSpeed = Movement->MaxWalkSpeed;
					Velocity.X = MoveDirection.X * TargetSpeed;
					Velocity.Y = MoveDirection.Y * TargetSpeed;
				}
				else
				{
					Velocity.X = 0.0f;
					Velocity.Y = 0.0f;
				}
				Movement->Velocity = Velocity;
			}
			else if (Movement->IsFalling())
			{
				if (!bHasAirborneVisualInput && bHasMovementInput)
				{
					// 空中点按W/S/Q/E后，即使松开按键，也保持这次空中姿态到落地；否则Q/E会立刻回正。
					AirborneVisualForwardInput = ForwardInput;
					AirborneVisualStrafeInput = StrafeInput;
					bHasAirborneVisualInput = true;
				}
				if (!bHasAirborneInertiaVelocity)
				{
					AirborneInertiaVelocity = FVector(Movement->Velocity.X, Movement->Velocity.Y, 0.0f);
					if (AirborneInertiaVelocity.IsNearlyZero() && bHasMovementInput)
					{
						const FVector MoveDirection = BuildWoWKeyboardMovementDirection(YawOnlyRotation, ForwardInput, StrafeInput);
						AirborneInertiaVelocity = MoveDirection * Movement->MaxWalkSpeed * WoWStandingJumpAirInputSpeedScale;
					}
					AirborneInertiaVelocity.Z = 0.0f;
					bHasAirborneInertiaVelocity = true;
				}
				else if (AirborneInertiaVelocity.IsNearlyZero() && bHasMovementInput)
				{
					// 原地起跳后才按W/S/Q/E时，允许第一次空中位移输入生成水平惯性；跑动起跳已有惯性时不改方向。
					const FVector MoveDirection = BuildWoWKeyboardMovementDirection(YawOnlyRotation, ForwardInput, StrafeInput);
					AirborneInertiaVelocity = MoveDirection * Movement->MaxWalkSpeed * WoWStandingJumpAirInputSpeedScale;
				}

				FVector Velocity = Movement->Velocity;
				Velocity.X = AirborneInertiaVelocity.X;
				Velocity.Y = AirborneInertiaVelocity.Y;
				Movement->Velocity = Velocity;
			}
		}
	}

	if (!FMath::IsNearlyZero(ForwardInput))
	{
		ControlledPawn->AddMovementInput(YawOnlyRotation.Vector(), ForwardInput);
	}
	if (!FMath::IsNearlyZero(StrafeInput))
	{
		ControlledPawn->AddMovementInput(FRotationMatrix(YawOnlyRotation).GetUnitAxis(EAxis::Y), StrafeInput);
	}
}

void AWoWPlayerController::StartCameraReturnToPawnYaw()
{
	if (bRightMouseHeld)
	{
		return;
	}

	const APawn* ControlledPawn = GetPawn();
	if (!ControlledPawn)
	{
		return;
	}

	const float YawDelta = FMath::FindDeltaAngleDegrees(GetControlRotation().Yaw, ControlledPawn->GetActorRotation().Yaw);
	bCameraReturnToPawnYawActive = FMath::Abs(YawDelta) > 0.5f;
}

void AWoWPlayerController::UpdateCameraReturnToPawnYaw(float DeltaTime)
{
	if (!bCameraReturnToPawnYawActive || bLeftMouseHeld || bRightMouseHeld)
	{
		return;
	}

	const APawn* ControlledPawn = GetPawn();
	if (!ControlledPawn)
	{
		bCameraReturnToPawnYawActive = false;
		return;
	}

	FRotator NewControlRotation = GetControlRotation();
	const float TargetYaw = ControlledPawn->GetActorRotation().Yaw;
	const float NewYaw = FMath::FixedTurn(
		NewControlRotation.Yaw,
		TargetYaw,
		FMath::Max(CameraReturnToPawnYawSpeedDegrees, 1.0f) * DeltaTime);
	NewControlRotation.Yaw = NewYaw;
	SetControlRotation(NewControlRotation);

	if (FMath::Abs(FMath::FindDeltaAngleDegrees(NewYaw, TargetYaw)) <= 0.5f)
	{
		NewControlRotation.Yaw = TargetYaw;
		SetControlRotation(NewControlRotation);
		bCameraReturnToPawnYawActive = false;
	}
}

void AWoWPlayerController::SetSittingRequested(bool bInSitting, bool bPlayTransition)
{
	bSittingRequested = bInSitting;
	if (UWoWCharacterPreviewComponent* PreviewComponent = CharacterPreviewComponent.Get())
	{
		PreviewComponent->SetSitting(bSittingRequested, bPlayTransition);
	}
}

void AWoWPlayerController::CancelSittingForMovement()
{
	if (!bSittingRequested)
	{
		return;
	}

	// WoW里坐下后按移动会立刻按移动逻辑接管，不完整播放起立过渡。
	SetSittingRequested(false, false);
}

bool AWoWPlayerController::CanStartSitting() const
{
	if (bMoveForwardHeld ||
		bMoveBackwardHeld ||
		bTurnLeftHeld ||
		bTurnRightHeld ||
		bStrafeLeftHeld ||
		bStrafeRightHeld ||
		(bLeftMouseHeld && bRightMouseHeld))
	{
		return false;
	}

	const ACharacter* ControlledCharacter = Cast<ACharacter>(GetPawn());
	const UCharacterMovementComponent* Movement = ControlledCharacter ? ControlledCharacter->GetCharacterMovement() : nullptr;
	if (!Movement)
	{
		return false;
	}

	// X坐下只允许在地面静止时触发；跳跃/下落/空中补方向时按X应被忽略。
	return Movement->IsMovingOnGround() && Movement->Velocity.SizeSquared2D() <= 1.0f;
}

void AWoWPlayerController::SetLocalSpeedModifier(float NewSpeedModifier, bool bShowFeedback)
{
	LocalSpeedModifier = FMath::Clamp(NewSpeedModifier, WoWMinLocalSpeedModifier, WoWMaxLocalSpeedModifier);
	if (ACharacter* ControlledCharacter = Cast<ACharacter>(GetPawn()))
	{
		if (UCharacterMovementComponent* Movement = ControlledCharacter->GetCharacterMovement())
		{
			Movement->MaxWalkSpeed = GetLocalMovementMaxWalkSpeed(CurrentForwardInput);
		}
	}

	if (bShowFeedback)
	{
		const FString Message = FString::Printf(TEXT("WoW local speed: %.1fx"), LocalSpeedModifier);
		UE_LOG(LogTemp, Display, TEXT("%s"), *Message);
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(INDEX_NONE, 1.5f, FColor::Cyan, Message);
		}
	}
}

float AWoWPlayerController::GetLocalMovementMaxWalkSpeed(float ForwardInput) const
{
	const bool bBackpedaling = ForwardInput < -0.1f;
	const float DirectionScale = bBackpedaling ? WoWBackpedalSpeedScale : 1.0f;
	return WoWForwardRunSpeed * FMath::Clamp(DirectionScale, 0.1f, 1.0f) * LocalSpeedModifier;
}

void AWoWPlayerController::ZoomCamera(float WheelValue)
{
	APawn* ControlledPawn = GetPawn();
	if (!ControlledPawn)
	{
		return;
	}

	if (USpringArmComponent* SpringArm = ControlledPawn->FindComponentByClass<USpringArmComponent>())
	{
		const float NewArmLength = SpringArm->TargetArmLength - WheelValue * CameraZoomStep;
		SpringArm->TargetArmLength = FMath::Clamp(NewArmLength, MinCameraDistance, MaxCameraDistance);
	}
}

void AWoWPlayerController::ToggleSheathedWeaponDebugPanel()
{
	if (!GEngine || !GEngine->GameViewport)
	{
		return;
	}

	if (SheathedWeaponDebugPanelWidget.IsValid())
	{
		GEngine->GameViewport->RemoveViewportWidgetContent(SheathedWeaponDebugPanelWidget.ToSharedRef());
		SheathedWeaponDebugPanelWidget.Reset();
		ApplyCursorMode();
		return;
	}

	EnsureCharacterPreviewComponent();
	UWoWCharacterPreviewComponent* PreviewComponent = CharacterPreviewComponent.Get();
	if (!PreviewComponent)
	{
		return;
	}

	PreviewComponent->SetDebugWeaponSheathedPreview(true);
	SheathedWeaponDebugPanelWidget = SNew(SOverlay)
		+ SOverlay::Slot()
		.HAlign(HAlign_Left)
		.VAlign(VAlign_Top)
		.Padding(16.0f)
		[
			SNew(SWoWSheathedWeaponDebugPanel)
			.PreviewComponent(PreviewComponent)
		];
	GEngine->GameViewport->AddViewportWidgetContent(SheathedWeaponDebugPanelWidget.ToSharedRef(), 20);

	FInputModeGameAndUI InputMode;
	InputMode.SetWidgetToFocus(SheathedWeaponDebugPanelWidget);
	InputMode.SetHideCursorDuringCapture(false);
	InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
	SetInputMode(InputMode);
	bShowMouseCursor = true;
}
