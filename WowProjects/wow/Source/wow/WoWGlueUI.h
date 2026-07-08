#pragma once

#include "CoreMinimal.h"
#include "Fonts/SlateFontInfo.h"
#include "Framework/Text/SlateHyperlinkRun.h"
#include "Input/Reply.h"
#include "Templates/SharedPointer.h"
#include "Types/SlateStructs.h"
#include "Widgets/Layout/Anchors.h"
#include "Widgets/SCompoundWidget.h"

class SConstraintCanvas;
class SWidget;
class SEditableTextBox;
class FSlateStyleSet;
class UTexture2D;
class FWoWGlueUIManager;

enum class EWoWGlueFrameType : uint8
{
	Frame,
	Button,
	EditBox,
	FontString,
	ScrollFrame,
	SimpleHTML,
	Texture,
	Unknown
};

enum class EWoWGlueAlphaMode : uint8
{
	Blend,
	Add,
	AlphaKey,
	Disable,
	Mod
};

enum class EWoWGlueTextGradientMode : uint8
{
	None,
	Horizontal,
	Vertical
};

struct WOW_API FWoWGluePoint
{
	FString Point = TEXT("CENTER");
	FString RelativeTo = TEXT("UIParent");
	FString RelativePoint = TEXT("CENTER");
	FVector2D Offset = FVector2D::ZeroVector;
};

struct WOW_API FWoWGlueUVRect
{
	float Left = 0.0f;
	float Right = 1.0f;
	float Top = 0.0f;
	float Bottom = 1.0f;
	bool bValid = false;
};

struct WOW_API FWoWGlueScrollBarVisualState
{
	FWoWGlueUVRect ThumbTop;
	FWoWGlueUVRect ThumbMiddle;
	FWoWGlueUVRect ThumbBottom;
	FWoWGlueUVRect UpArrow;
	FWoWGlueUVRect DownArrow;
};

struct WOW_API FWoWGlueScrollBarStyle
{
	float Width = 12.0f;
	float ThumbWidth = 8.0f;
	float ThumbMinHeight = 28.0f;
	float ThumbCapHeight = 0.0f;
	float ButtonHeight = 0.0f;
	float TrackInset = 2.0f;
	float TrackBackgroundWidth = 0.0f;
	float TrackBackgroundAlpha = 0.42f;
	float Step = 48.0f;
	UTexture2D* ThumbTopBottomTexture = nullptr;
	UTexture2D* ThumbMiddleTexture = nullptr;
	UTexture2D* ArrowTexture = nullptr;
	FString ThumbTopBottomTexturePath;
	FString ThumbMiddleTexturePath;
	FString ArrowTexturePath;
	FWoWGlueScrollBarVisualState Normal;
	FWoWGlueScrollBarVisualState Highlight;
	FWoWGlueScrollBarVisualState Pushed;
};

class WOW_API FWoWGlueFrame : public TSharedFromThis<FWoWGlueFrame>
{
public:
	using FScriptHandler = TFunction<void(TSharedRef<FWoWGlueFrame>)>;
	using FHyperlinkScriptHandler = TFunction<void(TSharedRef<FWoWGlueFrame>, const FString&, const FString&)>;

	FWoWGlueFrame(EWoWGlueFrameType InFrameType, FString InTypeName, FName InName, TSharedPtr<FWoWGlueFrame> InParent);

	const FString& GetTypeName() const { return TypeName; }
	EWoWGlueFrameType GetFrameType() const { return FrameType; }
	FName GetName() const { return Name; }
	TSharedPtr<FWoWGlueFrame> GetParent() const { return Parent.Pin(); }
	const TArray<TSharedRef<FWoWGlueFrame>>& GetChildren() const { return Children; }
	const FWoWGluePoint& GetPoint() const { return Point; }
	FVector2D GetSize() const { return Size; }
	bool HasExplicitSize() const { return bHasExplicitSize; }
	FText GetText() const { return Text; }
	bool IsShown() const { return bShown; }
	bool IsEnabled() const { return bEnabled; }
	bool IsMouseEnabled() const { return bMouseEnabled; }
	bool IsPassword() const { return bPassword; }
	bool KeepsKeyboardFocus() const { return bKeepKeyboardFocus; }
	float GetAlpha() const { return Alpha; }

	FWoWGlueFrame& SetPoint(const FString& InPoint, TSharedPtr<FWoWGlueFrame> InRelativeTo, const FString& InRelativePoint, float XOffset, float YOffset);
	FWoWGlueFrame& SetSize(float Width, float Height);
	FWoWGlueFrame& SetAllPoints(bool bInSetAllPoints);
	FWoWGlueFrame& SetText(const FText& InText);
	FWoWGlueFrame& SetText(const FString& InText);
	FString GetTextString() const;
	FWoWGlueFrame& SetAlpha(float InAlpha);
	FWoWGlueFrame& SetVertexColor(float R, float G, float B, float A = 1.0f);
	FWoWGlueFrame& SetPassword(bool bInPassword);
	FWoWGlueFrame& SetPlaceholderText(const FString& InPlaceholderText);
	FWoWGlueFrame& SetPlaceholderTextColor(float R, float G, float B, float A = 1.0f);
	FWoWGlueFrame& SetPlaceholderJustifyH(const FString& InJustifyH);
	FWoWGlueFrame& SetTextInsets(float Left, float Right, float Top, float Bottom);
	FWoWGlueFrame& SetTextColor(float R, float G, float B, float A = 1.0f);
	FWoWGlueFrame& SetFont(const FString& FontPath, float FontSize, const FString& Flags = FString());
	FWoWGlueFrame& SetFontObject(const FString& FontObjectName);
	FWoWGlueFrame& SetShadowColor(float R, float G, float B, float A = 1.0f);
	FWoWGlueFrame& SetShadowOffset(float XOffset, float YOffset);
	FWoWGlueFrame& SetJustifyH(const FString& InJustifyH);
	FWoWGlueFrame& SetJustifyV(const FString& InJustifyV);
	FWoWGlueFrame& SetTextFirstLineIndent(float InFirstLineIndent);
	FWoWGlueFrame& SetTextFirstLineIndentChars(float InFirstLineIndentChars);
	FWoWGlueFrame& SetTextParagraphSpacing(float InParagraphSpacing);
	FWoWGlueFrame& SetTextLineHeightPercentage(float InLineHeightPercentage);
	FWoWGlueFrame& SetTextJustifyLastLine(bool bInJustifyLastLine);
	FWoWGlueFrame& SetSimpleHTMLLineSpacing(float InLineSpacing);
	FWoWGlueFrame& SetTextGradient(const FString& Direction, const FLinearColor& StartColor, const FLinearColor& EndColor);
	FWoWGlueFrame& ClearAllPoints();
	FWoWGlueFrame& Raise();
	FWoWGlueFrame& SetScript(const FString& ScriptName, FScriptHandler Handler);
	FWoWGlueFrame& SetHyperlinkScript(FHyperlinkScriptHandler Handler);
	FWoWGlueFrame& SetScrollChild(TSharedPtr<FWoWGlueFrame> InScrollChild);
	FWoWGlueFrame& SetVerticalScroll(float InVerticalScroll);
	FWoWGlueFrame& SetScrollBarStyle(const FWoWGlueScrollBarStyle& InScrollBarStyle);
	FWoWGlueFrame& Show();
	FWoWGlueFrame& Hide();
	FWoWGlueFrame& SetEnabled(bool bInEnabled);
	FWoWGlueFrame& SetMouseEnabled(bool bInMouseEnabled);
	FWoWGlueFrame& SetKeepKeyboardFocus(bool bInKeepKeyboardFocus);
	FWoWGlueFrame& SetDrawLayer(int32 InDrawLayer);
	void SetKeyboardFocus();
	void SetKeyboardFocus(bool bSelectAllText);
	FWoWGlueFrame& SetTexture(const FString& TexturePath);
	FWoWGlueFrame& SetTexture(UTexture2D* Texture, const FString& TexturePath, EWoWGlueAlphaMode AlphaMode = EWoWGlueAlphaMode::Blend);
	FWoWGlueFrame& SetTexCoord(float Left, float Right, float Top, float Bottom);
	FWoWGlueFrame& SetNormalTexture(const FString& TexturePath);
	FWoWGlueFrame& SetPushedTexture(const FString& TexturePath);
	FWoWGlueFrame& SetHighlightTexture(const FString& TexturePath);
	FWoWGlueFrame& SetNormalTexture(UTexture2D* Texture, const FString& TexturePath);
	FWoWGlueFrame& SetPushedTexture(UTexture2D* Texture, const FString& TexturePath);
	FWoWGlueFrame& SetHighlightTexture(UTexture2D* Texture, const FString& TexturePath, EWoWGlueAlphaMode AlphaMode = EWoWGlueAlphaMode::Add);
	FWoWGlueFrame& SetBackdrop(UTexture2D* BackgroundTexture, const FString& BackgroundPath, UTexture2D* EdgeTexture, const FString& EdgePath, float EdgeSize, const FMargin& Insets);
	int32 GetDrawLayer() const { return DrawLayer; }
	int32 GetEffectiveZOrder() const;
	bool IsEffectivelyShown() const;

	void AddChild(TSharedRef<FWoWGlueFrame> Child);
	void SetOwnerManager(TSharedPtr<FWoWGlueUIManager> InOwnerManager);
	TSharedRef<SWidget> BuildSlateWidget();
	TSharedRef<SWidget> BuildSubtreeWidget();
	FMargin GetCanvasOffset() const;
	FVector2D GetCanvasAlignment() const;
	FAnchors GetCanvasAnchors() const;
	EVisibility GetSlateVisibility() const;
	EVisibility GetNonInteractiveSlateVisibility() const;
	EVisibility GetHighlightVisibility() const;
	FSlateColor GetHighlightSlateColor() const;
	FSlateColor GetTextureSlateColor() const;
	FLinearColor GetBackdropBackgroundTint() const;
	FSlateFontInfo GetSlateFont() const;
	const FString& GetFontPath() const { return FontPath; }
	float GetFontSize() const { return FontSize; }
	FSlateColor GetTextSlateColor() const;
	FSlateColor GetEditBoxInputSlateColor() const;
	FLinearColor GetTextColor() const { return TextColor; }
	FLinearColor GetShadowColor() const { return ShadowColor; }
	FVector2D GetShadowOffset() const { return ShadowOffset; }
	float GetTextFirstLineIndent() const { return TextFirstLineIndent; }
	float GetTextParagraphSpacing() const { return TextParagraphSpacing; }
	bool ShouldTextJustifyLastLine() const { return bTextJustifyLastLine; }
	float GetSimpleHTMLLineSpacing() const { return SimpleHTMLLineSpacing; }
	FText GetPlaceholderText() const;
	EVisibility GetPlaceholderVisibility() const;
	FSlateColor GetPlaceholderSlateColor() const;
	ETextJustify::Type GetPlaceholderTextJustification() const;
	FText GetSimpleHTMLRichText() const;
	TSharedPtr<FWoWGlueFrame> GetScrollChild() const { return ScrollChild.Pin(); }
	float GetVerticalScroll() const { return VerticalScroll; }
	float GetVerticalScrollRange() const;
	const FWoWGlueScrollBarStyle& GetScrollBarStyle() const { return ScrollBarStyle; }
	FVector2D MeasureDesiredContentSize() const;
	FVector2D GetLuaReportedSize() const;
	FVector2D GetLayoutSize() const;
	const FString& GetJustifyH() const { return JustifyH; }
	FOptionalSize GetSlateWidthOverride() const;
	FOptionalSize GetSlateHeightOverride() const;
	bool GetSimpleHTMLAutoWrapText() const;
	float GetSimpleHTMLWrapTextAt() const;
	float GetSimpleHTMLLineHeightPercentage() const;
	ETextJustify::Type GetTextJustification() const;
	EVerticalAlignment GetVerticalAlignment() const;
	FReply HandleClicked();
	void Click();
	void TriggerDoubleClick();
	void TriggerHyperlinkClick(const FString& Link, const FString& LinkText);
	void HandleTextChanged(const FText& InText);
	FReply HandleEditKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent);
	void HandleEditFocusGained();
	void HandleEditFocusLost();
	FReply HandleEditTabPressed();
	FReply HandleEditEnterPressed();
	FReply HandleEditEscapePressed();
	void HandleMouseEnter();
	void HandleMouseLeave();
	void HandleHyperlinkClicked(const FSlateHyperlinkRun::FMetadata& Metadata);
	void HandleMouseWheel(float WheelDelta);

private:
	static EWoWGlueFrameType ParseFrameType(const FString& TypeName);
	FVector2D MeasureNaturalTextSize() const;
	void CollectSubtreeFrames(TSharedRef<FWoWGlueFrame> Frame, TArray<TSharedRef<FWoWGlueFrame>>& Frames, TMap<FString, TSharedRef<FWoWGlueFrame>>& FramesByName) const;
	FVector2D ResolveSubtreeTopLeft(
		TSharedRef<FWoWGlueFrame> Frame,
		const TMap<FString, TSharedRef<FWoWGlueFrame>>& FramesByName,
		TMap<const FWoWGlueFrame*, FVector2D>& PositionCache,
		int32 Depth = 0) const;

	EWoWGlueFrameType FrameType = EWoWGlueFrameType::Unknown;
	FString TypeName;
	FName Name;
	TWeakPtr<FWoWGlueFrame> Parent;
	TWeakPtr<FWoWGlueFrame> ScrollChild;
	TWeakPtr<FWoWGlueUIManager> OwnerManager;
	TArray<TSharedRef<FWoWGlueFrame>> Children;
	FWoWGluePoint Point;
	FVector2D Size = FVector2D(160.0f, 32.0f);
	bool bHasExplicitSize = false;
	bool bSetAllPoints = false;
	int32 RaiseOrder = 0;
	FText Text = FText::GetEmpty();
	FText PlaceholderText = FText::GetEmpty();
	FLinearColor PlaceholderTextColor = FLinearColor(0.45f, 0.45f, 0.45f, 1.0f);
	FString PlaceholderJustifyH = TEXT("LEFT");
	bool bShown = true;
	bool bEnabled = true;
	bool bMouseEnabled = false;
	bool bPassword = false;
	bool bEditFocused = false;
	bool bHovered = false;
	bool bPendingKeyboardFocus = false;
	bool bKeepKeyboardFocus = false;
	int32 DrawLayer = 0;
	FString FontPath = TEXT("Fonts\\zykai_t.TTF");
	FString FontFlags;
	float FontSize = 14.0f;
	float Alpha = 1.0f;
	FLinearColor VertexColor = FLinearColor::White;
	bool bHasExplicitVertexColor = false;
	FLinearColor TextColor = FLinearColor(0.95f, 0.84f, 0.22f, 1.0f);
	FLinearColor ShadowColor = FLinearColor(0.0f, 0.0f, 0.0f, 0.0f);
	FVector2D ShadowOffset = FVector2D::ZeroVector;
	FString JustifyH = TEXT("CENTER");
	FString JustifyV = TEXT("MIDDLE");
	float TextFirstLineIndent = 0.0f;
	float TextParagraphSpacing = 0.0f;
	float TextLineHeightPercentage = 1.08f;
	float SimpleHTMLLineSpacing = 0.0f;
	float VerticalScroll = 0.0f;
	FWoWGlueScrollBarStyle ScrollBarStyle;
	bool bTextJustifyLastLine = false;
	EWoWGlueTextGradientMode TextGradientMode = EWoWGlueTextGradientMode::None;
	FLinearColor TextGradientStart = FLinearColor::White;
	FLinearColor TextGradientEnd = FLinearColor::White;
	FString NormalTexturePath;
	FString PushedTexturePath;
	FString HighlightTexturePath;
	FString MainTexturePath;
	FWoWGlueUVRect MainTextureUVRect;
	FString BackdropBackgroundPath;
	FString BackdropEdgePath;
	EWoWGlueAlphaMode MainTextureAlphaMode = EWoWGlueAlphaMode::Blend;
	EWoWGlueAlphaMode HighlightAlphaMode = EWoWGlueAlphaMode::Add;
	UTexture2D* NormalTexture = nullptr;
	UTexture2D* PushedTexture = nullptr;
	UTexture2D* HighlightTexture = nullptr;
	UTexture2D* MainTexture = nullptr;
	UTexture2D* BackdropBackgroundTexture = nullptr;
	UTexture2D* BackdropEdgeTexture = nullptr;
	float BackdropEdgeSize = 16.0f;
	FMargin BackdropInsets = FMargin(10.0f, 4.0f, 5.0f, 9.0f);
	FMargin TextInsets = FMargin(12.0f, 0.0f, 5.0f, 5.0f);
	TSharedPtr<FSlateBrush> NormalBrush;
	TSharedPtr<FSlateBrush> PushedBrush;
	TSharedPtr<FSlateBrush> HighlightBrush;
	TSharedPtr<FSlateBrush> TextureBrush;
	TSharedPtr<FSlateBrush> BackdropBackgroundBrush;
	TSharedPtr<FSlateBrush> BackdropEdgeBrush;
	TArray<TSharedPtr<FSlateBrush>> BackdropAtlasBrushes;
	TSharedPtr<FTextBlockStyle> TextStyle;
	TSharedPtr<FButtonStyle> ButtonStyle;
	TSharedPtr<FEditableTextBoxStyle> EditBoxStyle;
	TSharedPtr<FTextBlockStyle> SimpleHTMLTextStyle;
	TSharedPtr<FSlateStyleSet> SimpleHTMLStyleSet;
	TWeakPtr<SEditableTextBox> EditableTextBoxWidget;
	FScriptHandler OnClickScript;
	FScriptHandler OnDoubleClickScript;
	FScriptHandler OnTextChangedScript;
	FScriptHandler OnEditFocusGainedScript;
	FScriptHandler OnEditFocusLostScript;
	FScriptHandler OnTabPressedScript;
	FScriptHandler OnEnterPressedScript;
	FScriptHandler OnEscapePressedScript;
	FScriptHandler OnEnterScript;
	FScriptHandler OnLeaveScript;
	FHyperlinkScriptHandler OnHyperlinkClickScript;
	double LastClickTimeSeconds = -1.0;

	static int32 NextRaiseOrder;
};

class WOW_API FWoWGlueUIManager : public TSharedFromThis<FWoWGlueUIManager>
{
public:
	FWoWGlueUIManager();
	~FWoWGlueUIManager();

	TSharedRef<FWoWGlueFrame> CreateFrame(const FString& TypeName, const FString& Name, TSharedPtr<FWoWGlueFrame> Parent = nullptr);
	TSharedPtr<FWoWGlueFrame> FindFrame(const FString& Name) const;
	TSharedRef<FWoWGlueFrame> GetUIParent() const { return UIParent.ToSharedRef(); }
	const TArray<TSharedRef<FWoWGlueFrame>>& GetFramesInCreationOrder() const { return FramesInCreationOrder; }
	FVector2D GetScreenSize() const;
	TSharedRef<SWidget> MakeSlateWidget();
	void UpdateRootFrameSize();
	void SetActiveEditBox(TSharedPtr<FWoWGlueFrame> InEditBox);
	TSharedPtr<FWoWGlueFrame> GetActiveEditBox() const;
	void RestoreActiveEditBoxFocus();
	void RestoreActiveEditBoxFocusDeferred(TSharedRef<SWidget> TimerHost);
	FString ResolveDataPath(const FString& WowPath) const;
	UTexture2D* LoadTexture(const FString& WowPath, EWoWGlueAlphaMode AlphaMode = EWoWGlueAlphaMode::Blend);

private:
	bool IsOwnedScrollChild(TSharedRef<FWoWGlueFrame> Frame) const;
	FAnchors ResolveCanvasAnchors(TSharedRef<FWoWGlueFrame> Frame, int32 Depth = 0) const;
	FVector2D ResolveCanvasAlignment(TSharedRef<FWoWGlueFrame> Frame, int32 Depth = 0) const;
	FVector2D ResolveCanvasOffset(TSharedRef<FWoWGlueFrame> Frame, int32 Depth = 0) const;
	TSharedPtr<FWoWGlueFrame> ResolveRelativeFrame(const FWoWGlueFrame& Frame) const;

	TSharedPtr<FWoWGlueFrame> UIParent;
	TMap<FString, TSharedRef<FWoWGlueFrame>> FramesByName;
	TArray<TSharedRef<FWoWGlueFrame>> FramesInCreationOrder;
	TMap<FString, UTexture2D*> TextureCache;
	TWeakPtr<FWoWGlueFrame> ActiveEditBox;
};

class WOW_API SWoWGlueRoot : public SCompoundWidget
{
public:
	using FPointerHandler = TDelegate<FReply(const FGeometry&, const FPointerEvent&)>;
	SLATE_BEGIN_ARGS(SWoWGlueRoot) {}
		SLATE_ARGUMENT(TSharedPtr<FWoWGlueUIManager>, GlueManager)
		SLATE_EVENT(FPointerHandler, OnRootMouseButtonDown)
		SLATE_EVENT(FPointerHandler, OnRootMouseButtonUp)
		SLATE_EVENT(FPointerHandler, OnRootMouseMove)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	void SetGlueManager(TSharedPtr<FWoWGlueUIManager> InGlueManager);
	void Refresh();

	virtual FReply OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnMouseMove(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;

private:
	TSharedPtr<FWoWGlueUIManager> GlueManager;
	FPointerHandler RootMouseButtonDownHandler;
	FPointerHandler RootMouseButtonUpHandler;
	FPointerHandler RootMouseMoveHandler;
};
