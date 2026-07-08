#include "WoWGlueUI.h"

#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/Texture2D.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Text/SlateHyperlinkRun.h"
#include "Fonts/FontMeasure.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Input/Reply.h"
#include "Input/CursorReply.h"
#include "InputCoreTypes.h"
#include "Json.h"
#include "Modules/ModuleManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Styling/SlateStyle.h"
#include "Rendering/DrawElements.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SConstraintCanvas.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SLeafWidget.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/SRichTextBlock.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
struct FGlueRenderPolicy
{
	float AddDrawTintMultiplier = 2.4f;
	float AddLoadRgbMultiplier = 2.0f;
	float AddLoadAlphaMultiplier = 1.45f;
};

FGlueRenderPolicy GGlueRenderPolicy;
constexpr float DefaultAddDrawTintMultiplier = 2.4f;

EWoWGlueFrameType ParseGlueFrameTypeName(const FString& TypeName)
{
	if (TypeName.Equals(TEXT("Frame"), ESearchCase::IgnoreCase))
	{
		return EWoWGlueFrameType::Frame;
	}
	if (TypeName.Equals(TEXT("Button"), ESearchCase::IgnoreCase))
	{
		return EWoWGlueFrameType::Button;
	}
	if (TypeName.Equals(TEXT("EditBox"), ESearchCase::IgnoreCase))
	{
		return EWoWGlueFrameType::EditBox;
	}
	if (TypeName.Equals(TEXT("FontString"), ESearchCase::IgnoreCase))
	{
		return EWoWGlueFrameType::FontString;
	}
	if (TypeName.Equals(TEXT("ScrollFrame"), ESearchCase::IgnoreCase))
	{
		return EWoWGlueFrameType::ScrollFrame;
	}
	if (TypeName.Equals(TEXT("SimpleHTML"), ESearchCase::IgnoreCase))
	{
		return EWoWGlueFrameType::SimpleHTML;
	}
	if (TypeName.Equals(TEXT("Texture"), ESearchCase::IgnoreCase))
	{
		return EWoWGlueFrameType::Texture;
	}
	return EWoWGlueFrameType::Unknown;
}

FString NormalizeGluePoint(FString Value)
{
	Value.TrimStartAndEndInline();
	Value.ToUpperInline();
	return Value.IsEmpty() ? TEXT("CENTER") : Value;
}

FVector2D GlueAlignmentFromPoint(const FString& Point)
{
	const FString NormalizedPoint = NormalizeGluePoint(Point);
	float X = 0.5f;
	float Y = 0.5f;

	if (NormalizedPoint.Contains(TEXT("LEFT")))
	{
		X = 0.0f;
	}
	else if (NormalizedPoint.Contains(TEXT("RIGHT")))
	{
		X = 1.0f;
	}

	if (NormalizedPoint.Contains(TEXT("TOP")))
	{
		Y = 0.0f;
	}
	else if (NormalizedPoint.Contains(TEXT("BOTTOM")))
	{
		Y = 1.0f;
	}

	return FVector2D(X, Y);
}

FAnchors GlueAnchorFromPoint(const FString& Point)
{
	const FVector2D Alignment = GlueAlignmentFromPoint(Point);
	return FAnchors(Alignment.X, Alignment.Y);
}

FLinearColor GluePanelColor()
{
	return FLinearColor(0.015f, 0.018f, 0.025f, 0.72f);
}

FLinearColor GlueTextColor()
{
	return FLinearColor(0.95f, 0.84f, 0.22f, 1.0f);
}

FString EscapeRichTextValue(const FString& Value)
{
	FString Escaped = Value;
	Escaped.ReplaceInline(TEXT("&"), TEXT("&amp;"));
	Escaped.ReplaceInline(TEXT("<"), TEXT("&lt;"));
	Escaped.ReplaceInline(TEXT(">"), TEXT("&gt;"));
	Escaped.ReplaceInline(TEXT("\""), TEXT("&quot;"));
	return Escaped;
}

FString ExtractQuotedAttribute(const FString& Source, const FString& AttributeName)
{
	const FString SearchToken = AttributeName.ToLower() + TEXT("=");
	const int32 AttributeIndex = Source.ToLower().Find(SearchToken);
	if (AttributeIndex == INDEX_NONE)
	{
		return FString();
	}

	int32 ValueStart = AttributeIndex + SearchToken.Len();
	while (ValueStart < Source.Len() && FChar::IsWhitespace(Source[ValueStart]))
	{
		++ValueStart;
	}
	if (ValueStart >= Source.Len())
	{
		return FString();
	}

	const TCHAR Quote = Source[ValueStart];
	if (Quote != TEXT('\'') && Quote != TEXT('"'))
	{
		return FString();
	}

	const int32 ContentStart = ValueStart + 1;
	const int32 ContentEnd = Source.Find(FString::Chr(Quote), ESearchCase::CaseSensitive, ESearchDir::FromStart, ContentStart);
	return ContentEnd == INDEX_NONE ? FString() : Source.Mid(ContentStart, ContentEnd - ContentStart);
}

FString ExtractHTMLAttribute(const FString& Source, const FString& AttributeName)
{
	const FString LowerSource = Source.ToLower();
	const FString LowerAttribute = AttributeName.ToLower();
	int32 SearchStart = 0;

	while (SearchStart < Source.Len())
	{
		const int32 AttributeIndex = LowerSource.Find(LowerAttribute, ESearchCase::CaseSensitive, ESearchDir::FromStart, SearchStart);
		if (AttributeIndex == INDEX_NONE)
		{
			return FString();
		}

		const bool bValidPrefix = AttributeIndex == 0
			|| FChar::IsWhitespace(Source[AttributeIndex - 1])
			|| Source[AttributeIndex - 1] == TEXT('<');
		const int32 AttributeEnd = AttributeIndex + AttributeName.Len();
		const bool bValidSuffix = AttributeEnd >= Source.Len()
			|| FChar::IsWhitespace(Source[AttributeEnd])
			|| Source[AttributeEnd] == TEXT('=')
			|| Source[AttributeEnd] == TEXT('/');
		if (!bValidPrefix || !bValidSuffix)
		{
			SearchStart = AttributeEnd;
			continue;
		}

		int32 ValueStart = AttributeEnd;
		while (ValueStart < Source.Len() && FChar::IsWhitespace(Source[ValueStart]))
		{
			++ValueStart;
		}
		if (ValueStart >= Source.Len() || Source[ValueStart] != TEXT('='))
		{
			return FString();
		}

		++ValueStart;
		while (ValueStart < Source.Len() && FChar::IsWhitespace(Source[ValueStart]))
		{
			++ValueStart;
		}
		if (ValueStart >= Source.Len())
		{
			return FString();
		}

		const TCHAR Quote = Source[ValueStart];
		if (Quote == TEXT('\'') || Quote == TEXT('"'))
		{
			const int32 ContentStart = ValueStart + 1;
			const int32 ContentEnd = Source.Find(FString::Chr(Quote), ESearchCase::CaseSensitive, ESearchDir::FromStart, ContentStart);
			return ContentEnd == INDEX_NONE ? FString() : Source.Mid(ContentStart, ContentEnd - ContentStart);
		}

		int32 ValueEnd = ValueStart;
		while (ValueEnd < Source.Len() && !FChar::IsWhitespace(Source[ValueEnd]) && Source[ValueEnd] != TEXT('/'))
		{
			++ValueEnd;
		}
		return Source.Mid(ValueStart, ValueEnd - ValueStart);
	}

	return FString();
}

FString GetSimpleHTMLTagName(const FString& Tag)
{
	FString NormalizedTag = Tag;
	NormalizedTag.TrimStartAndEndInline();

	if (NormalizedTag.StartsWith(TEXT("/")))
	{
		NormalizedTag.RightChopInline(1);
		NormalizedTag.TrimStartInline();
	}

	int32 NameLength = 0;
	while (NameLength < NormalizedTag.Len()
		&& !FChar::IsWhitespace(NormalizedTag[NameLength])
		&& NormalizedTag[NameLength] != TEXT('/')
		&& NormalizedTag[NameLength] != TEXT('>'))
	{
		++NameLength;
	}

	FString TagName = NormalizedTag.Left(NameLength);
	TagName.ToLowerInline();
	return TagName;
}

bool ParseHexByte(const FString& Value, int32 StartIndex, float& OutByte)
{
	if (StartIndex + 2 > Value.Len())
	{
		return false;
	}

	const FString ByteText = Value.Mid(StartIndex, 2);
	TCHAR* EndPtr = nullptr;
	const int64 ParsedValue = FCString::Strtoi64(*ByteText, &EndPtr, 16);
	if (EndPtr == *ByteText || *EndPtr != TEXT('\0') || ParsedValue < 0 || ParsedValue > 255)
	{
		return false;
	}

	OutByte = static_cast<float>(ParsedValue) / 255.0f;
	return true;
}

bool ParseSimpleHTMLColor(FString ColorText, FLinearColor& OutColor)
{
	ColorText.TrimStartAndEndInline();
	if (ColorText.IsEmpty())
	{
		return false;
	}

	FString LowerColor = ColorText.ToLower();
	LowerColor.TrimStartAndEndInline();

	if (LowerColor.StartsWith(TEXT("#")))
	{
		LowerColor.RightChopInline(1);
	}
	else if (LowerColor.StartsWith(TEXT("0x")))
	{
		LowerColor.RightChopInline(2);
	}

	if (LowerColor.StartsWith(TEXT("|cff")))
	{
		LowerColor.RightChopInline(4);
	}
	if (LowerColor.EndsWith(TEXT("|r")))
	{
		LowerColor.LeftChopInline(2);
	}

	if (LowerColor == TEXT("red"))
	{
		OutColor = FLinearColor(1.0f, 0.0f, 0.0f, 1.0f);
		return true;
	}
	if (LowerColor == TEXT("green"))
	{
		OutColor = FLinearColor(0.0f, 1.0f, 0.0f, 1.0f);
		return true;
	}
	if (LowerColor == TEXT("blue"))
	{
		OutColor = FLinearColor(0.0f, 0.25f, 1.0f, 1.0f);
		return true;
	}
	if (LowerColor == TEXT("white"))
	{
		OutColor = FLinearColor::White;
		return true;
	}
	if (LowerColor == TEXT("black"))
	{
		OutColor = FLinearColor::Black;
		return true;
	}
	if (LowerColor == TEXT("yellow"))
	{
		OutColor = FLinearColor::Yellow;
		return true;
	}
	if (LowerColor == TEXT("orange"))
	{
		OutColor = FLinearColor(1.0f, 0.45f, 0.0f, 1.0f);
		return true;
	}
	if (LowerColor == TEXT("gray") || LowerColor == TEXT("grey"))
	{
		OutColor = FLinearColor(0.5f, 0.5f, 0.5f, 1.0f);
		return true;
	}
	if (LowerColor == TEXT("cyan") || LowerColor == TEXT("aqua"))
	{
		OutColor = FLinearColor(0.0f, 1.0f, 1.0f, 1.0f);
		return true;
	}
	if (LowerColor == TEXT("magenta") || LowerColor == TEXT("fuchsia"))
	{
		OutColor = FLinearColor(1.0f, 0.0f, 1.0f, 1.0f);
		return true;
	}
	if (LowerColor == TEXT("purple"))
	{
		OutColor = FLinearColor(0.55f, 0.0f, 0.8f, 1.0f);
		return true;
	}
	if (LowerColor == TEXT("gold"))
	{
		OutColor = FLinearColor(1.0f, 0.82f, 0.0f, 1.0f);
		return true;
	}

	if (LowerColor.StartsWith(TEXT("rgb(")) && LowerColor.EndsWith(TEXT(")")))
	{
		FString Inner = LowerColor.Mid(4, LowerColor.Len() - 5);
		TArray<FString> Components;
		Inner.ParseIntoArray(Components, TEXT(","), true);
		if (Components.Num() == 3)
		{
			float R = 0.0f;
			float G = 0.0f;
			float B = 0.0f;
			if (LexTryParseString(R, *Components[0].TrimStartAndEnd())
				&& LexTryParseString(G, *Components[1].TrimStartAndEnd())
				&& LexTryParseString(B, *Components[2].TrimStartAndEnd()))
			{
				OutColor = FLinearColor(FMath::Clamp(R / 255.0f, 0.0f, 1.0f), FMath::Clamp(G / 255.0f, 0.0f, 1.0f), FMath::Clamp(B / 255.0f, 0.0f, 1.0f), 1.0f);
				return true;
			}
		}
	}

	if (LowerColor.Len() == 3)
	{
		float R = 0.0f;
		float G = 0.0f;
		float B = 0.0f;
		if (ParseHexByte(FString::ChrN(2, LowerColor[0]), 0, R)
			&& ParseHexByte(FString::ChrN(2, LowerColor[1]), 0, G)
			&& ParseHexByte(FString::ChrN(2, LowerColor[2]), 0, B))
		{
			OutColor = FLinearColor(R, G, B, 1.0f);
			return true;
		}
	}

	if (LowerColor.Len() == 6 || LowerColor.Len() == 8)
	{
		int32 Offset = 0;
		float A = 1.0f;
		float R = 0.0f;
		float G = 0.0f;
		float B = 0.0f;
		if (LowerColor.Len() == 8)
		{
			if (!ParseHexByte(LowerColor, 0, A))
			{
				return false;
			}
			Offset = 2;
		}

		if (ParseHexByte(LowerColor, Offset, R)
			&& ParseHexByte(LowerColor, Offset + 2, G)
			&& ParseHexByte(LowerColor, Offset + 4, B))
		{
			OutColor = FLinearColor(R, G, B, A);
			return true;
		}
	}

	return false;
}

TOptional<FLinearColor> ExtractSimpleHTMLTagColor(const FString& Tag)
{
	FLinearColor ParsedColor;
	const FString ColorAttribute = ExtractHTMLAttribute(Tag, TEXT("color"));
	if (!ColorAttribute.IsEmpty() && ParseSimpleHTMLColor(ColorAttribute, ParsedColor))
	{
		return ParsedColor;
	}

	const FString StyleAttribute = ExtractHTMLAttribute(Tag, TEXT("style"));
	TArray<FString> Declarations;
	StyleAttribute.ParseIntoArray(Declarations, TEXT(";"), true);
	for (FString Declaration : Declarations)
	{
		Declaration.TrimStartAndEndInline();
		if (Declaration.ToLower().StartsWith(TEXT("color")))
		{
			const int32 SeparatorIndex = Declaration.Find(TEXT(":"));
			if (SeparatorIndex != INDEX_NONE)
			{
				FString StyleColor = Declaration.Mid(SeparatorIndex + 1);
				if (ParseSimpleHTMLColor(StyleColor, ParsedColor))
				{
					return ParsedColor;
				}
			}
		}
	}

	return TOptional<FLinearColor>();
}

struct FSimpleHTMLFontSize
{
	bool bAbsolutePixels = false;
	float Value = 1.0f;
};

bool ParseSimpleHTMLFontSize(FString SizeText, FSimpleHTMLFontSize& OutFontSize)
{
	SizeText.TrimStartAndEndInline();
	if (SizeText.IsEmpty())
	{
		return false;
	}

	FString LowerSize = SizeText.ToLower();
	LowerSize.TrimStartAndEndInline();
	if (LowerSize.EndsWith(TEXT("px")))
	{
		LowerSize.LeftChopInline(2);
		LowerSize.TrimStartAndEndInline();

		float ParsedPixels = 0.0f;
		if (!LexTryParseString(ParsedPixels, *LowerSize) || ParsedPixels <= 0.0f)
		{
			return false;
		}

		OutFontSize.bAbsolutePixels = true;
		OutFontSize.Value = FMath::Clamp(ParsedPixels, 6.0f, 96.0f);
		return true;
	}

	float ParsedSize = 0.0f;
	if (!LexTryParseString(ParsedSize, *SizeText))
	{
		return false;
	}

	if (ParsedSize <= 0.0f)
	{
		return false;
	}

	// Match classic HTML font-size semantics closely enough for client-authored
	// Glue strings: 3 is the default baseline, 1/2 are smaller, 4+ are larger.
	const int32 SizeLevel = FMath::Clamp(FMath::RoundToInt(ParsedSize), 1, 7);
	OutFontSize.bAbsolutePixels = false;
	switch (SizeLevel)
	{
	case 1:
		OutFontSize.Value = 0.62f;
		return true;
	case 2:
		OutFontSize.Value = 0.80f;
		return true;
	case 3:
		OutFontSize.Value = 1.00f;
		return true;
	case 4:
		OutFontSize.Value = 1.20f;
		return true;
	case 5:
		OutFontSize.Value = 1.45f;
		return true;
	case 6:
		OutFontSize.Value = 1.75f;
		return true;
	case 7:
	default:
		OutFontSize.Value = 2.10f;
		return true;
	}
}

TOptional<FSimpleHTMLFontSize> ExtractSimpleHTMLTagFontSize(const FString& Tag)
{
	FSimpleHTMLFontSize ParsedFontSize;
	const FString SizeAttribute = ExtractHTMLAttribute(Tag, TEXT("size"));
	if (!SizeAttribute.IsEmpty() && ParseSimpleHTMLFontSize(SizeAttribute, ParsedFontSize))
	{
		return ParsedFontSize;
	}

	const FString StyleAttribute = ExtractHTMLAttribute(Tag, TEXT("style"));
	TArray<FString> Declarations;
	StyleAttribute.ParseIntoArray(Declarations, TEXT(";"), true);
	for (FString Declaration : Declarations)
	{
		Declaration.TrimStartAndEndInline();
		if (Declaration.ToLower().StartsWith(TEXT("font-size")))
		{
			const int32 SeparatorIndex = Declaration.Find(TEXT(":"));
			if (SeparatorIndex != INDEX_NONE)
			{
				FString StyleFontSize = Declaration.Mid(SeparatorIndex + 1);
				if (ParseSimpleHTMLFontSize(StyleFontSize, ParsedFontSize))
				{
					return ParsedFontSize;
				}
			}
		}
	}

	return TOptional<FSimpleHTMLFontSize>();
}

bool AreOptionalColorsEqual(const TOptional<FLinearColor>& Left, const TOptional<FLinearColor>& Right)
{
	if (Left.IsSet() != Right.IsSet())
	{
		return false;
	}
	return !Left.IsSet() || Left.GetValue().Equals(Right.GetValue());
}

bool AreOptionalFontSizesEqual(const TOptional<FSimpleHTMLFontSize>& Left, const TOptional<FSimpleHTMLFontSize>& Right)
{
	if (Left.IsSet() != Right.IsSet())
	{
		return false;
	}
	if (!Left.IsSet())
	{
		return true;
	}

	return Left.GetValue().bAbsolutePixels == Right.GetValue().bAbsolutePixels
		&& FMath::IsNearlyEqual(Left.GetValue().Value, Right.GetValue().Value);
}

FString DecodeSimpleHTMLEntities(FString Text)
{
	Text.ReplaceInline(TEXT("&nbsp;"), TEXT(" "));
	Text.ReplaceInline(TEXT("&lt;"), TEXT("<"));
	Text.ReplaceInline(TEXT("&gt;"), TEXT(">"));
	Text.ReplaceInline(TEXT("&quot;"), TEXT("\""));
	Text.ReplaceInline(TEXT("&#39;"), TEXT("'"));
	Text.ReplaceInline(TEXT("&apos;"), TEXT("'"));
	Text.ReplaceInline(TEXT("&amp;"), TEXT("&"));
	return Text;
}

bool IsSimpleHTMLFormattingWhitespace(const FString& Text)
{
	bool bHasNewlineOrTab = false;
	for (const TCHAR Character : Text)
	{
		if (Character == TEXT('\n') || Character == TEXT('\r') || Character == TEXT('\t'))
		{
			bHasNewlineOrTab = true;
		}
		else if (!FChar::IsWhitespace(Character))
		{
			return false;
		}
	}
	return bHasNewlineOrTab;
}

FString NormalizeSimpleHTMLTextSegment(FString Text)
{
	const FString ExplicitLineBreakMarker = FString::Chr(TEXT('\x1E'));
	if (IsSimpleHTMLFormattingWhitespace(Text))
	{
		return FString();
	}

	Text = DecodeSimpleHTMLEntities(MoveTemp(Text));
	Text.ReplaceInline(*ExplicitLineBreakMarker, TEXT("\n"));
	return Text;
}

FString ConvertSimpleHTMLToRichText(const FString& HtmlText)
{
	FString Input = HtmlText;
	Input.ReplaceInline(TEXT("\r\n"), TEXT("\n"));
	Input.ReplaceInline(TEXT("\r"), TEXT("\n"));
	Input.ReplaceInline(TEXT("|n"), TEXT("\n"));

	FString Output;
	int32 Cursor = 0;
	while (Cursor < Input.Len())
	{
		const int32 TagStart = Input.Find(TEXT("<"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Cursor);
		if (TagStart == INDEX_NONE)
		{
			Output += EscapeRichTextValue(Input.Mid(Cursor));
			break;
		}

		Output += EscapeRichTextValue(Input.Mid(Cursor, TagStart - Cursor));
		const int32 TagEnd = Input.Find(TEXT(">"), ESearchCase::CaseSensitive, ESearchDir::FromStart, TagStart);
		if (TagEnd == INDEX_NONE)
		{
			Output += EscapeRichTextValue(Input.Mid(TagStart));
			break;
		}

		const FString Tag = Input.Mid(TagStart + 1, TagEnd - TagStart - 1);
		FString NormalizedTag = Tag;
		NormalizedTag.TrimStartAndEndInline();
		NormalizedTag.ToLowerInline();

		if (NormalizedTag.StartsWith(TEXT("a ")))
		{
			const int32 CloseTagStart = Input.Find(TEXT("</a>"), ESearchCase::IgnoreCase, ESearchDir::FromStart, TagEnd + 1);
			if (CloseTagStart != INDEX_NONE)
			{
				const FString Href = ExtractQuotedAttribute(Tag, TEXT("href"));
				const FString LinkText = Input.Mid(TagEnd + 1, CloseTagStart - TagEnd - 1);
				Output += FString::Printf(
					TEXT("<a id=\"GlueSimpleHTML\" href=\"%s\" text=\"%s\">%s</>"),
					*EscapeRichTextValue(Href),
					*EscapeRichTextValue(LinkText),
					*EscapeRichTextValue(LinkText));
				Cursor = CloseTagStart + 4;
				continue;
			}
		}
		else if (NormalizedTag == TEXT("br") || NormalizedTag == TEXT("br/") || NormalizedTag == TEXT("br /"))
		{
			Output += TEXT("\n");
		}
		else if (NormalizedTag == TEXT("p") || NormalizedTag == TEXT("/p") || NormalizedTag.StartsWith(TEXT("p ")))
		{
			if (!Output.IsEmpty() && !Output.EndsWith(TEXT("\n")))
			{
				Output += TEXT("\n");
			}
		}

		Cursor = TagEnd + 1;
	}

	Output.TrimStartAndEndInline();
	return Output;
}

struct FSimpleHTMLTextRun
{
	FString Text;
	FString Href;
	TOptional<FLinearColor> Color;
	TOptional<ETextJustify::Type> Justification;
	TOptional<FSimpleHTMLFontSize> FontSize;

	bool IsLink() const
	{
		return !Href.IsEmpty();
	}
};

void AppendSimpleHTMLRun(
	TArray<FSimpleHTMLTextRun>& Runs,
	const FString& Text,
	const FString& Href = FString(),
	const TOptional<FLinearColor>& Color = TOptional<FLinearColor>(),
	const TOptional<ETextJustify::Type>& Justification = TOptional<ETextJustify::Type>(),
	const TOptional<FSimpleHTMLFontSize>& FontSize = TOptional<FSimpleHTMLFontSize>())
{
	if (Text.IsEmpty())
	{
		return;
	}

	if (Runs.Num() > 0
		&& Runs.Last().Href == Href
		&& AreOptionalColorsEqual(Runs.Last().Color, Color)
		&& Runs.Last().Justification == Justification
		&& AreOptionalFontSizesEqual(Runs.Last().FontSize, FontSize))
	{
		Runs.Last().Text += Text;
		return;
	}

	FSimpleHTMLTextRun Run;
	Run.Text = Text;
	Run.Href = Href;
	Run.Color = Color;
	Run.Justification = Justification;
	Run.FontSize = FontSize;
	Runs.Add(MoveTemp(Run));
}

TOptional<FLinearColor> ResolveGlueTextRunColor(
	const TOptional<FLinearColor>& BaseColor,
	const TOptional<FLinearColor>& InlineColor)
{
	return InlineColor.IsSet() ? InlineColor : BaseColor;
}

bool ParseWoWInlineColorCode(const FString& Input, int32 MarkerIndex, FLinearColor& OutColor)
{
	if (MarkerIndex + 10 > Input.Len() || Input[MarkerIndex] != TEXT('|'))
	{
		return false;
	}

	const TCHAR CodeType = Input[MarkerIndex + 1];
	if (CodeType != TEXT('c') && CodeType != TEXT('C'))
	{
		return false;
	}

	const FString ColorText = Input.Mid(MarkerIndex + 2, 8);
	return ParseSimpleHTMLColor(ColorText, OutColor);
}

void AppendGlueTextRunWithWoWColorCodes(
	TArray<FSimpleHTMLTextRun>& Runs,
	const FString& Text,
	const FString& Href,
	const TOptional<FLinearColor>& BaseColor,
	const TOptional<ETextJustify::Type>& Justification,
	const TOptional<FSimpleHTMLFontSize>& FontSize,
	TOptional<FLinearColor>& InlineColor,
	TArray<TOptional<FLinearColor>>& InlineColorStack)
{
	FString Buffer;
	auto FlushBuffer = [&]()
	{
		if (!Buffer.IsEmpty())
		{
			AppendSimpleHTMLRun(Runs, Buffer, Href, ResolveGlueTextRunColor(BaseColor, InlineColor), Justification, FontSize);
			Buffer.Reset();
		}
	};

	for (int32 Index = 0; Index < Text.Len();)
	{
		if (Text[Index] == TEXT('|') && Index + 1 < Text.Len())
		{
			FLinearColor ParsedColor;
			if (ParseWoWInlineColorCode(Text, Index, ParsedColor))
			{
				FlushBuffer();
				InlineColorStack.Add(InlineColor);
				InlineColor = ParsedColor;
				Index += 10;
				continue;
			}

			const TCHAR CodeType = Text[Index + 1];
			if (CodeType == TEXT('r') || CodeType == TEXT('R'))
			{
				FlushBuffer();
				InlineColor = InlineColorStack.Num() > 0 ? InlineColorStack.Pop(EAllowShrinking::No) : TOptional<FLinearColor>();
				Index += 2;
				continue;
			}
			if (CodeType == TEXT('n') || CodeType == TEXT('N'))
			{
				Buffer.AppendChar(TEXT('\n'));
				Index += 2;
				continue;
			}
			if (CodeType == TEXT('|'))
			{
				Buffer.AppendChar(TEXT('|'));
				Index += 2;
				continue;
			}
		}

		Buffer.AppendChar(Text[Index]);
		++Index;
	}

	FlushBuffer();
}

TArray<FSimpleHTMLTextRun> ParseGluePlainTextRuns(const FString& SourceText)
{
	FString Input = SourceText;
	Input.ReplaceInline(TEXT("\r\n"), TEXT("\n"));
	Input.ReplaceInline(TEXT("\r"), TEXT("\n"));

	TArray<FSimpleHTMLTextRun> Runs;
	TOptional<FLinearColor> InlineColor;
	TArray<TOptional<FLinearColor>> InlineColorStack;
	AppendGlueTextRunWithWoWColorCodes(
		Runs,
		Input,
		FString(),
		TOptional<FLinearColor>(),
		TOptional<ETextJustify::Type>(),
		TOptional<FSimpleHTMLFontSize>(),
		InlineColor,
		InlineColorStack);
	return Runs;
}

FString StripWoWTextControlCodes(const FString& SourceText)
{
	FString Output;
	FString Input = SourceText;
	Input.ReplaceInline(TEXT("\r\n"), TEXT("\n"));
	Input.ReplaceInline(TEXT("\r"), TEXT("\n"));

	for (int32 Index = 0; Index < Input.Len();)
	{
		if (Input[Index] == TEXT('|') && Index + 1 < Input.Len())
		{
			FLinearColor ParsedColor;
			if (ParseWoWInlineColorCode(Input, Index, ParsedColor))
			{
				Index += 10;
				continue;
			}

			const TCHAR CodeType = Input[Index + 1];
			if (CodeType == TEXT('r') || CodeType == TEXT('R'))
			{
				Index += 2;
				continue;
			}
			if (CodeType == TEXT('n') || CodeType == TEXT('N'))
			{
				Output.AppendChar(TEXT('\n'));
				Index += 2;
				continue;
			}
			if (CodeType == TEXT('|'))
			{
				Output.AppendChar(TEXT('|'));
				Index += 2;
				continue;
			}
		}

		Output.AppendChar(Input[Index]);
		++Index;
	}

	return Output;
}

TArray<FSimpleHTMLTextRun> ParseSimpleHTMLRuns(const FString& HtmlText)
{
	const FString ExplicitLineBreakMarker = FString::Chr(TEXT('\x1E'));
	FString Input = HtmlText;
	Input.ReplaceInline(TEXT("\r\n"), TEXT("\n"));
	Input.ReplaceInline(TEXT("\r"), TEXT("\n"));
	Input.ReplaceInline(TEXT("|n"), *ExplicitLineBreakMarker);

	TArray<FSimpleHTMLTextRun> Runs;
	TArray<FString> HrefStack;
	TArray<TOptional<FLinearColor>> ColorStack;
	TArray<TOptional<ETextJustify::Type>> JustificationStack;
	TArray<TOptional<FSimpleHTMLFontSize>> FontSizeStack;
	FString CurrentHref;
	TOptional<FLinearColor> CurrentColor;
	TOptional<FLinearColor> CurrentInlineColor;
	TOptional<ETextJustify::Type> CurrentJustification;
	TOptional<FSimpleHTMLFontSize> CurrentFontSize;
	TArray<TOptional<FLinearColor>> InlineColorStack;

	int32 Cursor = 0;
	while (Cursor < Input.Len())
	{
		const int32 TagStart = Input.Find(TEXT("<"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Cursor);
		if (TagStart == INDEX_NONE)
		{
			AppendGlueTextRunWithWoWColorCodes(Runs, NormalizeSimpleHTMLTextSegment(Input.Mid(Cursor)), CurrentHref, CurrentColor, CurrentJustification, CurrentFontSize, CurrentInlineColor, InlineColorStack);
			break;
		}

		AppendGlueTextRunWithWoWColorCodes(Runs, NormalizeSimpleHTMLTextSegment(Input.Mid(Cursor, TagStart - Cursor)), CurrentHref, CurrentColor, CurrentJustification, CurrentFontSize, CurrentInlineColor, InlineColorStack);
		const int32 TagEnd = Input.Find(TEXT(">"), ESearchCase::CaseSensitive, ESearchDir::FromStart, TagStart);
		if (TagEnd == INDEX_NONE)
		{
			AppendGlueTextRunWithWoWColorCodes(Runs, NormalizeSimpleHTMLTextSegment(Input.Mid(TagStart)), CurrentHref, CurrentColor, CurrentJustification, CurrentFontSize, CurrentInlineColor, InlineColorStack);
			break;
		}

		const FString Tag = Input.Mid(TagStart + 1, TagEnd - TagStart - 1);
		FString NormalizedTag = Tag;
		NormalizedTag.TrimStartAndEndInline();
		NormalizedTag.ToLowerInline();
		const FString TagName = GetSimpleHTMLTagName(Tag);
		const bool bClosingTag = NormalizedTag.StartsWith(TEXT("/"));
		const bool bSelfClosingTag = NormalizedTag.EndsWith(TEXT("/"));

		if (!bClosingTag && TagName == TEXT("a"))
		{
			HrefStack.Add(CurrentHref);
			const FString Href = ExtractHTMLAttribute(Tag, TEXT("href"));
			if (!Href.IsEmpty())
			{
				CurrentHref = Href;
			}
			if (bSelfClosingTag)
			{
				CurrentHref = HrefStack.Pop(EAllowShrinking::No);
			}
		}
		else if (bClosingTag && TagName == TEXT("a"))
		{
			CurrentHref = HrefStack.Num() > 0 ? HrefStack.Pop(EAllowShrinking::No) : FString();
		}
		else if (!bClosingTag && (TagName == TEXT("font") || TagName == TEXT("span")))
		{
			ColorStack.Add(CurrentColor);
			FontSizeStack.Add(CurrentFontSize);
			if (const TOptional<FLinearColor> TagColor = ExtractSimpleHTMLTagColor(Tag); TagColor.IsSet())
			{
				CurrentColor = TagColor;
			}
			if (const TOptional<FSimpleHTMLFontSize> TagFontSize = ExtractSimpleHTMLTagFontSize(Tag); TagFontSize.IsSet())
			{
				CurrentFontSize = TagFontSize;
			}
			if (bSelfClosingTag)
			{
				CurrentColor = ColorStack.Num() > 0 ? ColorStack.Pop(EAllowShrinking::No) : TOptional<FLinearColor>();
				CurrentFontSize = FontSizeStack.Num() > 0 ? FontSizeStack.Pop(EAllowShrinking::No) : TOptional<FSimpleHTMLFontSize>();
			}
		}
		else if (bClosingTag && (TagName == TEXT("font") || TagName == TEXT("span")))
		{
			CurrentColor = ColorStack.Num() > 0 ? ColorStack.Pop(EAllowShrinking::No) : TOptional<FLinearColor>();
			CurrentFontSize = FontSizeStack.Num() > 0 ? FontSizeStack.Pop(EAllowShrinking::No) : TOptional<FSimpleHTMLFontSize>();
		}
		else if (!bClosingTag && (TagName == TEXT("br") || TagName == TEXT("hr")))
		{
			AppendSimpleHTMLRun(Runs, TEXT("\n"), CurrentHref, CurrentColor, CurrentJustification, CurrentFontSize);
		}
		else if (!bClosingTag && TagName == TEXT("p"))
		{
			JustificationStack.Add(CurrentJustification);
			FString Align = ExtractHTMLAttribute(Tag, TEXT("align"));
			Align.TrimStartAndEndInline();
			Align.ToLowerInline();
			if (Align == TEXT("center") || Align == TEXT("middle"))
			{
				CurrentJustification = ETextJustify::Center;
			}
			else if (Align == TEXT("right"))
			{
				CurrentJustification = ETextJustify::Right;
			}
			else if (Align == TEXT("left"))
			{
				CurrentJustification = ETextJustify::Left;
			}
			if (bSelfClosingTag)
			{
				CurrentJustification = JustificationStack.Num() > 0 ? JustificationStack.Pop(EAllowShrinking::No) : TOptional<ETextJustify::Type>();
			}
		}
		else if (bClosingTag && TagName == TEXT("p"))
		{
			AppendSimpleHTMLRun(Runs, TEXT("\n\n"), CurrentHref, CurrentColor, CurrentJustification, CurrentFontSize);
			CurrentJustification = JustificationStack.Num() > 0 ? JustificationStack.Pop(EAllowShrinking::No) : TOptional<ETextJustify::Type>();
		}

		Cursor = TagEnd + 1;
	}

	return Runs;
}

float MeasureGlueText(const FString& Text, const FSlateFontInfo& Font)
{
	if (Text.IsEmpty())
	{
		return 0.0f;
	}

	if (const TSharedPtr<FSlateFontMeasure> FontMeasure = FSlateApplication::Get().GetRenderer()->GetFontMeasureService())
	{
		return FontMeasure->Measure(FText::FromString(Text), Font).X;
	}
	return Text.Len() * Font.Size * 0.55f;
}

float EstimateGlueCharacterAdvance(TCHAR Character, float FontSize)
{
	if (Character == TEXT(' ') || Character == TEXT('\t'))
	{
		return FontSize * 0.35f;
	}
	if (Character < 0x80)
	{
		return FontSize * 0.62f;
	}
	return FontSize;
}

float GetGlueLineHeight(const FSlateFontInfo& Font, float FontSize, const TSharedPtr<FSlateFontMeasure>& FontMeasure)
{
	const float FallbackLineHeight = FMath::Max(1.0f, FontSize * 1.35f);
	if (FontMeasure.IsValid())
	{
		const uint16 MaxCharacterHeight = FontMeasure->GetMaxCharacterHeight(Font);
		if (MaxCharacterHeight > 0)
		{
			return FMath::Max(FallbackLineHeight, static_cast<float>(MaxCharacterHeight));
		}
	}
	return FallbackLineHeight;
}

FVector2D EstimateGlueTextSize(const FString& Text, float FontSize, float WrapWidth = 0.0f, float LineHeight = 0.0f)
{
	FString NormalizedText = Text;
	NormalizedText.ReplaceInline(TEXT("\r\n"), TEXT("\n"));
	NormalizedText.ReplaceInline(TEXT("\r"), TEXT("\n"));

	TArray<FString> Lines;
	NormalizedText.ParseIntoArray(Lines, TEXT("\n"), false);
	if (Lines.Num() == 0)
	{
		Lines.Add(NormalizedText);
	}

	const float EffectiveLineHeight = LineHeight > 0.0f ? LineHeight : FMath::Max(1.0f, FontSize * 1.35f);
	float MaxLineWidth = 1.0f;
	int32 VisualLineCount = 0;
	for (const FString& Line : Lines)
	{
		float LineWidth = 1.0f;
		for (int32 Index = 0; Index < Line.Len(); ++Index)
		{
			LineWidth += EstimateGlueCharacterAdvance(Line[Index], FontSize);
		}
		if (WrapWidth > 0.0f)
		{
			const int32 WrappedLines = FMath::Max(1, FMath::CeilToInt(LineWidth / WrapWidth));
			VisualLineCount += WrappedLines;
			MaxLineWidth = FMath::Max(MaxLineWidth, FMath::Min(LineWidth, WrapWidth));
		}
		else
		{
			++VisualLineCount;
			MaxLineWidth = FMath::Max(MaxLineWidth, LineWidth);
		}
	}

	return FVector2D(MaxLineWidth, FMath::Max(1.0f, VisualLineCount * EffectiveLineHeight));
}

bool IsGlueSingleLineMeasureValid(const FVector2D& MeasuredSize, const FString& Text, float FontSize, float LineHeight)
{
	if (!FMath::IsFinite(MeasuredSize.X) || !FMath::IsFinite(MeasuredSize.Y) || MeasuredSize.X <= 0.0f || MeasuredSize.Y <= 0.0f)
	{
		return false;
	}

	const FVector2D EstimatedSize = EstimateGlueTextSize(Text, FontSize, 0.0f, LineHeight);
	const float MaxSingleLineHeight = FMath::Max(LineHeight * 1.75f, FontSize * 2.0f);
	const float MaxSingleLineWidth = FMath::Max(EstimatedSize.X + LineHeight * 2.0f, Text.Len() * LineHeight * 2.0f);
	return MeasuredSize.Y <= MaxSingleLineHeight && MeasuredSize.X <= MaxSingleLineWidth;
}

bool IsGlueWrappedMeasureValid(const FVector2D& MeasuredSize, const FVector2D& FallbackSize, float WrapWidth, float LineHeight)
{
	if (!FMath::IsFinite(MeasuredSize.X) || !FMath::IsFinite(MeasuredSize.Y) || MeasuredSize.X <= 0.0f || MeasuredSize.Y <= 0.0f)
	{
		return false;
	}

	const float MaxWrappedWidth = FMath::Max(WrapWidth + LineHeight * 2.0f, FallbackSize.X + LineHeight);
	const float MaxWrappedHeight = FallbackSize.Y + LineHeight * 2.0f;
	return MeasuredSize.X <= MaxWrappedWidth && MeasuredSize.Y <= MaxWrappedHeight;
}

FString NormalizeGlueFontPath(FString FontPath)
{
	FontPath.TrimStartAndEndInline();
	FontPath.ReplaceInline(TEXT("\\"), TEXT("/"));
	FontPath.RemoveFromStart(TEXT("/"));
	if (!FPaths::GetExtension(FontPath).Len())
	{
		FontPath += TEXT(".ttf");
	}
	return FontPath;
}

FString ResolveGlueFontPath(const FString& FontPath)
{
	const FString Normalized = NormalizeGlueFontPath(FontPath);
	if (Normalized.IsEmpty())
	{
		return FString();
	}

	const TArray<FString> Candidates =
	{
		FPaths::ProjectDir() / TEXT("Data") / Normalized,
		FPaths::ProjectContentDir() / Normalized,
		FPaths::EngineContentDir() / Normalized
	};

	for (const FString& Candidate : Candidates)
	{
		if (FPaths::FileExists(Candidate))
		{
			return Candidate;
		}
	}
	return FString();
}

FFontOutlineSettings MakeGlueFontOutlineSettings(const FString& Flags)
{
	FString NormalizedFlags = Flags;
	NormalizedFlags.ToUpperInline();

	FFontOutlineSettings OutlineSettings;
	if (NormalizedFlags.Contains(TEXT("THICKOUTLINE")))
	{
		OutlineSettings.OutlineSize = 2;
	}
	else if (NormalizedFlags.Contains(TEXT("OUTLINE")))
	{
		OutlineSettings.OutlineSize = 1;
	}
	OutlineSettings.OutlineColor = FLinearColor::Black;
	return OutlineSettings;
}

ETextJustify::Type ParseGlueJustifyH(const FString& Value)
{
	FString Normalized = Value;
	Normalized.TrimStartAndEndInline();
	Normalized.ToUpperInline();
	if (Normalized == TEXT("LEFT"))
	{
		return ETextJustify::Left;
	}
	if (Normalized == TEXT("JUSTIFY") || Normalized == TEXT("DISTRIBUTE") || Normalized == TEXT("DISTRIBUTED"))
	{
		return ETextJustify::Left;
	}
	if (Normalized == TEXT("RIGHT"))
	{
		return ETextJustify::Right;
	}
	return ETextJustify::Center;
}

EHorizontalAlignment ParseGlueHAlign(const FString& Value)
{
	FString Normalized = Value;
	Normalized.TrimStartAndEndInline();
	Normalized.ToUpperInline();
	if (Normalized == TEXT("LEFT"))
	{
		return HAlign_Left;
	}
	if (Normalized == TEXT("JUSTIFY") || Normalized == TEXT("DISTRIBUTE") || Normalized == TEXT("DISTRIBUTED"))
	{
		return HAlign_Left;
	}
	if (Normalized == TEXT("RIGHT"))
	{
		return HAlign_Right;
	}
	return HAlign_Center;
}

EVerticalAlignment ParseGlueVAlign(const FString& Value)
{
	FString Normalized = Value;
	Normalized.TrimStartAndEndInline();
	Normalized.ToUpperInline();
	if (Normalized == TEXT("TOP"))
	{
		return VAlign_Top;
	}
	if (Normalized == TEXT("BOTTOM"))
	{
		return VAlign_Bottom;
	}
	return VAlign_Center;
}

TSharedPtr<FSlateBrush> MakeTextureBrush(UTexture2D* Texture, const FVector2D& Size)
{
	if (!Texture)
	{
		return nullptr;
	}

	TSharedRef<FSlateBrush> Brush = MakeShared<FSlateBrush>();
	Brush->SetResourceObject(Texture);
	Brush->ImageSize = Size;
	Brush->DrawAs = ESlateBrushDrawType::Image;
	return Brush;
}

TSharedPtr<FSlateBrush> MakeTextureRegionBrush(UTexture2D* Texture, const FVector2D& Size, const FBox2f& UVRegion)
{
	if (!Texture)
	{
		return nullptr;
	}

	TSharedRef<FSlateBrush> Brush = MakeShared<FSlateBrush>();
	Brush->SetResourceObject(Texture);
	Brush->ImageSize = Size;
	Brush->DrawAs = ESlateBrushDrawType::Image;
	Brush->SetUVRegion(UVRegion);
	return Brush;
}

FBox2f ToSlateUVRegion(const FWoWGlueUVRect& UVRect)
{
	return FBox2f(
		FVector2f(UVRect.Left, UVRect.Top),
		FVector2f(UVRect.Right, UVRect.Bottom));
}

TSharedPtr<FSlateBrush> MakeTextureBoxBrush(UTexture2D* Texture, const FVector2D& Size, float EdgeSize)
{
	if (!Texture)
	{
		return nullptr;
	}

	TSharedRef<FSlateBrush> Brush = MakeShared<FSlateBrush>();
	Brush->SetResourceObject(Texture);
	Brush->ImageSize = Size;
	Brush->DrawAs = ESlateBrushDrawType::Box;
	const float TextureWidth = FMath::Max(1.0f, static_cast<float>(Texture->GetSizeX()));
	const float TextureHeight = FMath::Max(1.0f, static_cast<float>(Texture->GetSizeY()));
	const FMargin Margin(
		FMath::Clamp(EdgeSize / TextureWidth, 0.0f, 0.5f),
		FMath::Clamp(EdgeSize / TextureHeight, 0.0f, 0.5f));
	Brush->Margin = Margin;
	return Brush;
}

void BuildHorizontalAtlasRegions(UTexture2D* Texture, int32 RegionCount, TArray<TSharedPtr<FSlateBrush>>& OutBrushes)
{
	OutBrushes.Reset();
	if (!Texture || RegionCount <= 0)
	{
		return;
	}

	const float TextureWidth = FMath::Max(1.0f, static_cast<float>(Texture->GetSizeX()));
	const float TextureHeight = FMath::Max(1.0f, static_cast<float>(Texture->GetSizeY()));
	const float RegionWidth = TextureWidth / static_cast<float>(RegionCount);
	for (int32 RegionIndex = 0; RegionIndex < RegionCount; ++RegionIndex)
	{
		const float Left = (RegionWidth * static_cast<float>(RegionIndex)) / TextureWidth;
		const float Right = (RegionWidth * static_cast<float>(RegionIndex + 1)) / TextureWidth;
		const FBox2f UVRegion(FVector2f(Left, 0.0f), FVector2f(Right, 1.0f));
		OutBrushes.Add(MakeTextureRegionBrush(Texture, FVector2D(RegionWidth, TextureHeight), UVRegion));
	}
}

class SWoWBackdrop : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SWoWBackdrop) {}
		SLATE_ARGUMENT(UTexture2D*, BackgroundTexture)
		SLATE_ARGUMENT(UTexture2D*, EdgeTexture)
		SLATE_ARGUMENT(FVector2D, DesiredSize)
		SLATE_ARGUMENT(float, EdgeSize)
		SLATE_ARGUMENT(FMargin, BackgroundInsets)
		SLATE_ARGUMENT(FLinearColor, BackgroundTint)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		DesiredSize = InArgs._DesiredSize;
		EdgeSize = FMath::Max(1.0f, InArgs._EdgeSize);
		BackgroundInsets = InArgs._BackgroundInsets;
		BackgroundTint = InArgs._BackgroundTint;
		BackgroundTexture = InArgs._BackgroundTexture;
		EdgeTexture = InArgs._EdgeTexture;
		BackgroundBrush = MakeTextureBrush(BackgroundTexture, DesiredSize);
		BuildHorizontalAtlasRegions(EdgeTexture, 8, EdgeBrushes);
	}

	virtual FVector2D ComputeDesiredSize(float) const override
	{
		return DesiredSize;
	}

	virtual int32 OnPaint(
		const FPaintArgs& Args,
		const FGeometry& AllottedGeometry,
		const FSlateRect& MyCullingRect,
		FSlateWindowElementList& OutDrawElements,
		int32 LayerId,
		const FWidgetStyle& InWidgetStyle,
		bool bParentEnabled) const override
	{
		const FVector2D LocalSize = AllottedGeometry.GetLocalSize();
		const float E = FMath::Clamp(EdgeSize, 1.0f, FMath::Min(LocalSize.X, LocalSize.Y) * 0.5f);

		if (BackgroundBrush.IsValid())
		{
			const FVector2D BackgroundPosition(BackgroundInsets.Left, BackgroundInsets.Top);
			const FVector2D BackgroundSize(
				FMath::Max(1.0f, LocalSize.X - BackgroundInsets.Left - BackgroundInsets.Right),
				FMath::Max(1.0f, LocalSize.Y - BackgroundInsets.Top - BackgroundInsets.Bottom));
			FSlateDrawElement::MakeBox(
				OutDrawElements,
				LayerId,
				AllottedGeometry.ToPaintGeometry(BackgroundSize, FSlateLayoutTransform(BackgroundPosition)),
				BackgroundBrush.Get(),
				ESlateDrawEffect::None,
				BackgroundTint);
		}

		if (EdgeBrushes.Num() < 8)
		{
			return LayerId + 1;
		}

		auto DrawTile = [&](int32 TileIndex, const FVector2D& Position, const FVector2D& DrawSize, int32 PaintLayer)
		{
			if (!EdgeBrushes.IsValidIndex(TileIndex) || !EdgeBrushes[TileIndex].IsValid())
			{
				return;
			}
			FSlateDrawElement::MakeBox(
				OutDrawElements,
				PaintLayer,
				AllottedGeometry.ToPaintGeometry(DrawSize, FSlateLayoutTransform(Position)),
				EdgeBrushes[TileIndex].Get(),
				ESlateDrawEffect::None,
				FLinearColor::White);
		};

		auto DrawRotatedTile = [&](int32 TileIndex, const FVector2D& Position, const FVector2D& DrawSize, float Angle, int32 PaintLayer)
		{
			if (!EdgeBrushes.IsValidIndex(TileIndex) || !EdgeBrushes[TileIndex].IsValid())
			{
				return;
			}
			const FVector2D Pivot = DrawSize * 0.5f;
			const FVector2D RotatedLocalSize(DrawSize.Y, DrawSize.X);
			const FSlateLayoutTransform RotatedTransform(Position + Pivot - RotatedLocalSize * 0.5f);
			FSlateDrawElement::MakeRotatedBox(
				OutDrawElements,
				PaintLayer,
				AllottedGeometry.ToPaintGeometry(RotatedLocalSize, RotatedTransform),
				EdgeBrushes[TileIndex].Get(),
				ESlateDrawEffect::None,
				Angle,
				RotatedLocalSize * 0.5f,
				FSlateDrawElement::RelativeToElement,
				FLinearColor::White);
		};

		auto DrawVerticalTiled = [&](int32 TileIndex, const FVector2D& Position, float Height, int32 PaintLayer)
		{
			float Painted = 0.0f;
			while (Painted < Height - KINDA_SMALL_NUMBER)
			{
				const float Segment = FMath::Min(E, Height - Painted);
				DrawTile(TileIndex, FVector2D(Position.X, Position.Y + Painted), FVector2D(E, Segment), PaintLayer);
				Painted += Segment;
			}
		};

		auto DrawHorizontalTiled = [&](int32 TileIndex, const FVector2D& Position, float Width, float Angle, int32 PaintLayer)
		{
			float Painted = 0.0f;
			while (Painted < Width - KINDA_SMALL_NUMBER)
			{
				const float Segment = FMath::Min(E, Width - Painted);
				DrawRotatedTile(TileIndex, FVector2D(Position.X + Painted, Position.Y), FVector2D(Segment, E), Angle, PaintLayer);
				Painted += Segment;
			}
		};

		const int32 EdgeLayer = LayerId + 1;
		const FVector2D TopLeft(0.0f, 0.0f);
		const FVector2D TopRight(LocalSize.X - E, 0.0f);
		const FVector2D BottomLeft(0.0f, LocalSize.Y - E);
		const FVector2D BottomRight(LocalSize.X - E, LocalSize.Y - E);
		const FVector2D CornerSize(E, E);
		const float HorizontalLength = FMath::Max(1.0f, LocalSize.X - E * 2.0f);
		const float VerticalLength = FMath::Max(1.0f, LocalSize.Y - E * 2.0f);

		// Tooltip-style borders use the same atlas order documented by Blizzard's
		// OptionsFrame templates: left, right, top/bottom strokes, then TL/TR/BL/BR.
		DrawTile(4, TopLeft, CornerSize, EdgeLayer);
		DrawTile(5, TopRight, CornerSize, EdgeLayer);
		DrawTile(6, BottomLeft, CornerSize, EdgeLayer);
		DrawTile(7, BottomRight, CornerSize, EdgeLayer);

		DrawHorizontalTiled(2, FVector2D(E, 0.0f), HorizontalLength, UE_HALF_PI, EdgeLayer);
		DrawHorizontalTiled(3, FVector2D(E, LocalSize.Y - E), HorizontalLength, UE_HALF_PI, EdgeLayer);
		DrawVerticalTiled(0, FVector2D(0.0f, E), VerticalLength, EdgeLayer);
		DrawVerticalTiled(1, FVector2D(LocalSize.X - E, E), VerticalLength, EdgeLayer);

		return EdgeLayer + 1;
	}

private:
	FVector2D DesiredSize = FVector2D::ZeroVector;
	float EdgeSize = 16.0f;
	FMargin BackgroundInsets = FMargin(0.0f);
	UTexture2D* BackgroundTexture = nullptr;
	UTexture2D* EdgeTexture = nullptr;
	FLinearColor BackgroundTint = FLinearColor(0.0f, 0.0f, 0.0f, 0.78f);
	TSharedPtr<FSlateBrush> BackgroundBrush;
	TArray<TSharedPtr<FSlateBrush>> EdgeBrushes;
};

TSharedRef<SWidget> MakeBackdropAtlasWidget(
	UTexture2D* BackgroundTexture,
	UTexture2D* EdgeTexture,
	const FVector2D& Size,
	float EdgeSize,
	const FMargin& Insets,
	const FLinearColor& BackgroundTint,
	TSharedRef<SWidget> Content)
{
	return SNew(SOverlay)
		+ SOverlay::Slot()
		.HAlign(HAlign_Fill)
		.VAlign(VAlign_Fill)
		[
			SNew(SWoWBackdrop)
			.BackgroundTexture(BackgroundTexture)
			.EdgeTexture(EdgeTexture)
			.DesiredSize(Size)
			.EdgeSize(EdgeSize)
			.BackgroundInsets(Insets)
			.BackgroundTint(BackgroundTint)
		]
		+ SOverlay::Slot()
		.HAlign(HAlign_Fill)
		.VAlign(VAlign_Fill)
		[
			Content
		];
}

FString AlphaModeToCacheSuffix(EWoWGlueAlphaMode AlphaMode)
{
	switch (AlphaMode)
	{
	case EWoWGlueAlphaMode::Add:
		return TEXT("|ADD");
	case EWoWGlueAlphaMode::AlphaKey:
		return TEXT("|ALPHAKEY");
	case EWoWGlueAlphaMode::Disable:
		return TEXT("|DISABLE");
	case EWoWGlueAlphaMode::Mod:
		return TEXT("|MOD");
	default:
		return TEXT("|BLEND");
	}
}

FString MakeRenderPolicyCacheSuffix()
{
	return FString::Printf(
		TEXT("|ADDPOLICY:Draw=%.3f;Rgb=%.3f;Alpha=%.3f"),
		GGlueRenderPolicy.AddDrawTintMultiplier,
		GGlueRenderPolicy.AddLoadRgbMultiplier,
		GGlueRenderPolicy.AddLoadAlphaMultiplier);
}

FLinearColor ApplyAlphaModeSlateColor(FLinearColor Color, EWoWGlueAlphaMode AlphaMode)
{
	if (AlphaMode == EWoWGlueAlphaMode::Add)
	{
		Color.R *= GGlueRenderPolicy.AddDrawTintMultiplier;
		Color.G *= GGlueRenderPolicy.AddDrawTintMultiplier;
		Color.B *= GGlueRenderPolicy.AddDrawTintMultiplier;
	}
	return Color;
}

float ReadJsonFloat(TSharedPtr<FJsonObject> Object, const TCHAR* FieldName, float DefaultValue, float MinValue, float MaxValue)
{
	if (!Object.IsValid() || !Object->HasTypedField<EJson::Number>(FieldName))
	{
		return DefaultValue;
	}
	return FMath::Clamp(static_cast<float>(Object->GetNumberField(FieldName)), MinValue, MaxValue);
}

void LoadGlueRenderPolicy()
{
	GGlueRenderPolicy = FGlueRenderPolicy();

	const FString PolicyPath = FPaths::ProjectDir() / TEXT("Data/Config/GlueRenderPolicy.json");
	FString JsonText;
	if (!FFileHelper::LoadFileToString(JsonText, *PolicyPath))
	{
		UE_LOG(LogTemp, Display, TEXT("Glue render policy using defaults: %s"), *PolicyPath);
		return;
	}

	TSharedPtr<FJsonObject> RootObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
	if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("Glue render policy parse failed, using defaults: %s"), *PolicyPath);
		return;
	}

	TSharedPtr<FJsonObject> AlphaModesObject = RootObject->GetObjectField(TEXT("alphaModes"));
	TSharedPtr<FJsonObject> AddObject = AlphaModesObject.IsValid()
		? AlphaModesObject->GetObjectField(TEXT("ADD"))
		: nullptr;

	GGlueRenderPolicy.AddDrawTintMultiplier = ReadJsonFloat(AddObject, TEXT("drawTintMultiplier"), GGlueRenderPolicy.AddDrawTintMultiplier, 0.0f, 8.0f);
	GGlueRenderPolicy.AddLoadRgbMultiplier = ReadJsonFloat(AddObject, TEXT("loadRgbMultiplier"), GGlueRenderPolicy.AddLoadRgbMultiplier, 0.0f, 8.0f);
	GGlueRenderPolicy.AddLoadAlphaMultiplier = ReadJsonFloat(AddObject, TEXT("loadAlphaMultiplier"), GGlueRenderPolicy.AddLoadAlphaMultiplier, 0.0f, 8.0f);

	UE_LOG(LogTemp, Display, TEXT("Glue render policy loaded: ADD draw=%.3f rgb=%.3f alpha=%.3f"),
		GGlueRenderPolicy.AddDrawTintMultiplier,
		GGlueRenderPolicy.AddLoadRgbMultiplier,
		GGlueRenderPolicy.AddLoadAlphaMultiplier);
}

bool IsGlueDistributedJustify(const FString& JustifyH)
{
	FString NormalizedJustifyH = JustifyH;
	NormalizedJustifyH.TrimStartAndEndInline();
	NormalizedJustifyH.ToUpperInline();
	return NormalizedJustifyH == TEXT("JUSTIFY") || NormalizedJustifyH == TEXT("DISTRIBUTE") || NormalizedJustifyH == TEXT("DISTRIBUTED");
}

class SWoWEditableTextBox : public SEditableTextBox
{
public:
	SLATE_BEGIN_ARGS(SWoWEditableTextBox) {}
		SLATE_STYLE_ARGUMENT(FEditableTextBoxStyle, Style)
		SLATE_ATTRIBUTE(EVisibility, Visibility)
		SLATE_ATTRIBUTE(FText, Text)
		SLATE_ATTRIBUTE(FText, HintText)
		SLATE_ATTRIBUTE(bool, IsPassword)
		SLATE_ATTRIBUTE(bool, IsInteractionEnabled)
		SLATE_ATTRIBUTE(bool, IsReadOnly)
		SLATE_ATTRIBUTE(ETextJustify::Type, Justification)
		SLATE_ATTRIBUTE(bool, SelectAllTextWhenFocused)
		SLATE_EVENT(FOnTextChanged, OnTextChanged)
		SLATE_EVENT(FOnCursorMovedWithSelection, OnCursorMovedWithSelection)
		SLATE_EVENT(FOnKeyDown, OnKeyDownHandler)
		SLATE_EVENT(FOnClicked, OnTabPressed)
		SLATE_EVENT(FOnClicked, OnEnterPressed)
		SLATE_EVENT(FOnClicked, OnEscapePressed)
		SLATE_EVENT(FSimpleDelegate, OnEditFocusGained)
		SLATE_EVENT(FSimpleDelegate, OnEditFocusLost)
		SLATE_EVENT(FSimpleDelegate, OnEditMouseDown)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		OnEditFocusGained = InArgs._OnEditFocusGained;
		OnEditFocusLost = InArgs._OnEditFocusLost;
		OnEditMouseDown = InArgs._OnEditMouseDown;
		OnTabPressed = InArgs._OnTabPressed;
		OnEnterPressed = InArgs._OnEnterPressed;
		OnEscapePressed = InArgs._OnEscapePressed;
		IsInteractionEnabled = InArgs._IsInteractionEnabled;

		SEditableTextBox::Construct(SEditableTextBox::FArguments()
			.Style(InArgs._Style)
			.Visibility(InArgs._Visibility)
			.Text(InArgs._Text)
			.HintText(InArgs._HintText)
			.IsPassword(InArgs._IsPassword)
			.IsReadOnly(InArgs._IsReadOnly)
			.Justification(InArgs._Justification)
			.SelectAllTextWhenFocused(InArgs._SelectAllTextWhenFocused)
			.ClearKeyboardFocusOnCommit(false)
			.RevertTextOnEscape(false)
			.OnTextChanged(InArgs._OnTextChanged)
			.OnCursorMovedWithSelection(InArgs._OnCursorMovedWithSelection)
			.OnKeyDownHandler(InArgs._OnKeyDownHandler));
	}

	virtual FReply OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override
	{
		if (!IsInteractionEnabled.Get(true))
		{
			return FReply::Handled();
		}
		if (OnEditMouseDown.IsBound())
		{
			OnEditMouseDown.Execute();
		}
		Invalidate(EInvalidateWidgetReason::Paint);
		return SEditableTextBox::OnMouseButtonDown(MyGeometry, MouseEvent);
	}

	virtual FReply OnFocusReceived(const FGeometry& MyGeometry, const FFocusEvent& InFocusEvent) override
	{
		if (!IsInteractionEnabled.Get(true))
		{
			return FReply::Handled();
		}
		if (OnEditFocusGained.IsBound())
		{
			OnEditFocusGained.Execute();
		}
		Invalidate(EInvalidateWidgetReason::Paint);
		return SEditableTextBox::OnFocusReceived(MyGeometry, InFocusEvent);
	}

	virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override
	{
		const FReply Reply = HandleTabKey(InKeyEvent);
		if (Reply.IsEventHandled())
		{
			return Reply;
		}
		return SEditableTextBox::OnKeyDown(MyGeometry, InKeyEvent);
	}

	virtual void OnFocusLost(const FFocusEvent& InFocusEvent) override
	{
		SEditableTextBox::OnFocusLost(InFocusEvent);
		if (OnEditFocusLost.IsBound())
		{
			OnEditFocusLost.Execute();
		}
		Invalidate(EInvalidateWidgetReason::Paint);
	}

private:
	FReply HandleTabKey(const FKeyEvent& InKeyEvent)
	{
		if (InKeyEvent.GetKey() == EKeys::Tab && OnTabPressed.IsBound())
		{
			return OnTabPressed.Execute();
		}
		if (InKeyEvent.GetKey() == EKeys::Enter && OnEnterPressed.IsBound())
		{
			return OnEnterPressed.Execute();
		}
		if (InKeyEvent.GetKey() == EKeys::Escape && OnEscapePressed.IsBound())
		{
			return OnEscapePressed.Execute();
		}
		return FReply::Unhandled();
	}

	TAttribute<bool> IsInteractionEnabled;
	FSimpleDelegate OnEditFocusGained;
	FSimpleDelegate OnEditFocusLost;
	FSimpleDelegate OnEditMouseDown;
	FOnClicked OnTabPressed;
	FOnClicked OnEnterPressed;
	FOnClicked OnEscapePressed;
};

class SWoWMouseBlocker : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SWoWMouseBlocker) {}
		SLATE_ATTRIBUTE(EVisibility, Visibility)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		SetVisibility(InArgs._Visibility);
	}

	virtual FVector2D ComputeDesiredSize(float) const override
	{
		return FVector2D::ZeroVector;
	}

	virtual int32 OnPaint(
		const FPaintArgs&,
		const FGeometry&,
		const FSlateRect&,
		FSlateWindowElementList&,
		int32 LayerId,
		const FWidgetStyle&,
		bool) const override
	{
		return LayerId;
	}

	virtual FReply OnMouseButtonDown(const FGeometry&, const FPointerEvent&) override
	{
		return FReply::Handled();
	}

	virtual FReply OnMouseButtonUp(const FGeometry&, const FPointerEvent&) override
	{
		return FReply::Handled();
	}

	virtual FReply OnMouseMove(const FGeometry&, const FPointerEvent& MouseEvent) override
	{
		return FReply::Handled();
	}

	virtual FReply OnMouseWheel(const FGeometry&, const FPointerEvent&) override
	{
		return FReply::Handled();
	}

};

struct FWoWSimpleHTMLGlyph
{
	FString Text;
	FString Href;
	FString LinkText;
	TOptional<FLinearColor> Color;
	TOptional<ETextJustify::Type> Justification;
	float FontSize = 0.0f;
	float Width = 0.0f;
};

struct FWoWSimpleHTMLPaintRun
{
	FString Text;
	FString Href;
	FString LinkText;
	FVector2D Position = FVector2D::ZeroVector;
	FVector2D Size = FVector2D::ZeroVector;
};

struct FWoWSimpleHTMLPaintLine
{
	TArray<FWoWSimpleHTMLGlyph> Glyphs;
	TOptional<ETextJustify::Type> Justification;
	float Width = 0.0f;
	bool bParagraphEnd = false;
};

class SWoWSimpleHTMLText : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SWoWSimpleHTMLText) {}
		SLATE_ARGUMENT(TSharedPtr<FWoWGlueFrame>, Frame)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		Frame = InArgs._Frame;
	}

	virtual FVector2D ComputeDesiredSize(float) const override
	{
		const TSharedPtr<FWoWGlueFrame> PinnedFrame = Frame.Pin();
		return PinnedFrame.IsValid() ? MeasureDesiredSize(*PinnedFrame) : FVector2D::ZeroVector;
	}

	static FVector2D MeasureDesiredSize(const FWoWGlueFrame& SourceFrame)
	{
		const FSlateFontInfo Font = SourceFrame.GetSlateFont();
		const FVector2D FrameSize = SourceFrame.GetSize();
		const float WrapWidth = GetWrapWidth(SourceFrame);
		const TArray<FWoWSimpleHTMLPaintLine> Lines = BuildLines(SourceFrame, Font, WrapWidth);
		float Height = 0.0f;
		float Width = 1.0f;

		for (const FWoWSimpleHTMLPaintLine& Line : Lines)
		{
			const float LineHeight = GetLineHeight(SourceFrame, Font, Line);
			Height += LineHeight;
			Width = FMath::Max(Width, Line.Width);
			if (Line.bParagraphEnd)
			{
				Height += SourceFrame.GetTextParagraphSpacing();
			}
		}

		const float DesiredWidth = SourceFrame.GetFrameType() == EWoWGlueFrameType::FontString && !SourceFrame.HasExplicitSize()
			? Width
			: FrameSize.X;
		const float DesiredHeight = SourceFrame.GetFrameType() == EWoWGlueFrameType::FontString && !SourceFrame.HasExplicitSize()
			? Height
			: FMath::Max(FrameSize.Y, Height);
		return FVector2D(FMath::Max(1.0f, DesiredWidth), FMath::Max(1.0f, DesiredHeight));
	}

	virtual int32 OnPaint(
		const FPaintArgs& Args,
		const FGeometry& AllottedGeometry,
		const FSlateRect& MyCullingRect,
		FSlateWindowElementList& OutDrawElements,
		int32 LayerId,
		const FWidgetStyle& InWidgetStyle,
		bool bParentEnabled) const override
	{
		const TSharedPtr<FWoWGlueFrame> PinnedFrame = Frame.Pin();
		if (!PinnedFrame.IsValid() || !PinnedFrame->IsShown())
		{
			return LayerId;
		}

		LinkRuns.Reset();
		const FSlateFontInfo Font = PinnedFrame->GetSlateFont();
		const FVector2D FrameSize = AllottedGeometry.GetLocalSize();
		const TArray<FWoWSimpleHTMLPaintLine> Lines = BuildLines(*PinnedFrame, Font, GetWrapWidth(*PinnedFrame));
		float Y = 0.0f;

		const FLinearColor TextColor = PinnedFrame->GetTextSlateColor().GetSpecifiedColor() * InWidgetStyle.GetColorAndOpacityTint();
		const FLinearColor LinkColor(0.024f, 1.0f, 0.027f, TextColor.A);
		const FLinearColor ShadowColor = PinnedFrame->GetShadowColor() * InWidgetStyle.GetColorAndOpacityTint();
		const FVector2D ShadowOffset = PinnedFrame->GetShadowOffset();

		for (const FWoWSimpleHTMLPaintLine& Line : Lines)
		{
			const float LineHeight = GetLineHeight(*PinnedFrame, Font, Line);
			if (Line.Glyphs.IsEmpty())
			{
				Y += LineHeight + PinnedFrame->GetTextParagraphSpacing();
				continue;
			}

			const bool bJustifyLine = !Line.bParagraphEnd || PinnedFrame->ShouldTextJustifyLastLine();
			const int32 GapCount = FMath::Max(0, Line.Glyphs.Num() - 1);
			const bool bDistributedLine = IsGlueDistributedJustify(PinnedFrame->GetJustifyH());
			const float ExtraGap = bDistributedLine && bJustifyLine && GapCount > 0
				? FMath::Max(0.0f, (FrameSize.X - Line.Width) / static_cast<float>(GapCount))
				: 0.0f;
			float X = 0.0f;
			if (!bDistributedLine)
			{
				const ETextJustify::Type Justification = Line.Justification.IsSet()
					? Line.Justification.GetValue()
					: PinnedFrame->GetTextJustification();
				if (Justification == ETextJustify::Center)
				{
					X = FMath::Max(0.0f, (FrameSize.X - Line.Width) * 0.5f);
				}
				else if (Justification == ETextJustify::Right)
				{
					X = FMath::Max(0.0f, FrameSize.X - Line.Width);
				}
			}

			for (const FWoWSimpleHTMLGlyph& Glyph : Line.Glyphs)
			{
				const FVector2D Position(X, Y);
				const FVector2D GlyphSize(Glyph.Width + ExtraGap, LineHeight);
				const FText GlyphText = FText::FromString(Glyph.Text);
				FLinearColor GlyphColor = Glyph.Color.IsSet() ? Glyph.Color.GetValue() : (Glyph.Href.IsEmpty() ? TextColor : LinkColor);
				GlyphColor.A *= TextColor.A;

				if (ShadowColor.A > 0.0f && !ShadowOffset.IsNearlyZero())
				{
					FSlateFontInfo GlyphFont = Font;
					GlyphFont.Size = Glyph.FontSize;
					FSlateDrawElement::MakeText(
						OutDrawElements,
						LayerId,
						AllottedGeometry.ToPaintGeometry(FVector2D(Glyph.Width, LineHeight), FSlateLayoutTransform(Position + ShadowOffset)),
						GlyphText,
						GlyphFont,
						ESlateDrawEffect::None,
						ShadowColor);
				}

				FSlateFontInfo GlyphFont = Font;
				GlyphFont.Size = Glyph.FontSize;
				FSlateDrawElement::MakeText(
					OutDrawElements,
					LayerId + 1,
					AllottedGeometry.ToPaintGeometry(FVector2D(Glyph.Width, LineHeight), FSlateLayoutTransform(Position)),
					GlyphText,
					GlyphFont,
					ESlateDrawEffect::None,
					GlyphColor);

				if (!Glyph.Href.IsEmpty())
				{
					FWoWSimpleHTMLPaintRun PaintRun;
					PaintRun.Text = Glyph.Text;
					PaintRun.Href = Glyph.Href;
					PaintRun.LinkText = Glyph.LinkText;
					PaintRun.Position = Position;
					PaintRun.Size = GlyphSize;
					LinkRuns.Add(MoveTemp(PaintRun));
				}

				X += Glyph.Width + ExtraGap;
			}

			Y += LineHeight;
			if (Line.bParagraphEnd)
			{
				Y += PinnedFrame->GetTextParagraphSpacing();
			}
		}

		return LayerId + 1;
	}

	virtual FReply OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override
	{
		return HandleHyperlinkMouseEvent(MyGeometry, MouseEvent);
	}

	virtual FReply OnMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override
	{
		return HandleHyperlinkMouseEvent(MyGeometry, MouseEvent);
	}

	virtual FCursorReply OnCursorQuery(const FGeometry& MyGeometry, const FPointerEvent& CursorEvent) const override
	{
		return FindLinkAt(MyGeometry.AbsoluteToLocal(CursorEvent.GetScreenSpacePosition()))
			? FCursorReply::Cursor(EMouseCursor::Hand)
			: FCursorReply::Unhandled();
	}

	private:
	static float GetWrapWidth(const FWoWGlueFrame& SourceFrame)
	{
		if (SourceFrame.GetFrameType() == EWoWGlueFrameType::FontString && !SourceFrame.HasExplicitSize())
		{
			return 4096.0f;
		}
		return FMath::Max(1.0f, SourceFrame.GetSize().X);
	}

	static float GetLineHeight(const FWoWGlueFrame& SourceFrame, const FSlateFontInfo& BaseFont, const FWoWSimpleHTMLPaintLine& Line)
	{
		float MaxFontSize = BaseFont.Size;
		for (const FWoWSimpleHTMLGlyph& Glyph : Line.Glyphs)
		{
			MaxFontSize = FMath::Max(MaxFontSize, Glyph.FontSize);
		}
		return FMath::Max(1.0f, MaxFontSize * SourceFrame.GetSimpleHTMLLineHeightPercentage()) + SourceFrame.GetSimpleHTMLLineSpacing();
	}

	const FWoWSimpleHTMLPaintRun* FindLinkAt(const FVector2D& LocalPosition) const
	{
		constexpr float HitPadding = 3.0f;
		for (const FWoWSimpleHTMLPaintRun& Run : LinkRuns)
		{
			const bool bInsideX = LocalPosition.X >= Run.Position.X - HitPadding && LocalPosition.X <= Run.Position.X + Run.Size.X + HitPadding;
			const bool bInsideY = LocalPosition.Y >= Run.Position.Y - HitPadding && LocalPosition.Y <= Run.Position.Y + Run.Size.Y + HitPadding;
			if (bInsideX && bInsideY)
			{
				return &Run;
			}
		}
		return nullptr;
	}

	FReply HandleHyperlinkMouseEvent(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
	{
		const TSharedPtr<FWoWGlueFrame> PinnedFrame = Frame.Pin();
		if (!PinnedFrame.IsValid())
		{
			return FReply::Unhandled();
		}

		if (const FWoWSimpleHTMLPaintRun* LinkRun = FindLinkAt(MyGeometry.AbsoluteToLocal(MouseEvent.GetScreenSpacePosition())))
		{
			PinnedFrame->TriggerHyperlinkClick(LinkRun->Href, LinkRun->LinkText);
			return FReply::Handled();
		}

		return FReply::Unhandled();
	}

	static TArray<FWoWSimpleHTMLGlyph> BuildGlyphs(const FWoWGlueFrame& SourceFrame, const FSlateFontInfo& Font)
	{
		TArray<FWoWSimpleHTMLGlyph> Glyphs;
		const TArray<FSimpleHTMLTextRun> Runs = SourceFrame.GetFrameType() == EWoWGlueFrameType::SimpleHTML
			? ParseSimpleHTMLRuns(SourceFrame.GetTextString())
			: ParseGluePlainTextRuns(SourceFrame.GetTextString());
		for (const FSimpleHTMLTextRun& Run : Runs)
		{
			const FString LinkText = Run.Text;
			for (int32 CharIndex = 0; CharIndex < Run.Text.Len(); ++CharIndex)
			{
				const TCHAR Character = Run.Text[CharIndex];
				FSlateFontInfo GlyphFont = Font;
				if (Run.FontSize.IsSet())
				{
					const FSimpleHTMLFontSize& RunFontSize = Run.FontSize.GetValue();
					GlyphFont.Size = RunFontSize.bAbsolutePixels
						? RunFontSize.Value
						: FMath::Max(1.0f, Font.Size * RunFontSize.Value);
				}
				FWoWSimpleHTMLGlyph Glyph;
				Glyph.Text = FString::Chr(Character);
				Glyph.Href = Run.Href;
				Glyph.LinkText = LinkText;
				Glyph.Color = Run.Color;
				Glyph.Justification = Run.Justification;
				Glyph.FontSize = GlyphFont.Size;
				Glyph.Width = MeasureGlueText(Glyph.Text, GlyphFont);
				Glyphs.Add(MoveTemp(Glyph));
			}
		}
		return Glyphs;
	}

	static TArray<FWoWSimpleHTMLPaintLine> BuildLines(const FWoWGlueFrame& SourceFrame, const FSlateFontInfo& Font, float WrapWidth)
	{
		TArray<FWoWSimpleHTMLPaintLine> Lines;
		Lines.AddDefaulted();
		float CurrentTargetWidth = FMath::Max(1.0f, WrapWidth);
		const float IndentWidth = SourceFrame.GetTextFirstLineIndent();
		const float SpaceWidth = FMath::Max(1.0f, MeasureGlueText(TEXT(" "), Font));
		const int32 IndentSpaces = FMath::Max(0, FMath::RoundToInt(IndentWidth / SpaceWidth));
		if (IndentSpaces > 0)
		{
			FWoWSimpleHTMLGlyph IndentGlyph;
			IndentGlyph.Text = FString::ChrN(IndentSpaces, TEXT(' '));
			IndentGlyph.Width = IndentWidth;
			Lines.Last().Glyphs.Add(MoveTemp(IndentGlyph));
			Lines.Last().Width += IndentWidth;
		}

		for (const FWoWSimpleHTMLGlyph& Glyph : BuildGlyphs(SourceFrame, Font))
		{
			if (Glyph.Text == TEXT("\n"))
			{
				Lines.Last().bParagraphEnd = true;
				Lines.AddDefaulted();
				CurrentTargetWidth = FMath::Max(1.0f, WrapWidth);
				if (IndentSpaces > 0)
				{
					FWoWSimpleHTMLGlyph IndentGlyph;
					IndentGlyph.Text = FString::ChrN(IndentSpaces, TEXT(' '));
					IndentGlyph.Width = IndentWidth;
					Lines.Last().Glyphs.Add(MoveTemp(IndentGlyph));
					Lines.Last().Width += IndentWidth;
				}
				continue;
			}

			if (!Lines.Last().Justification.IsSet() && Glyph.Justification.IsSet())
			{
				Lines.Last().Justification = Glyph.Justification;
			}

			if (!Lines.Last().Glyphs.IsEmpty() && Lines.Last().Width + Glyph.Width > CurrentTargetWidth)
			{
				Lines.AddDefaulted();
				Lines.Last().Justification = Glyph.Justification;
				CurrentTargetWidth = FMath::Max(1.0f, WrapWidth);
			}

			Lines.Last().Glyphs.Add(Glyph);
			Lines.Last().Width += Glyph.Width;
		}

		while (Lines.Num() > 0 && Lines.Last().Glyphs.IsEmpty())
		{
			Lines.Pop();
		}
		if (Lines.Num() > 0)
		{
			Lines.Last().bParagraphEnd = true;
		}
		return Lines;
	}

	TWeakPtr<FWoWGlueFrame> Frame;
	mutable TArray<FWoWSimpleHTMLPaintRun> LinkRuns;
};

class SWoWGlueScrollFrame : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SWoWGlueScrollFrame) {}
		SLATE_ARGUMENT(TSharedPtr<FWoWGlueFrame>, Frame)
		SLATE_ARGUMENT(TSharedPtr<FWoWGlueFrame>, ScrollChild)
	SLATE_END_ARGS()

	enum class EScrollPart : uint8
	{
		None,
		UpArrow,
		DownArrow,
		Thumb,
		Track
	};

	void Construct(const FArguments& InArgs)
	{
		Frame = InArgs._Frame;
		ScrollChild = InArgs._ScrollChild;

		ChildSlot
		.HAlign(HAlign_Fill)
		.VAlign(VAlign_Fill)
		[
			SNew(SBox)
			.WidthOverride(TAttribute<FOptionalSize>::Create(TAttribute<FOptionalSize>::FGetter::CreateSP(this, &SWoWGlueScrollFrame::GetViewportWidth)))
			.HeightOverride(TAttribute<FOptionalSize>::Create(TAttribute<FOptionalSize>::FGetter::CreateSP(this, &SWoWGlueScrollFrame::GetViewportHeight)))
			[
				SAssignNew(Canvas, SConstraintCanvas)
			]
		];

		if (Canvas.IsValid() && ScrollChild.IsValid())
		{
			Canvas->AddSlot()
				.Anchors(FAnchors(0.0f, 0.0f))
				.Alignment(FVector2D::ZeroVector)
				.Offset(TAttribute<FMargin>::Create(TAttribute<FMargin>::FGetter::CreateSP(this, &SWoWGlueScrollFrame::GetScrollChildOffset)))
				.ZOrder(0)
				[
					ScrollChild.Pin()->BuildSubtreeWidget()
				];
		}
	}

	virtual FVector2D ComputeDesiredSize(float) const override
	{
		const TSharedPtr<FWoWGlueFrame> PinnedFrame = Frame.Pin();
		return PinnedFrame.IsValid() ? PinnedFrame->GetSize() : FVector2D::ZeroVector;
	}

	virtual FReply OnMouseWheel(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override
	{
		if (const TSharedPtr<FWoWGlueFrame> PinnedFrame = Frame.Pin())
		{
			PinnedFrame->HandleMouseWheel(MouseEvent.GetWheelDelta());
			return FReply::Handled();
		}
		return FReply::Unhandled();
	}

	virtual int32 OnPaint(
		const FPaintArgs& Args,
		const FGeometry& AllottedGeometry,
		const FSlateRect& MyCullingRect,
		FSlateWindowElementList& OutDrawElements,
		int32 LayerId,
		const FWidgetStyle& InWidgetStyle,
		bool bParentEnabled) const override
	{
		const int32 ResultLayer = SCompoundWidget::OnPaint(Args, AllottedGeometry, MyCullingRect, OutDrawElements, LayerId, InWidgetStyle, bParentEnabled);
		const TSharedPtr<FWoWGlueFrame> PinnedFrame = Frame.Pin();
		if (!PinnedFrame.IsValid() || PinnedFrame->GetVerticalScrollRange() <= KINDA_SMALL_NUMBER)
		{
			return ResultLayer;
		}

		const FVector2D LocalSize = AllottedGeometry.GetLocalSize();
		const FWoWGlueScrollBarStyle& Style = PinnedFrame->GetScrollBarStyle();
		const float TrackWidth = FMath::Max(1.0f, Style.Width);
		const float TrackPadding = FMath::Max(0.0f, Style.TrackInset);
		const float TrackX = FMath::Max(0.0f, LocalSize.X - TrackWidth);
		const float ButtonHeight = FMath::Clamp(Style.ButtonHeight, 0.0f, LocalSize.Y * 0.45f);
		const float TrackTop = ButtonHeight;
		const float TrackHeight = FMath::Max(1.0f, LocalSize.Y - ButtonHeight * 2.0f);
		const float Range = PinnedFrame->GetVerticalScrollRange();
		const float ContentHeight = TrackHeight + Range;
		const float ThumbHeight = FMath::Clamp((TrackHeight / ContentHeight) * TrackHeight, FMath::Max(1.0f, Style.ThumbMinHeight), TrackHeight);
		const float ThumbTravel = FMath::Max(0.0f, TrackHeight - ThumbHeight);
		const float ThumbY = TrackTop + (Range > KINDA_SMALL_NUMBER
			? (PinnedFrame->GetVerticalScroll() / Range) * ThumbTravel
			: 0.0f);
		const float ThumbWidth = FMath::Max(1.0f, Style.ThumbWidth);
		const float ThumbX = TrackX + TrackPadding + (TrackWidth - TrackPadding * 2.0f - ThumbWidth) * 0.5f;
		const bool bCanScrollUp = CanScrollUp();
		const bool bCanScrollDown = CanScrollDown();
		const FWoWGlueScrollBarVisualState& ThumbVisualState = bDraggingThumb
			? Style.Pushed
			: (HoveredPart == EScrollPart::Thumb ? Style.Highlight : Style.Normal);
		const FWoWGlueScrollBarVisualState& UpArrowVisualState = (PressedPart == EScrollPart::UpArrow && bCanScrollUp)
			? Style.Pushed
			: ((HoveredPart == EScrollPart::UpArrow && bCanScrollUp) ? Style.Highlight : Style.Normal);
		const FWoWGlueScrollBarVisualState& DownArrowVisualState = (PressedPart == EScrollPart::DownArrow && bCanScrollDown)
			? Style.Pushed
			: ((HoveredPart == EScrollPart::DownArrow && bCanScrollDown) ? Style.Highlight : Style.Normal);
		const int32 ScrollLayer = ResultLayer + 1;

		if (Style.TrackBackgroundAlpha > 0.0f)
		{
			const float BackgroundWidth = FMath::Max(1.0f, Style.TrackBackgroundWidth > 0.0f ? Style.TrackBackgroundWidth : ThumbWidth);
			const float BackgroundX = ThumbX + (ThumbWidth - BackgroundWidth) * 0.5f;
			FSlateDrawElement::MakeBox(
				OutDrawElements,
				ScrollLayer,
				AllottedGeometry.ToPaintGeometry(FVector2D(BackgroundWidth, TrackHeight), FSlateLayoutTransform(FVector2D(BackgroundX, TrackTop))),
				FCoreStyle::Get().GetBrush("WhiteBrush"),
				ESlateDrawEffect::None,
				FLinearColor(0.0f, 0.0f, 0.0f, FMath::Clamp(Style.TrackBackgroundAlpha, 0.0f, 1.0f)));
		}

		DrawArrow(
			AllottedGeometry,
			OutDrawElements,
			ScrollLayer + 1,
			FVector2D(TrackX, 0.0f),
			FVector2D(TrackWidth, ButtonHeight),
			Style.ArrowTexture,
			UpArrowVisualState.UpArrow);

		DrawArrow(
			AllottedGeometry,
			OutDrawElements,
			ScrollLayer + 1,
			FVector2D(TrackX, FMath::Max(0.0f, LocalSize.Y - ButtonHeight)),
			FVector2D(TrackWidth, ButtonHeight),
			Style.ArrowTexture,
			DownArrowVisualState.DownArrow);

		DrawThumb(
			AllottedGeometry,
			OutDrawElements,
			ScrollLayer + 2,
			FVector2D(ThumbX, ThumbY),
			FVector2D(ThumbWidth, ThumbHeight),
			Style,
			ThumbVisualState);

		return ScrollLayer + 3;
	}

	virtual FReply OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override
	{
		if (MouseEvent.GetEffectingButton() != EKeys::LeftMouseButton)
		{
			return FReply::Unhandled();
		}

		if (const TSharedPtr<FWoWGlueFrame> PinnedFrame = Frame.Pin())
		{
			if (PinnedFrame->GetVerticalScrollRange() <= KINDA_SMALL_NUMBER)
			{
				return FReply::Unhandled();
			}

			const FVector2D LocalPosition = MyGeometry.AbsoluteToLocal(MouseEvent.GetScreenSpacePosition());
			const FVector2D LocalSize = MyGeometry.GetLocalSize();
			const EScrollPart HitPart = HitTestScrollPart(LocalSize, LocalPosition);
			if (HitPart == EScrollPart::UpArrow)
			{
				if (!CanScrollUp())
				{
					return FReply::Handled();
				}

				PressedPart = EScrollPart::UpArrow;
				ScrollByStep(-1.0f);
				Invalidate(EInvalidateWidgetReason::Paint);
				return FReply::Handled().CaptureMouse(SharedThis(this));
			}
			if (HitPart == EScrollPart::DownArrow)
			{
				if (!CanScrollDown())
				{
					return FReply::Handled();
				}

				PressedPart = EScrollPart::DownArrow;
				ScrollByStep(1.0f);
				Invalidate(EInvalidateWidgetReason::Paint);
				return FReply::Handled().CaptureMouse(SharedThis(this));
			}
			if (HitPart == EScrollPart::Thumb)
			{
				PressedPart = EScrollPart::Thumb;
				bDraggingThumb = true;
				DragGrabOffset = LocalPosition.Y - GetThumbTop(LocalSize);
				UpdateScrollFromLocalY(LocalSize, LocalPosition.Y - DragGrabOffset);
				Invalidate(EInvalidateWidgetReason::Paint);
				return FReply::Handled().CaptureMouse(SharedThis(this));
			}
		}

		return FReply::Unhandled();
	}

	virtual FReply OnMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override
	{
		if (bDraggingThumb && MouseEvent.GetEffectingButton() == EKeys::LeftMouseButton)
		{
			bDraggingThumb = false;
			PressedPart = EScrollPart::None;
			Invalidate(EInvalidateWidgetReason::Paint);
			return FReply::Handled().ReleaseMouseCapture();
		}
		if (PressedPart != EScrollPart::None && MouseEvent.GetEffectingButton() == EKeys::LeftMouseButton)
		{
			PressedPart = EScrollPart::None;
			Invalidate(EInvalidateWidgetReason::Paint);
			return FReply::Handled().ReleaseMouseCapture();
		}
		return FReply::Unhandled();
	}

	virtual void OnMouseLeave(const FPointerEvent& MouseEvent) override
	{
		SCompoundWidget::OnMouseLeave(MouseEvent);
		if (!bDraggingThumb)
		{
			HoveredPart = EScrollPart::None;
			Invalidate(EInvalidateWidgetReason::Paint);
		}
	}

	virtual FReply OnMouseMove(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override
	{
		if (const TSharedPtr<FWoWGlueFrame> PinnedFrame = Frame.Pin())
		{
			const FVector2D LocalPosition = MyGeometry.AbsoluteToLocal(MouseEvent.GetScreenSpacePosition());
			const EScrollPart NewHoveredPart = HitTestScrollPart(MyGeometry.GetLocalSize(), LocalPosition);
			if (HoveredPart != NewHoveredPart)
			{
				HoveredPart = NewHoveredPart;
				Invalidate(EInvalidateWidgetReason::Paint);
			}
		}

		if (!bDraggingThumb)
		{
			return FReply::Unhandled();
		}

		const FVector2D LocalPosition = MyGeometry.AbsoluteToLocal(MouseEvent.GetScreenSpacePosition());
		UpdateScrollFromLocalY(MyGeometry.GetLocalSize(), LocalPosition.Y - DragGrabOffset);
		return FReply::Handled();
	}

private:
	EScrollPart HitTestScrollPart(const FVector2D& LocalSize, const FVector2D& LocalPosition) const
	{
		const TSharedPtr<FWoWGlueFrame> PinnedFrame = Frame.Pin();
		if (!PinnedFrame.IsValid() || PinnedFrame->GetVerticalScrollRange() <= KINDA_SMALL_NUMBER)
		{
			return EScrollPart::None;
		}

		const FWoWGlueScrollBarStyle& Style = PinnedFrame->GetScrollBarStyle();
		const float TrackWidth = FMath::Max(1.0f, Style.Width);
		const float TrackPadding = FMath::Max(0.0f, Style.TrackInset);
		const float TrackX = FMath::Max(0.0f, LocalSize.X - TrackWidth);
		if (LocalPosition.X < TrackX || LocalPosition.X > TrackX + TrackWidth)
		{
			return EScrollPart::None;
		}

		const float ButtonHeight = FMath::Clamp(Style.ButtonHeight, 0.0f, LocalSize.Y * 0.45f);
		if (ButtonHeight > 0.0f && LocalPosition.Y >= 0.0f && LocalPosition.Y <= ButtonHeight)
		{
			return EScrollPart::UpArrow;
		}
		if (ButtonHeight > 0.0f && LocalPosition.Y >= LocalSize.Y - ButtonHeight && LocalPosition.Y <= LocalSize.Y)
		{
			return EScrollPart::DownArrow;
		}

		const float TrackTop = ButtonHeight;
		const float TrackHeight = FMath::Max(1.0f, LocalSize.Y - ButtonHeight * 2.0f);
		const float ThumbHeight = GetThumbHeight(LocalSize);
		const float ThumbWidth = FMath::Max(1.0f, Style.ThumbWidth);
		const float ThumbX = TrackX + TrackPadding + (TrackWidth - TrackPadding * 2.0f - ThumbWidth) * 0.5f;
		const float ThumbY = FMath::Clamp(GetThumbTop(LocalSize), TrackTop, TrackTop + FMath::Max(0.0f, TrackHeight - ThumbHeight));

		if (
			LocalPosition.X >= ThumbX &&
			LocalPosition.X <= ThumbX + ThumbWidth &&
			LocalPosition.Y >= ThumbY &&
			LocalPosition.Y <= ThumbY + ThumbHeight)
		{
			return EScrollPart::Thumb;
		}

		if (LocalPosition.Y >= TrackTop && LocalPosition.Y <= TrackTop + TrackHeight)
		{
			return EScrollPart::Track;
		}

		return EScrollPart::None;
	}

	bool CanScrollUp() const
	{
		const TSharedPtr<FWoWGlueFrame> PinnedFrame = Frame.Pin();
		return PinnedFrame.IsValid() && PinnedFrame->GetVerticalScroll() > KINDA_SMALL_NUMBER;
	}

	bool CanScrollDown() const
	{
		const TSharedPtr<FWoWGlueFrame> PinnedFrame = Frame.Pin();
		return PinnedFrame.IsValid() && PinnedFrame->GetVerticalScroll() < PinnedFrame->GetVerticalScrollRange() - KINDA_SMALL_NUMBER;
	}

	void ScrollByStep(float Direction)
	{
		if (const TSharedPtr<FWoWGlueFrame> PinnedFrame = Frame.Pin())
		{
			const float Step = FMath::Max(1.0f, PinnedFrame->GetScrollBarStyle().Step);
			PinnedFrame->SetVerticalScroll(PinnedFrame->GetVerticalScroll() + Direction * Step);
		}
	}

	void DrawArrow(
		const FGeometry& AllottedGeometry,
		FSlateWindowElementList& OutDrawElements,
		int32 LayerId,
		const FVector2D& Position,
		const FVector2D& Size,
		UTexture2D* Texture,
		const FWoWGlueUVRect& UVRect) const
	{
		if (Size.X <= 0.0f || Size.Y <= 0.0f)
		{
			return;
		}

		if (Texture && UVRect.bValid)
		{
			TSharedPtr<FSlateBrush> Brush = MakeTextureRegionBrush(Texture, Size, ToSlateUVRegion(UVRect));
			FSlateDrawElement::MakeBox(
				OutDrawElements,
				LayerId,
				AllottedGeometry.ToPaintGeometry(Size, FSlateLayoutTransform(Position)),
				Brush.Get(),
				ESlateDrawEffect::None,
				FLinearColor::White);
			return;
		}

		FSlateDrawElement::MakeBox(
			OutDrawElements,
			LayerId,
			AllottedGeometry.ToPaintGeometry(Size, FSlateLayoutTransform(Position)),
			FCoreStyle::Get().GetBrush("WhiteBrush"),
			ESlateDrawEffect::None,
			FLinearColor(0.13f, 0.11f, 0.06f, 0.65f));
	}

	void DrawThumb(
		const FGeometry& AllottedGeometry,
		FSlateWindowElementList& OutDrawElements,
		int32 LayerId,
		const FVector2D& Position,
		const FVector2D& Size,
		const FWoWGlueScrollBarStyle& Style,
		const FWoWGlueScrollBarVisualState& VisualState) const
	{
		if (Style.ThumbTopBottomTexture && Style.ThumbMiddleTexture && VisualState.ThumbTop.bValid && VisualState.ThumbMiddle.bValid && VisualState.ThumbBottom.bValid)
		{
			const float CapHeight = FMath::Clamp(Style.ThumbCapHeight, 0.0f, Size.Y * 0.5f);
			const float MiddleHeight = FMath::Max(0.0f, Size.Y - CapHeight * 2.0f);

			TSharedPtr<FSlateBrush> TopBrush = MakeTextureRegionBrush(Style.ThumbTopBottomTexture, FVector2D(Size.X, CapHeight), ToSlateUVRegion(VisualState.ThumbTop));
			TSharedPtr<FSlateBrush> MiddleBrush = MakeTextureRegionBrush(Style.ThumbMiddleTexture, FVector2D(Size.X, MiddleHeight), ToSlateUVRegion(VisualState.ThumbMiddle));
			TSharedPtr<FSlateBrush> BottomBrush = MakeTextureRegionBrush(Style.ThumbTopBottomTexture, FVector2D(Size.X, CapHeight), ToSlateUVRegion(VisualState.ThumbBottom));

			if (CapHeight > 0.0f)
			{
				FSlateDrawElement::MakeBox(
					OutDrawElements,
					LayerId,
					AllottedGeometry.ToPaintGeometry(FVector2D(Size.X, CapHeight), FSlateLayoutTransform(Position)),
					TopBrush.Get(),
					ESlateDrawEffect::None,
					FLinearColor::White);
			}
			if (MiddleHeight > 0.0f)
			{
				FSlateDrawElement::MakeBox(
					OutDrawElements,
					LayerId,
					AllottedGeometry.ToPaintGeometry(FVector2D(Size.X, MiddleHeight), FSlateLayoutTransform(Position + FVector2D(0.0f, CapHeight))),
					MiddleBrush.Get(),
					ESlateDrawEffect::None,
					FLinearColor::White);
			}
			if (CapHeight > 0.0f)
			{
				FSlateDrawElement::MakeBox(
					OutDrawElements,
					LayerId,
					AllottedGeometry.ToPaintGeometry(FVector2D(Size.X, CapHeight), FSlateLayoutTransform(Position + FVector2D(0.0f, CapHeight + MiddleHeight))),
					BottomBrush.Get(),
					ESlateDrawEffect::None,
					FLinearColor::White);
			}
			return;
		}

		FSlateDrawElement::MakeBox(
			OutDrawElements,
			LayerId,
			AllottedGeometry.ToPaintGeometry(Size, FSlateLayoutTransform(Position)),
			FCoreStyle::Get().GetBrush("WhiteBrush"),
			ESlateDrawEffect::None,
			bDraggingThumb ? FLinearColor(0.95f, 0.82f, 0.25f, 0.95f) : FLinearColor(0.78f, 0.64f, 0.22f, 0.88f));
	}

	FOptionalSize GetViewportWidth() const
	{
		const TSharedPtr<FWoWGlueFrame> PinnedFrame = Frame.Pin();
		return PinnedFrame.IsValid() ? FOptionalSize(PinnedFrame->GetSize().X) : FOptionalSize();
	}

	FOptionalSize GetViewportHeight() const
	{
		const TSharedPtr<FWoWGlueFrame> PinnedFrame = Frame.Pin();
		return PinnedFrame.IsValid() ? FOptionalSize(PinnedFrame->GetSize().Y) : FOptionalSize();
	}

	FMargin GetScrollChildOffset() const
	{
		const TSharedPtr<FWoWGlueFrame> PinnedFrame = Frame.Pin();
		const TSharedPtr<FWoWGlueFrame> PinnedScrollChild = ScrollChild.Pin();
		if (!PinnedFrame.IsValid() || !PinnedScrollChild.IsValid())
		{
			return FMargin(0.0f, 0.0f, 1.0f, 1.0f);
		}

		return FMargin(0.0f, -PinnedFrame->GetVerticalScroll(), PinnedScrollChild->GetSize().X, PinnedScrollChild->MeasureDesiredContentSize().Y);
	}

	float GetThumbHeight(const FVector2D& LocalSize) const
	{
		const TSharedPtr<FWoWGlueFrame> PinnedFrame = Frame.Pin();
		if (!PinnedFrame.IsValid())
		{
			return 0.0f;
		}

		const float TrackHeight = FMath::Max(1.0f, LocalSize.Y);
		const float ButtonHeight = FMath::Clamp(PinnedFrame->GetScrollBarStyle().ButtonHeight, 0.0f, TrackHeight * 0.45f);
		const float ScrollTrackHeight = FMath::Max(1.0f, TrackHeight - ButtonHeight * 2.0f);
		const float ContentHeight = TrackHeight + PinnedFrame->GetVerticalScrollRange();
		return FMath::Clamp((ScrollTrackHeight / ContentHeight) * ScrollTrackHeight, FMath::Max(1.0f, PinnedFrame->GetScrollBarStyle().ThumbMinHeight), ScrollTrackHeight);
	}

	float GetThumbTop(const FVector2D& LocalSize) const
	{
		const TSharedPtr<FWoWGlueFrame> PinnedFrame = Frame.Pin();
		if (!PinnedFrame.IsValid())
		{
			return 0.0f;
		}

		const float ButtonHeight = FMath::Clamp(PinnedFrame->GetScrollBarStyle().ButtonHeight, 0.0f, LocalSize.Y * 0.45f);
		const float ThumbTravel = FMath::Max(0.0f, LocalSize.Y - ButtonHeight * 2.0f - GetThumbHeight(LocalSize));
		const float Range = PinnedFrame->GetVerticalScrollRange();
		return ButtonHeight + (Range > KINDA_SMALL_NUMBER ? (PinnedFrame->GetVerticalScroll() / Range) * ThumbTravel : 0.0f);
	}

	void UpdateScrollFromLocalY(const FVector2D& LocalSize, float ThumbTop)
	{
		if (const TSharedPtr<FWoWGlueFrame> PinnedFrame = Frame.Pin())
		{
			const float ButtonHeight = FMath::Clamp(PinnedFrame->GetScrollBarStyle().ButtonHeight, 0.0f, LocalSize.Y * 0.45f);
			const float ThumbTravel = FMath::Max(0.0f, LocalSize.Y - ButtonHeight * 2.0f - GetThumbHeight(LocalSize));
			const float ScrollRange = PinnedFrame->GetVerticalScrollRange();
			const float ScrollAlpha = ThumbTravel > KINDA_SMALL_NUMBER ? FMath::Clamp((ThumbTop - ButtonHeight) / ThumbTravel, 0.0f, 1.0f) : 0.0f;
			PinnedFrame->SetVerticalScroll(ScrollAlpha * ScrollRange);
		}
	}

	TWeakPtr<FWoWGlueFrame> Frame;
	TWeakPtr<FWoWGlueFrame> ScrollChild;
	TSharedPtr<SConstraintCanvas> Canvas;
	mutable bool bDraggingThumb = false;
	mutable EScrollPart HoveredPart = EScrollPart::None;
	mutable EScrollPart PressedPart = EScrollPart::None;
	float DragGrabOffset = 0.0f;
};
}

int32 FWoWGlueFrame::NextRaiseOrder = 0;

FWoWGlueFrame::FWoWGlueFrame(EWoWGlueFrameType InFrameType, FString InTypeName, FName InName, TSharedPtr<FWoWGlueFrame> InParent)
	: FrameType(InFrameType)
	, TypeName(MoveTemp(InTypeName))
	, Name(InName)
	, Parent(InParent)
{
	if (FrameType == EWoWGlueFrameType::EditBox)
	{
		JustifyH = TEXT("LEFT");
	}
}

FWoWGlueFrame& FWoWGlueFrame::SetPoint(const FString& InPoint, TSharedPtr<FWoWGlueFrame> InRelativeTo, const FString& InRelativePoint, float XOffset, float YOffset)
{
	bSetAllPoints = false;
	Point.Point = NormalizeGluePoint(InPoint);
	Point.RelativeTo = InRelativeTo.IsValid() ? InRelativeTo->GetName().ToString() : TEXT("UIParent");
	Point.RelativePoint = NormalizeGluePoint(InRelativePoint);
	Point.Offset = FVector2D(XOffset, YOffset);
	return *this;
}

FWoWGlueFrame& FWoWGlueFrame::SetSize(float Width, float Height)
{
	Size = FVector2D(FMath::Max(1.0f, Width), FMath::Max(1.0f, Height));
	bHasExplicitSize = true;
	return *this;
}

FWoWGlueFrame& FWoWGlueFrame::SetAllPoints(bool bInSetAllPoints)
{
	bSetAllPoints = bInSetAllPoints;
	if (bSetAllPoints)
	{
		TSharedPtr<FWoWGlueFrame> ParentFrame = Parent.Pin();
		Point.Point = TEXT("CENTER");
		Point.RelativeTo = ParentFrame.IsValid() ? ParentFrame->GetName().ToString() : TEXT("UIParent");
		Point.RelativePoint = TEXT("CENTER");
		Point.Offset = FVector2D::ZeroVector;
	}
	return *this;
}

FWoWGlueFrame& FWoWGlueFrame::SetText(const FText& InText)
{
	Text = InText;
	return *this;
}

FWoWGlueFrame& FWoWGlueFrame::SetText(const FString& InText)
{
	Text = FText::FromString(InText);
	return *this;
}

FString FWoWGlueFrame::GetTextString() const
{
	return Text.ToString();
}

FWoWGlueFrame& FWoWGlueFrame::SetAlpha(float InAlpha)
{
	Alpha = FMath::Clamp(InAlpha, 0.0f, 1.0f);
	return *this;
}

FWoWGlueFrame& FWoWGlueFrame::SetVertexColor(float R, float G, float B, float A)
{
	bHasExplicitVertexColor = true;
	VertexColor = FLinearColor(
		FMath::Clamp(R, 0.0f, 1.0f),
		FMath::Clamp(G, 0.0f, 1.0f),
		FMath::Clamp(B, 0.0f, 1.0f),
		FMath::Clamp(A, 0.0f, 1.0f));
	return *this;
}

FWoWGlueFrame& FWoWGlueFrame::SetPassword(bool bInPassword)
{
	bPassword = bInPassword;
	return *this;
}

void FWoWGlueFrame::SetKeyboardFocus()
{
	SetKeyboardFocus(true);
}

void FWoWGlueFrame::SetKeyboardFocus(bool bSelectAllText)
{
	if (FrameType != EWoWGlueFrameType::EditBox || !bShown || !bEnabled)
	{
		return;
	}
	bPendingKeyboardFocus = true;
	TSharedPtr<SEditableTextBox> TextBox = EditableTextBoxWidget.Pin();
	if (TextBox.IsValid() && FSlateApplication::IsInitialized())
	{
		if (TSharedPtr<FWoWGlueUIManager> Manager = OwnerManager.Pin())
		{
			Manager->SetActiveEditBox(AsShared());
		}
		FSlateApplication& SlateApplication = FSlateApplication::Get();
		SlateApplication.SetUserFocus(SlateApplication.GetUserIndexForKeyboard(), TextBox, EFocusCause::SetDirectly);
		SlateApplication.SetKeyboardFocus(TextBox, EFocusCause::SetDirectly);
		if (bSelectAllText)
		{
			TextBox->SelectAllText();
		}
		bPendingKeyboardFocus = false;
	}
}

FWoWGlueFrame& FWoWGlueFrame::SetKeepKeyboardFocus(bool bInKeepKeyboardFocus)
{
	bKeepKeyboardFocus = bInKeepKeyboardFocus;
	return *this;
}

void FWoWGlueFrame::SetOwnerManager(TSharedPtr<FWoWGlueUIManager> InOwnerManager)
{
	OwnerManager = InOwnerManager;
}

FWoWGlueFrame& FWoWGlueFrame::SetPlaceholderText(const FString& InPlaceholderText)
{
	PlaceholderText = FText::FromString(InPlaceholderText);
	return *this;
}

FWoWGlueFrame& FWoWGlueFrame::SetPlaceholderTextColor(float R, float G, float B, float A)
{
	PlaceholderTextColor = FLinearColor(
		FMath::Clamp(R, 0.0f, 1.0f),
		FMath::Clamp(G, 0.0f, 1.0f),
		FMath::Clamp(B, 0.0f, 1.0f),
		FMath::Clamp(A, 0.0f, 1.0f));
	return *this;
}

FWoWGlueFrame& FWoWGlueFrame::SetPlaceholderJustifyH(const FString& InJustifyH)
{
	PlaceholderJustifyH = InJustifyH;
	PlaceholderJustifyH.TrimStartAndEndInline();
	PlaceholderJustifyH.ToUpperInline();
	if (PlaceholderJustifyH.IsEmpty())
	{
		PlaceholderJustifyH = TEXT("LEFT");
	}
	return *this;
}

FWoWGlueFrame& FWoWGlueFrame::SetTextInsets(float Left, float Right, float Top, float Bottom)
{
	TextInsets = FMargin(Left, Top, Right, Bottom);
	return *this;
}

FWoWGlueFrame& FWoWGlueFrame::SetTextColor(float R, float G, float B, float A)
{
	TextColor = FLinearColor(
		FMath::Clamp(R, 0.0f, 1.0f),
		FMath::Clamp(G, 0.0f, 1.0f),
		FMath::Clamp(B, 0.0f, 1.0f),
		FMath::Clamp(A, 0.0f, 1.0f));
	TextGradientMode = EWoWGlueTextGradientMode::None;
	return *this;
}

FWoWGlueFrame& FWoWGlueFrame::SetFont(const FString& InFontPath, float InFontSize, const FString& Flags)
{
	FontPath = InFontPath;
	FontSize = FMath::Clamp(InFontSize, 1.0f, 72.0f);
	FontFlags = Flags;
	return *this;
}

FWoWGlueFrame& FWoWGlueFrame::SetFontObject(const FString& FontObjectName)
{
	FString Normalized = FontObjectName;
	Normalized.TrimStartAndEndInline();
	Normalized.ToUpperInline();

	float PresetSize = 14.0f;
	FLinearColor PresetColor = GlueTextColor();
	FString PresetFlags;

	if (Normalized.Contains(TEXT("EXTRASMALL")))
	{
		PresetSize = 10.0f;
	}
	else if (Normalized.Contains(TEXT("SMALL")))
	{
		PresetSize = 12.0f;
	}
	else if (Normalized.Contains(TEXT("HUGE")))
	{
		PresetSize = 24.0f;
	}
	else if (Normalized.Contains(TEXT("LARGE")))
	{
		PresetSize = 16.0f;
	}

	if (Normalized.Contains(TEXT("HIGHLIGHT")))
	{
		PresetColor = FLinearColor(1.0f, 0.82f, 0.0f, 1.0f);
	}
	else if (Normalized.Contains(TEXT("DISABLE")) || Normalized.Contains(TEXT("DISABLED")) || Normalized.Contains(TEXT("GRAY")))
	{
		PresetColor = FLinearColor(0.5f, 0.5f, 0.5f, 1.0f);
	}
	else if (Normalized.Contains(TEXT("RED")))
	{
		PresetColor = FLinearColor(1.0f, 0.1f, 0.1f, 1.0f);
	}
	else if (Normalized.Contains(TEXT("WHITE")))
	{
		PresetColor = FLinearColor::White;
	}

	if (Normalized.Contains(TEXT("OUTLINE")))
	{
		PresetFlags = TEXT("OUTLINE");
	}

	SetFont(FontPath, PresetSize, PresetFlags);
	SetTextColor(PresetColor.R, PresetColor.G, PresetColor.B, PresetColor.A);
	return *this;
}

FWoWGlueFrame& FWoWGlueFrame::SetShadowColor(float R, float G, float B, float A)
{
	ShadowColor = FLinearColor(
		FMath::Clamp(R, 0.0f, 1.0f),
		FMath::Clamp(G, 0.0f, 1.0f),
		FMath::Clamp(B, 0.0f, 1.0f),
		FMath::Clamp(A, 0.0f, 1.0f));
	return *this;
}

FWoWGlueFrame& FWoWGlueFrame::SetShadowOffset(float XOffset, float YOffset)
{
	ShadowOffset = FVector2D(XOffset, YOffset);
	return *this;
}

FWoWGlueFrame& FWoWGlueFrame::SetJustifyH(const FString& InJustifyH)
{
	JustifyH = InJustifyH;
	JustifyH.TrimStartAndEndInline();
	JustifyH.ToUpperInline();
	if (JustifyH.IsEmpty())
	{
		JustifyH = TEXT("CENTER");
	}
	return *this;
}

FWoWGlueFrame& FWoWGlueFrame::SetJustifyV(const FString& InJustifyV)
{
	JustifyV = InJustifyV;
	JustifyV.TrimStartAndEndInline();
	JustifyV.ToUpperInline();
	if (JustifyV.IsEmpty())
	{
		JustifyV = TEXT("MIDDLE");
	}
	return *this;
}

FWoWGlueFrame& FWoWGlueFrame::SetTextFirstLineIndent(float InFirstLineIndent)
{
	TextFirstLineIndent = FMath::Max(0.0f, InFirstLineIndent);
	return *this;
}

FWoWGlueFrame& FWoWGlueFrame::SetTextFirstLineIndentChars(float InFirstLineIndentChars)
{
	TextFirstLineIndent = FMath::Max(0.0f, InFirstLineIndentChars) * FontSize;
	return *this;
}

FWoWGlueFrame& FWoWGlueFrame::SetTextParagraphSpacing(float InParagraphSpacing)
{
	TextParagraphSpacing = FMath::Max(0.0f, InParagraphSpacing);
	return *this;
}

FWoWGlueFrame& FWoWGlueFrame::SetTextLineHeightPercentage(float InLineHeightPercentage)
{
	TextLineHeightPercentage = FMath::Clamp(InLineHeightPercentage, 0.5f, 3.0f);
	return *this;
}

FWoWGlueFrame& FWoWGlueFrame::SetTextJustifyLastLine(bool bInJustifyLastLine)
{
	bTextJustifyLastLine = bInJustifyLastLine;
	return *this;
}

FWoWGlueFrame& FWoWGlueFrame::SetSimpleHTMLLineSpacing(float InLineSpacing)
{
	SimpleHTMLLineSpacing = FMath::Max(0.0f, InLineSpacing);
	return *this;
}

FWoWGlueFrame& FWoWGlueFrame::SetTextGradient(const FString& Direction, const FLinearColor& StartColor, const FLinearColor& EndColor)
{
	FString NormalizedDirection = Direction;
	NormalizedDirection.TrimStartAndEndInline();
	NormalizedDirection.ToUpperInline();
	TextGradientMode = NormalizedDirection == TEXT("HORIZONTAL")
		? EWoWGlueTextGradientMode::Horizontal
		: EWoWGlueTextGradientMode::Vertical;
	TextGradientStart = StartColor;
	TextGradientEnd = EndColor;
	TextColor = StartColor;
	return *this;
}

FWoWGlueFrame& FWoWGlueFrame::ClearAllPoints()
{
	Point = FWoWGluePoint();
	bSetAllPoints = false;
	return *this;
}

FWoWGlueFrame& FWoWGlueFrame::Raise()
{
	RaiseOrder = ++NextRaiseOrder;
	return *this;
}

FWoWGlueFrame& FWoWGlueFrame::SetScript(const FString& ScriptName, FScriptHandler Handler)
{
	if (ScriptName.Equals(TEXT("OnClick"), ESearchCase::IgnoreCase))
	{
		OnClickScript = MoveTemp(Handler);
	}
	else if (ScriptName.Equals(TEXT("OnDoubleClick"), ESearchCase::IgnoreCase))
	{
		OnDoubleClickScript = MoveTemp(Handler);
	}
	else if (ScriptName.Equals(TEXT("OnTextChanged"), ESearchCase::IgnoreCase))
	{
		OnTextChangedScript = MoveTemp(Handler);
	}
	else if (ScriptName.Equals(TEXT("OnEditFocusGained"), ESearchCase::IgnoreCase))
	{
		OnEditFocusGainedScript = MoveTemp(Handler);
	}
	else if (ScriptName.Equals(TEXT("OnEditFocusLost"), ESearchCase::IgnoreCase))
	{
		OnEditFocusLostScript = MoveTemp(Handler);
	}
	else if (ScriptName.Equals(TEXT("OnTabPressed"), ESearchCase::IgnoreCase))
	{
		OnTabPressedScript = MoveTemp(Handler);
	}
	else if (ScriptName.Equals(TEXT("OnEnterPressed"), ESearchCase::IgnoreCase))
	{
		OnEnterPressedScript = MoveTemp(Handler);
	}
	else if (ScriptName.Equals(TEXT("OnEscapePressed"), ESearchCase::IgnoreCase))
	{
		OnEscapePressedScript = MoveTemp(Handler);
	}
	else if (ScriptName.Equals(TEXT("OnEnter"), ESearchCase::IgnoreCase))
	{
		OnEnterScript = MoveTemp(Handler);
	}
	else if (ScriptName.Equals(TEXT("OnLeave"), ESearchCase::IgnoreCase))
	{
		OnLeaveScript = MoveTemp(Handler);
	}
	return *this;
}

FWoWGlueFrame& FWoWGlueFrame::SetHyperlinkScript(FHyperlinkScriptHandler Handler)
{
	OnHyperlinkClickScript = MoveTemp(Handler);
	return *this;
}

FWoWGlueFrame& FWoWGlueFrame::SetScrollChild(TSharedPtr<FWoWGlueFrame> InScrollChild)
{
	ScrollChild = InScrollChild;
	VerticalScroll = 0.0f;
	return *this;
}

FWoWGlueFrame& FWoWGlueFrame::SetVerticalScroll(float InVerticalScroll)
{
	VerticalScroll = FMath::Clamp(InVerticalScroll, 0.0f, GetVerticalScrollRange());
	return *this;
}

FWoWGlueFrame& FWoWGlueFrame::SetScrollBarStyle(const FWoWGlueScrollBarStyle& InScrollBarStyle)
{
	ScrollBarStyle = InScrollBarStyle;
	return *this;
}

FWoWGlueFrame& FWoWGlueFrame::Show()
{
	bShown = true;
	return *this;
}

FWoWGlueFrame& FWoWGlueFrame::Hide()
{
	bShown = false;
	return *this;
}

FWoWGlueFrame& FWoWGlueFrame::SetEnabled(bool bInEnabled)
{
	bEnabled = bInEnabled;
	if (!bEnabled)
	{
		HandleMouseLeave();
	}
	return *this;
}

FWoWGlueFrame& FWoWGlueFrame::SetMouseEnabled(bool bInMouseEnabled)
{
	bMouseEnabled = bInMouseEnabled;
	return *this;
}

FWoWGlueFrame& FWoWGlueFrame::SetDrawLayer(int32 InDrawLayer)
{
	DrawLayer = InDrawLayer;
	return *this;
}

FWoWGlueFrame& FWoWGlueFrame::SetTexture(const FString& InTexturePath)
{
	MainTexturePath = InTexturePath;
	return *this;
}

FWoWGlueFrame& FWoWGlueFrame::SetTexture(UTexture2D* InTexture, const FString& InTexturePath, EWoWGlueAlphaMode AlphaMode)
{
	MainTexture = InTexture;
	MainTexturePath = InTexturePath;
	MainTextureAlphaMode = AlphaMode;
	return *this;
}

FWoWGlueFrame& FWoWGlueFrame::SetTexCoord(float Left, float Right, float Top, float Bottom)
{
	// 对齐 WoW XML/Lua 的 TexCoords/SetTexCoord 语义：同一张贴图只显示指定 UV 区域。
	MainTextureUVRect.Left = Left;
	MainTextureUVRect.Right = Right;
	MainTextureUVRect.Top = Top;
	MainTextureUVRect.Bottom = Bottom;
	MainTextureUVRect.bValid = true;
	return *this;
}

FWoWGlueFrame& FWoWGlueFrame::SetNormalTexture(const FString& TexturePath)
{
	NormalTexturePath = TexturePath;
	return *this;
}

FWoWGlueFrame& FWoWGlueFrame::SetPushedTexture(const FString& TexturePath)
{
	PushedTexturePath = TexturePath;
	return *this;
}

FWoWGlueFrame& FWoWGlueFrame::SetHighlightTexture(const FString& TexturePath)
{
	HighlightTexturePath = TexturePath;
	return *this;
}

FWoWGlueFrame& FWoWGlueFrame::SetNormalTexture(UTexture2D* Texture, const FString& TexturePath)
{
	NormalTexture = Texture;
	NormalTexturePath = TexturePath;
	return *this;
}

FWoWGlueFrame& FWoWGlueFrame::SetPushedTexture(UTexture2D* Texture, const FString& TexturePath)
{
	PushedTexture = Texture;
	PushedTexturePath = TexturePath;
	return *this;
}

FWoWGlueFrame& FWoWGlueFrame::SetHighlightTexture(UTexture2D* Texture, const FString& TexturePath, EWoWGlueAlphaMode AlphaMode)
{
	HighlightTexture = Texture;
	HighlightTexturePath = TexturePath;
	HighlightAlphaMode = AlphaMode;
	return *this;
}

FWoWGlueFrame& FWoWGlueFrame::SetBackdrop(UTexture2D* BackgroundTexture, const FString& BackgroundPath, UTexture2D* EdgeTexture, const FString& EdgePath, float EdgeSize, const FMargin& Insets)
{
	BackdropBackgroundTexture = BackgroundTexture;
	BackdropBackgroundPath = BackgroundPath;
	BackdropEdgeTexture = EdgeTexture;
	BackdropEdgePath = EdgePath;
	BackdropEdgeSize = FMath::Max(1.0f, EdgeSize);
	BackdropInsets = Insets;
	return *this;
}

void FWoWGlueFrame::AddChild(TSharedRef<FWoWGlueFrame> Child)
{
	Children.Add(Child);
}

void FWoWGlueFrame::CollectSubtreeFrames(TSharedRef<FWoWGlueFrame> Frame, TArray<TSharedRef<FWoWGlueFrame>>& Frames, TMap<FString, TSharedRef<FWoWGlueFrame>>& FramesByName) const
{
	Frames.Add(Frame);
	FramesByName.Add(Frame->GetName().ToString(), Frame);
	for (const TSharedRef<FWoWGlueFrame>& Child : Frame->GetChildren())
	{
		CollectSubtreeFrames(Child, Frames, FramesByName);
	}
}

FVector2D FWoWGlueFrame::ResolveSubtreeTopLeft(
	TSharedRef<FWoWGlueFrame> Frame,
	const TMap<FString, TSharedRef<FWoWGlueFrame>>& FramesByName,
	TMap<const FWoWGlueFrame*, FVector2D>& PositionCache,
	int32 Depth) const
{
	if (Depth > 32)
	{
		return FVector2D::ZeroVector;
	}

	if (const FVector2D* Cached = PositionCache.Find(&Frame.Get()))
	{
		return *Cached;
	}

	if (&Frame.Get() == this)
	{
		PositionCache.Add(&Frame.Get(), FVector2D::ZeroVector);
		return FVector2D::ZeroVector;
	}

	const FWoWGluePoint& FramePoint = Frame->GetPoint();
	const TSharedRef<FWoWGlueFrame>* RelativeRef = FramesByName.Find(FramePoint.RelativeTo);
	TSharedPtr<FWoWGlueFrame> RelativeFrame = RelativeRef ? *RelativeRef : Frame->GetParent();
	if (!RelativeFrame.IsValid() || RelativeFrame.Get() == &Frame.Get())
	{
		PositionCache.Add(&Frame.Get(), FVector2D::ZeroVector);
		return FVector2D::ZeroVector;
	}

	const FVector2D ParentTopLeft = ResolveSubtreeTopLeft(RelativeFrame.ToSharedRef(), FramesByName, PositionCache, Depth + 1);
	const FVector2D ParentSize = RelativeFrame->GetLayoutSize();
	const FVector2D ChildAlignment = GlueAlignmentFromPoint(FramePoint.Point);
	const FVector2D RelativeAlignment = GlueAlignmentFromPoint(FramePoint.RelativePoint);
	const FVector2D LocalOffset(FramePoint.Offset.X, -FramePoint.Offset.Y);
	const FVector2D Result = ParentTopLeft + (ParentSize * RelativeAlignment) + LocalOffset - (Frame->GetLayoutSize() * ChildAlignment);
	PositionCache.Add(&Frame.Get(), Result);
	return Result;
}

TSharedRef<SWidget> FWoWGlueFrame::BuildSubtreeWidget()
{
	TArray<TSharedRef<FWoWGlueFrame>> Frames;
	TMap<FString, TSharedRef<FWoWGlueFrame>> FramesByName;
	CollectSubtreeFrames(AsShared(), Frames, FramesByName);

	TMap<const FWoWGlueFrame*, FVector2D> PositionCache;
	PositionCache.Add(this, FVector2D::ZeroVector);

	TSharedRef<SConstraintCanvas> Canvas = SNew(SConstraintCanvas);
	for (const TSharedRef<FWoWGlueFrame>& Frame : Frames)
	{
		const FVector2D TopLeft = ResolveSubtreeTopLeft(Frame, FramesByName, PositionCache);
		const FVector2D LayoutSize = Frame->GetLayoutSize();
		Canvas->AddSlot()
			.Anchors(FAnchors(0.0f, 0.0f))
			.Alignment(FVector2D::ZeroVector)
			.Offset(FMargin(TopLeft.X, TopLeft.Y, LayoutSize.X, LayoutSize.Y))
			.ZOrder(Frame->GetEffectiveZOrder())
			[
				Frame->BuildSlateWidget()
			];
	}
	return Canvas;
}

TSharedRef<SWidget> FWoWGlueFrame::BuildSlateWidget()
{
	switch (FrameType)
	{
	case EWoWGlueFrameType::Button:
		if (NormalTexture || PushedTexture || HighlightTexture)
		{
			NormalBrush = MakeTextureBrush(NormalTexture, Size);
			PushedBrush = MakeTextureBrush(PushedTexture ? PushedTexture : NormalTexture, Size);
			HighlightBrush = MakeTextureBrush(HighlightTexture, Size);

			ButtonStyle = MakeShared<FButtonStyle>(FCoreStyle::Get().GetWidgetStyle<FButtonStyle>("Button"));
			if (NormalBrush.IsValid())
			{
				ButtonStyle->SetNormal(*NormalBrush);
			}
			if (PushedBrush.IsValid())
			{
				ButtonStyle->SetPressed(*PushedBrush);
			}
			ButtonStyle->SetHovered(NormalBrush.IsValid() ? *NormalBrush : FCoreStyle::Get().GetWidgetStyle<FButtonStyle>("Button").Hovered);

			TSharedRef<SWidget> ButtonText = SNew(STextBlock)
				.Visibility(this, &FWoWGlueFrame::GetNonInteractiveSlateVisibility)
				.Text(this, &FWoWGlueFrame::GetText)
				.Font(this, &FWoWGlueFrame::GetSlateFont)
				.ColorAndOpacity(this, &FWoWGlueFrame::GetTextSlateColor)
				.ShadowColorAndOpacity(this, &FWoWGlueFrame::GetShadowColor)
				.ShadowOffset(this, &FWoWGlueFrame::GetShadowOffset)
				.Justification(this, &FWoWGlueFrame::GetTextJustification);

			TSharedRef<SWidget> ButtonWidget = SNew(SButton)
				.ButtonStyle(ButtonStyle.Get())
				.IsFocusable(false)
				.ContentPadding(0.0f)
				.HAlign(ParseGlueHAlign(JustifyH))
				.VAlign(ParseGlueVAlign(JustifyV))
				.Visibility(this, &FWoWGlueFrame::GetSlateVisibility)
				.OnClicked(this, &FWoWGlueFrame::HandleClicked)
				.OnHovered(this, &FWoWGlueFrame::HandleMouseEnter)
				.OnUnhovered(this, &FWoWGlueFrame::HandleMouseLeave)
				[
					SNew(SBox)
				];

			if (HighlightBrush.IsValid())
			{
				return SNew(SOverlay)
					.Visibility(this, &FWoWGlueFrame::GetSlateVisibility)
					+ SOverlay::Slot()
					.HAlign(HAlign_Fill)
					.VAlign(VAlign_Fill)
					[
						ButtonWidget
					]
					+ SOverlay::Slot()
					.HAlign(HAlign_Fill)
					.VAlign(VAlign_Fill)
					[
						SNew(SImage)
						.Visibility(this, &FWoWGlueFrame::GetHighlightVisibility)
						.Image(HighlightBrush.Get())
						.ColorAndOpacity(this, &FWoWGlueFrame::GetHighlightSlateColor)
					]
					+ SOverlay::Slot()
					.HAlign(ParseGlueHAlign(JustifyH))
					.VAlign(ParseGlueVAlign(JustifyV))
					[
						ButtonText
					];
			}

			return SNew(SOverlay)
				.Visibility(this, &FWoWGlueFrame::GetSlateVisibility)
				+ SOverlay::Slot()
				.HAlign(HAlign_Fill)
				.VAlign(VAlign_Fill)
				[
					ButtonWidget
				]
				+ SOverlay::Slot()
				.HAlign(ParseGlueHAlign(JustifyH))
				.VAlign(ParseGlueVAlign(JustifyV))
				[
					ButtonText
				];
		}

		if (BackdropBackgroundTexture || BackdropEdgeTexture)
		{
			BackdropBackgroundBrush = MakeTextureBrush(BackdropBackgroundTexture, Size);
			ButtonStyle = MakeShared<FButtonStyle>(FCoreStyle::Get().GetWidgetStyle<FButtonStyle>("Button"));
			const FSlateBrush* NoBrush = FCoreStyle::Get().GetBrush("NoBrush");
			ButtonStyle->SetNormal(*NoBrush);
			ButtonStyle->SetHovered(*NoBrush);
			ButtonStyle->SetPressed(*NoBrush);

			TSharedRef<SWidget> ButtonWidget = SNew(SButton)
				.ButtonStyle(ButtonStyle.Get())
				.IsFocusable(false)
				.ContentPadding(0.0f)
				.HAlign(ParseGlueHAlign(JustifyH))
				.VAlign(ParseGlueVAlign(JustifyV))
				.Visibility(this, &FWoWGlueFrame::GetSlateVisibility)
				.OnClicked(this, &FWoWGlueFrame::HandleClicked)
				.OnHovered(this, &FWoWGlueFrame::HandleMouseEnter)
				.OnUnhovered(this, &FWoWGlueFrame::HandleMouseLeave)
				[
					SNew(STextBlock)
					.Visibility(this, &FWoWGlueFrame::GetNonInteractiveSlateVisibility)
					.Text(this, &FWoWGlueFrame::GetText)
					.Font(this, &FWoWGlueFrame::GetSlateFont)
					.ColorAndOpacity(this, &FWoWGlueFrame::GetTextSlateColor)
					.ShadowColorAndOpacity(this, &FWoWGlueFrame::GetShadowColor)
					.ShadowOffset(this, &FWoWGlueFrame::GetShadowOffset)
					.Justification(this, &FWoWGlueFrame::GetTextJustification)
				];

			return SNew(SBox)
				.Visibility(this, &FWoWGlueFrame::GetSlateVisibility)
				[
					MakeBackdropAtlasWidget(
						BackdropBackgroundTexture,
						BackdropEdgeTexture,
						Size,
						BackdropEdgeSize,
						BackdropInsets,
						GetBackdropBackgroundTint(),
						ButtonWidget)
				];
		}

		{
			ButtonStyle = MakeShared<FButtonStyle>(FCoreStyle::Get().GetWidgetStyle<FButtonStyle>("Button"));
			const FSlateBrush* TransparentButtonBrush = FCoreStyle::Get().GetBrush("NoBrush");
			ButtonStyle->SetNormal(*TransparentButtonBrush);
			ButtonStyle->SetHovered(*TransparentButtonBrush);
			ButtonStyle->SetPressed(*TransparentButtonBrush);

			return SNew(SButton)
				.ButtonStyle(ButtonStyle.Get())
				.IsFocusable(false)
				.ContentPadding(0.0f)
				.HAlign(ParseGlueHAlign(JustifyH))
				.VAlign(ParseGlueVAlign(JustifyV))
				.Visibility(this, &FWoWGlueFrame::GetSlateVisibility)
				.OnClicked(this, &FWoWGlueFrame::HandleClicked)
				.OnHovered(this, &FWoWGlueFrame::HandleMouseEnter)
				.OnUnhovered(this, &FWoWGlueFrame::HandleMouseLeave)
				[
					SNew(STextBlock)
					.Text(this, &FWoWGlueFrame::GetText)
					.Font(this, &FWoWGlueFrame::GetSlateFont)
					.ColorAndOpacity(this, &FWoWGlueFrame::GetTextSlateColor)
					.ShadowColorAndOpacity(this, &FWoWGlueFrame::GetShadowColor)
					.ShadowOffset(this, &FWoWGlueFrame::GetShadowOffset)
					.Justification(this, &FWoWGlueFrame::GetTextJustification)
				];
		}

	case EWoWGlueFrameType::EditBox:
	{
		EditBoxStyle = MakeShared<FEditableTextBoxStyle>(FCoreStyle::Get().GetWidgetStyle<FEditableTextBoxStyle>("NormalEditableTextBox"));
		const FSlateBrush* NoBrush = FCoreStyle::Get().GetBrush("NoBrush");
		EditBoxStyle->SetBackgroundImageNormal(*NoBrush);
		EditBoxStyle->SetBackgroundImageHovered(*NoBrush);
		EditBoxStyle->SetBackgroundImageFocused(*NoBrush);
		EditBoxStyle->SetBackgroundImageReadOnly(*NoBrush);
		EditBoxStyle->SetBackgroundColor(FSlateColor(FLinearColor::Transparent));
		EditBoxStyle->SetFont(GetSlateFont());
		EditBoxStyle->SetForegroundColor(GetEditBoxInputSlateColor());
		EditBoxStyle->SetFocusedForegroundColor(GetEditBoxInputSlateColor());
		EditBoxStyle->SetReadOnlyForegroundColor(GetEditBoxInputSlateColor());
		EditBoxStyle->TextStyle.SetShadowColorAndOpacity(ShadowColor);
		EditBoxStyle->TextStyle.SetShadowOffset(ShadowOffset);
		EditBoxStyle->TextStyle.SetColorAndOpacity(GetEditBoxInputSlateColor());
		if (bPassword)
		{
			const FString PasswordFontPath = FPaths::EngineContentDir() / TEXT("Slate/Fonts/Roboto-Regular.ttf");
			const FFontOutlineSettings OutlineSettings = MakeGlueFontOutlineSettings(FontFlags);
PRAGMA_DISABLE_DEPRECATION_WARNINGS
			const FSlateFontInfo PasswordFont(PasswordFontPath, FontSize, EFontHinting::Default, OutlineSettings);
PRAGMA_ENABLE_DEPRECATION_WARNINGS
			EditBoxStyle->SetFont(PasswordFont);
			EditBoxStyle->TextStyle.SetFont(PasswordFont);
		}
		EditBoxStyle->SetPadding(TextInsets);

		TSharedPtr<SWoWEditableTextBox> EditBoxSlateWidget;
		TSharedRef<SWidget> EditBoxWidget = SAssignNew(EditBoxSlateWidget, SWoWEditableTextBox)
			.Style(EditBoxStyle.Get())
			.Visibility(this, &FWoWGlueFrame::GetSlateVisibility)
			.IsInteractionEnabled(this, &FWoWGlueFrame::IsEnabled)
			.IsReadOnly(TAttribute<bool>::Create(TAttribute<bool>::FGetter::CreateLambda([this]()
			{
				return !IsEnabled();
			})))
			.Text(this, &FWoWGlueFrame::GetText)
			.HintText(FText::GetEmpty())
			.IsPassword(bPassword)
			.Justification(this, &FWoWGlueFrame::GetTextJustification)
			.SelectAllTextWhenFocused(true)
			.OnTextChanged(this, &FWoWGlueFrame::HandleTextChanged)
			.OnKeyDownHandler(FOnKeyDown::CreateSP(this, &FWoWGlueFrame::HandleEditKeyDown))
			.OnTabPressed(FOnClicked::CreateSP(this, &FWoWGlueFrame::HandleEditTabPressed))
			.OnEnterPressed(FOnClicked::CreateSP(this, &FWoWGlueFrame::HandleEditEnterPressed))
			.OnEscapePressed(FOnClicked::CreateSP(this, &FWoWGlueFrame::HandleEditEscapePressed))
			.OnEditFocusGained(FSimpleDelegate::CreateSP(this, &FWoWGlueFrame::HandleEditFocusGained))
			.OnEditFocusLost(FSimpleDelegate::CreateSP(this, &FWoWGlueFrame::HandleEditFocusLost))
			.OnEditMouseDown(FSimpleDelegate::CreateSP(this, &FWoWGlueFrame::HandleEditFocusGained));
		EditableTextBoxWidget = StaticCastSharedPtr<SEditableTextBox>(EditBoxSlateWidget);
		if (bPendingKeyboardFocus)
		{
			EditBoxSlateWidget->RegisterActiveTimer(0.0f, FWidgetActiveTimerDelegate::CreateLambda([WeakFrame = TWeakPtr<FWoWGlueFrame>(AsShared())](double, float)
			{
				if (TSharedPtr<FWoWGlueFrame> Frame = WeakFrame.Pin())
				{
					Frame->SetKeyboardFocus();
				}
				return EActiveTimerReturnType::Stop;
			}));
		}

		TSharedRef<SWidget> EditBoxContent = SNew(SOverlay)
			.Visibility(this, &FWoWGlueFrame::GetSlateVisibility)
			+ SOverlay::Slot()
			.HAlign(HAlign_Fill)
			.VAlign(VAlign_Fill)
			[
				EditBoxWidget
			]
			+ SOverlay::Slot()
			.HAlign(ParseGlueHAlign(PlaceholderJustifyH))
			.VAlign(ParseGlueVAlign(JustifyV))
			.Padding(TextInsets)
			[
				SNew(STextBlock)
				.Visibility(this, &FWoWGlueFrame::GetPlaceholderVisibility)
				.Text(this, &FWoWGlueFrame::GetPlaceholderText)
				.Font(this, &FWoWGlueFrame::GetSlateFont)
				.ColorAndOpacity(this, &FWoWGlueFrame::GetPlaceholderSlateColor)
				.ShadowColorAndOpacity(this, &FWoWGlueFrame::GetShadowColor)
				.ShadowOffset(this, &FWoWGlueFrame::GetShadowOffset)
				.Justification(this, &FWoWGlueFrame::GetPlaceholderTextJustification)
			]
			;

		if (BackdropBackgroundTexture || BackdropEdgeTexture)
		{
			BackdropBackgroundBrush = MakeTextureBrush(BackdropBackgroundTexture, Size);
			return SNew(SBox)
				.Visibility(this, &FWoWGlueFrame::GetSlateVisibility)
				[
					MakeBackdropAtlasWidget(
						BackdropBackgroundTexture,
						BackdropEdgeTexture,
						Size,
						BackdropEdgeSize,
						BackdropInsets,
						GetBackdropBackgroundTint(),
						EditBoxContent)
				];
		}

		return EditBoxContent;
	}

	case EWoWGlueFrameType::FontString:
		return SNew(SBox)
			.Visibility(this, &FWoWGlueFrame::GetNonInteractiveSlateVisibility)
			.WidthOverride(this, &FWoWGlueFrame::GetSlateWidthOverride)
			.HeightOverride(this, &FWoWGlueFrame::GetSlateHeightOverride)
			.Clipping(EWidgetClipping::ClipToBounds)
			[
				SNew(SWoWSimpleHTMLText)
				.Frame(AsShared())
			];

	case EWoWGlueFrameType::ScrollFrame:
	{
		const TSharedPtr<FWoWGlueFrame> PinnedScrollChild = ScrollChild.Pin();
		return SNew(SBox)
			.Visibility(this, &FWoWGlueFrame::GetSlateVisibility)
			.WidthOverride(Size.X)
			.HeightOverride(Size.Y)
			.Clipping(EWidgetClipping::ClipToBounds)
			[
				SNew(SWoWGlueScrollFrame)
				.Frame(AsShared())
				.ScrollChild(PinnedScrollChild)
			];
	}

	case EWoWGlueFrameType::SimpleHTML:
	{
		return SNew(SWoWSimpleHTMLText)
			.Visibility(this, &FWoWGlueFrame::GetSlateVisibility)
			.Frame(AsShared());
	}

	case EWoWGlueFrameType::Texture:
		TextureBrush = MainTextureUVRect.bValid
			? MakeTextureRegionBrush(MainTexture, Size, ToSlateUVRegion(MainTextureUVRect))
			: MakeTextureBrush(MainTexture, Size);
		if (TextureBrush.IsValid())
		{
			return SNew(SImage)
				.Visibility(this, &FWoWGlueFrame::GetNonInteractiveSlateVisibility)
				.Image(TextureBrush.Get())
				.ColorAndOpacity(this, &FWoWGlueFrame::GetTextureSlateColor);
		}
		return SNew(SBox)
			.Visibility(this, &FWoWGlueFrame::GetNonInteractiveSlateVisibility);

	case EWoWGlueFrameType::Frame:
		if (BackdropBackgroundTexture || BackdropEdgeTexture)
		{
			TSharedRef<SWidget> BackdropWidget = SNew(SBox)
				.Visibility(this, &FWoWGlueFrame::GetNonInteractiveSlateVisibility)
				[
					MakeBackdropAtlasWidget(
						BackdropBackgroundTexture,
						BackdropEdgeTexture,
						Size,
						BackdropEdgeSize,
						BackdropInsets,
						GetBackdropBackgroundTint(),
						SNew(SOverlay))
				];
			if (bMouseEnabled)
			{
				return SNew(SOverlay)
					.Visibility(this, &FWoWGlueFrame::GetSlateVisibility)
					+ SOverlay::Slot()
					.HAlign(HAlign_Fill)
					.VAlign(VAlign_Fill)
					[
						SNew(SWoWMouseBlocker)
						.Visibility(this, &FWoWGlueFrame::GetSlateVisibility)
					]
					+ SOverlay::Slot()
					.HAlign(HAlign_Fill)
					.VAlign(VAlign_Fill)
					[
						BackdropWidget
					];
			}
			return BackdropWidget;
		}
		if (bMouseEnabled)
		{
			return SNew(SWoWMouseBlocker)
				.Visibility(this, &FWoWGlueFrame::GetSlateVisibility);
		}
		return SNew(SBox)
			.Visibility(this, &FWoWGlueFrame::GetNonInteractiveSlateVisibility)
			[
				SNew(SOverlay)
			];

	default:
		return SNew(SBox)
			.Visibility(this, &FWoWGlueFrame::GetSlateVisibility);
	}
}

FMargin FWoWGlueFrame::GetCanvasOffset() const
{
	// WoW UI offsets use a bottom-left style Y direction: positive Y moves upward.
	// Slate canvas offsets use screen-space Y where positive moves downward.
	return FMargin(Point.Offset.X, -Point.Offset.Y, Size.X, Size.Y);
}

FVector2D FWoWGlueFrame::GetCanvasAlignment() const
{
	return GlueAlignmentFromPoint(Point.Point);
}

FAnchors FWoWGlueFrame::GetCanvasAnchors() const
{
	return GlueAnchorFromPoint(Point.RelativePoint);
}

bool FWoWGlueFrame::IsEffectivelyShown() const
{
	if (!bShown)
	{
		return false;
	}

	TSharedPtr<FWoWGlueFrame> PinnedParent = Parent.Pin();
	while (PinnedParent.IsValid())
	{
		if (!PinnedParent->bShown)
		{
			return false;
		}
		PinnedParent = PinnedParent->Parent.Pin();
	}
	return true;
}

int32 FWoWGlueFrame::GetEffectiveZOrder() const
{
	int32 EffectiveZ = DrawLayer + RaiseOrder * 10000;
	TSharedPtr<FWoWGlueFrame> PinnedParent = Parent.Pin();
	while (PinnedParent.IsValid())
	{
		EffectiveZ += PinnedParent->DrawLayer + PinnedParent->RaiseOrder * 10000;
		PinnedParent = PinnedParent->Parent.Pin();
	}
	return EffectiveZ;
}

EVisibility FWoWGlueFrame::GetSlateVisibility() const
{
	return IsEffectivelyShown() ? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility FWoWGlueFrame::GetNonInteractiveSlateVisibility() const
{
	return IsEffectivelyShown() ? EVisibility::HitTestInvisible : EVisibility::Collapsed;
}

EVisibility FWoWGlueFrame::GetHighlightVisibility() const
{
	return IsEffectivelyShown() && bHovered ? EVisibility::HitTestInvisible : EVisibility::Collapsed;
}

FSlateColor FWoWGlueFrame::GetHighlightSlateColor() const
{
	FLinearColor Color = ApplyAlphaModeSlateColor(VertexColor, HighlightAlphaMode);
	Color.A *= Alpha;
	return FSlateColor(Color);
}

FSlateColor FWoWGlueFrame::GetTextureSlateColor() const
{
	FLinearColor Color = ApplyAlphaModeSlateColor(VertexColor, MainTextureAlphaMode);
	Color.A *= Alpha;
	return FSlateColor(Color);
}

FLinearColor FWoWGlueFrame::GetBackdropBackgroundTint() const
{
	if (!bHasExplicitVertexColor)
	{
		return FLinearColor(0.0f, 0.0f, 0.0f, 0.78f);
	}
	return GetTextureSlateColor().GetSpecifiedColor();
}

FSlateFontInfo FWoWGlueFrame::GetSlateFont() const
{
	const FFontOutlineSettings OutlineSettings = MakeGlueFontOutlineSettings(FontFlags);
	const FString ResolvedFontPath = ResolveGlueFontPath(FontPath);
	if (!ResolvedFontPath.IsEmpty())
	{
PRAGMA_DISABLE_DEPRECATION_WARNINGS
		return FSlateFontInfo(ResolvedFontPath, FontSize, EFontHinting::Default, OutlineSettings);
PRAGMA_ENABLE_DEPRECATION_WARNINGS
	}
	return FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), FontSize, OutlineSettings);
}

FSlateColor FWoWGlueFrame::GetTextSlateColor() const
{
	FLinearColor Color = TextColor * VertexColor;
	Color.A = TextColor.A * VertexColor.A * Alpha;
	return FSlateColor(Color);
}

FSlateColor FWoWGlueFrame::GetEditBoxInputSlateColor() const
{
	return FSlateColor(TextColor);
}

FText FWoWGlueFrame::GetPlaceholderText() const
{
	return PlaceholderText;
}

EVisibility FWoWGlueFrame::GetPlaceholderVisibility() const
{
	if (bEditFocused || !Text.IsEmpty())
	{
		return EVisibility::Collapsed;
	}
	return PlaceholderText.IsEmpty() ? EVisibility::Collapsed : EVisibility::HitTestInvisible;
}

FSlateColor FWoWGlueFrame::GetPlaceholderSlateColor() const
{
	FLinearColor Color = PlaceholderTextColor;
	Color.A *= VertexColor.A * Alpha;
	return FSlateColor(Color);
}

ETextJustify::Type FWoWGlueFrame::GetPlaceholderTextJustification() const
{
	return ParseGlueJustifyH(PlaceholderJustifyH);
}

FText FWoWGlueFrame::GetSimpleHTMLRichText() const
{
	return FText::FromString(ConvertSimpleHTMLToRichText(Text.ToString()));
}

float FWoWGlueFrame::GetVerticalScrollRange() const
{
	if (FrameType != EWoWGlueFrameType::ScrollFrame)
	{
		return 0.0f;
	}

	const TSharedPtr<FWoWGlueFrame> PinnedScrollChild = ScrollChild.Pin();
	if (!PinnedScrollChild.IsValid())
	{
		return 0.0f;
	}

	const FVector2D ContentSize = PinnedScrollChild->MeasureDesiredContentSize();
	return FMath::Max(0.0f, ContentSize.Y - Size.Y);
}

bool FWoWGlueFrame::GetSimpleHTMLAutoWrapText() const
{
	FString NormalizedJustifyH = JustifyH;
	NormalizedJustifyH.TrimStartAndEndInline();
	NormalizedJustifyH.ToUpperInline();
	return !(NormalizedJustifyH == TEXT("JUSTIFY") || NormalizedJustifyH == TEXT("DISTRIBUTE") || NormalizedJustifyH == TEXT("DISTRIBUTED"));
}

float FWoWGlueFrame::GetSimpleHTMLWrapTextAt() const
{
	if (FrameType == EWoWGlueFrameType::FontString && !bHasExplicitSize)
	{
		return 0.0f;
	}
	return GetSimpleHTMLAutoWrapText() ? Size.X : 0.0f;
}

float FWoWGlueFrame::GetSimpleHTMLLineHeightPercentage() const
{
	return TextLineHeightPercentage;
}

FVector2D FWoWGlueFrame::MeasureNaturalTextSize() const
{
	const FString TextString = StripWoWTextControlCodes(Text.ToString());
	if (TextString.IsEmpty())
	{
		return FVector2D(FMath::Max(1.0f, FontSize * 0.5f), FMath::Max(1.0f, FontSize * 1.35f));
	}

	return EstimateGlueTextSize(TextString, FontSize);
}

FVector2D FWoWGlueFrame::MeasureDesiredContentSize() const
{
	switch (FrameType)
	{
	case EWoWGlueFrameType::FontString:
	{
		const FString TextString = StripWoWTextControlCodes(Text.ToString());
		if (!bHasExplicitSize)
		{
			return MeasureNaturalTextSize();
		}
		if (TextString.IsEmpty())
		{
			return FVector2D(FMath::Max(1.0f, Size.X), FMath::Max(1.0f, FontSize * 1.35f));
		}
		const float WrapWidth = Size.X > 0.0f ? Size.X : 4096.0f;
		const FVector2D Estimated = EstimateGlueTextSize(TextString, FontSize, WrapWidth);
		return FVector2D(FMath::Max(1.0f, Estimated.X), FMath::Max(1.0f, Estimated.Y));
	}
	case EWoWGlueFrameType::SimpleHTML:
		return SWoWSimpleHTMLText::MeasureDesiredSize(*this);

	default:
		return Size;
	}
}

FVector2D FWoWGlueFrame::GetLuaReportedSize() const
{
	// WoW 的 Glue UI 会直接用 FontString/SimpleHTML:GetWidth/GetHeight 计算弹窗和气泡大小。
	// 无显式 Size 的 FontString 返回自然文本尺寸；有显式宽度的文本控件按该宽度测量换行高度。
	if (FrameType == EWoWGlueFrameType::FontString || FrameType == EWoWGlueFrameType::SimpleHTML)
	{
		return bHasExplicitSize || FrameType == EWoWGlueFrameType::SimpleHTML
			? MeasureDesiredContentSize()
			: MeasureNaturalTextSize();
	}
	return GetLayoutSize();
}

FVector2D FWoWGlueFrame::GetLayoutSize() const
{
	if (bSetAllPoints)
	{
		if (const TSharedPtr<FWoWGlueFrame> ParentFrame = Parent.Pin())
		{
			return ParentFrame->GetLayoutSize();
		}
	}

	if (FrameType == EWoWGlueFrameType::FontString && !bHasExplicitSize)
	{
		return MeasureNaturalTextSize();
	}

	if (Size.X > 0.0f && Size.Y > 0.0f)
	{
		return Size;
	}

	const FVector2D DesiredSize = MeasureDesiredContentSize();
	return FVector2D(
		Size.X > 0.0f ? Size.X : DesiredSize.X,
		Size.Y > 0.0f ? Size.Y : DesiredSize.Y);
}

FOptionalSize FWoWGlueFrame::GetSlateWidthOverride() const
{
	if (FrameType == EWoWGlueFrameType::FontString && !bHasExplicitSize)
	{
		return FOptionalSize();
	}
	return Size.X > 0.0f ? FOptionalSize(Size.X) : FOptionalSize();
}

FOptionalSize FWoWGlueFrame::GetSlateHeightOverride() const
{
	if (FrameType == EWoWGlueFrameType::FontString && !bHasExplicitSize)
	{
		return FOptionalSize();
	}
	return Size.Y > 0.0f ? FOptionalSize(Size.Y) : FOptionalSize();
}

ETextJustify::Type FWoWGlueFrame::GetTextJustification() const
{
	return ParseGlueJustifyH(JustifyH);
}

EVerticalAlignment FWoWGlueFrame::GetVerticalAlignment() const
{
	return ParseGlueVAlign(JustifyV);
}

FReply FWoWGlueFrame::HandleClicked()
{
	if (!bEnabled)
	{
		return FReply::Handled();
	}

	const double NowSeconds = FSlateApplication::IsInitialized()
		? FSlateApplication::Get().GetCurrentTime()
		: FPlatformTime::Seconds();
	const bool bDoubleClick = LastClickTimeSeconds >= 0.0 && (NowSeconds - LastClickTimeSeconds) <= 0.45;
	LastClickTimeSeconds = NowSeconds;

	if (OnClickScript)
	{
		OnClickScript(AsShared());
	}
	if (bDoubleClick && OnDoubleClickScript)
	{
		OnDoubleClickScript(AsShared());
	}
	if (TSharedPtr<FWoWGlueUIManager> Manager = OwnerManager.Pin())
	{
		Manager->RestoreActiveEditBoxFocus();
	}
	return FReply::Handled();
}

void FWoWGlueFrame::Click()
{
	HandleClicked();
}

void FWoWGlueFrame::TriggerDoubleClick()
{
	if (!bEnabled)
	{
		return;
	}

	if (OnDoubleClickScript)
	{
		OnDoubleClickScript(AsShared());
	}
}

void FWoWGlueFrame::TriggerHyperlinkClick(const FString& Link, const FString& LinkText)
{
	if (OnHyperlinkClickScript)
	{
		OnHyperlinkClickScript(AsShared(), Link, LinkText);
	}
}

void FWoWGlueFrame::HandleTextChanged(const FText& InText)
{
	if (!bEnabled)
	{
		return;
	}

	Text = InText;
	if (OnTextChangedScript)
	{
		OnTextChangedScript(AsShared());
	}
}

FReply FWoWGlueFrame::HandleEditKeyDown(const FGeometry&, const FKeyEvent& InKeyEvent)
{
	if (InKeyEvent.GetKey() == EKeys::Tab)
	{
		return HandleEditTabPressed();
	}
	if (InKeyEvent.GetKey() == EKeys::Enter || InKeyEvent.GetKey() == EKeys::Virtual_Gamepad_Accept.GetVirtualKey())
	{
		return HandleEditEnterPressed();
	}
	if (InKeyEvent.GetKey() == EKeys::Escape)
	{
		return HandleEditEscapePressed();
	}
	return FReply::Unhandled();
}

void FWoWGlueFrame::HandleEditFocusGained()
{
	if (!bEnabled)
	{
		return;
	}

	bEditFocused = true;
	if (TSharedPtr<FWoWGlueUIManager> Manager = OwnerManager.Pin())
	{
		Manager->SetActiveEditBox(AsShared());
	}
	if (OnEditFocusGainedScript)
	{
		OnEditFocusGainedScript(AsShared());
	}
}

void FWoWGlueFrame::HandleEditFocusLost()
{
	bEditFocused = false;
	if (OnEditFocusLostScript)
	{
		OnEditFocusLostScript(AsShared());
	}
}

FReply FWoWGlueFrame::HandleEditTabPressed()
{
	if (!bEnabled || !OnTabPressedScript)
	{
		return FReply::Unhandled();
	}

	OnTabPressedScript(AsShared());
	return FReply::Handled();
}

FReply FWoWGlueFrame::HandleEditEnterPressed()
{
	if (!bEnabled || !OnEnterPressedScript)
	{
		return FReply::Unhandled();
	}

	OnEnterPressedScript(AsShared());
	return FReply::Handled();
}

FReply FWoWGlueFrame::HandleEditEscapePressed()
{
	if (!bEnabled || !OnEscapePressedScript)
	{
		return FReply::Unhandled();
	}

	OnEscapePressedScript(AsShared());
	return FReply::Handled();
}

void FWoWGlueFrame::HandleMouseEnter()
{
	if (!bEnabled)
	{
		return;
	}

	bHovered = true;
	if (OnEnterScript)
	{
		OnEnterScript(AsShared());
	}
}

void FWoWGlueFrame::HandleMouseLeave()
{
	if (!bHovered)
	{
		return;
	}

	bHovered = false;
	if (OnLeaveScript)
	{
		OnLeaveScript(AsShared());
	}
}

void FWoWGlueFrame::HandleMouseWheel(float WheelDelta)
{
	if (!bEnabled || FrameType != EWoWGlueFrameType::ScrollFrame)
	{
		return;
	}

	const float ScrollStep = FMath::Max(1.0f, ScrollBarStyle.Step);
	const float TargetScroll = VerticalScroll - WheelDelta * ScrollStep;
	SetVerticalScroll(TargetScroll);
}

void FWoWGlueFrame::HandleHyperlinkClicked(const FSlateHyperlinkRun::FMetadata& Metadata)
{
	if (!bEnabled)
	{
		return;
	}

	const FString* Link = Metadata.Find(TEXT("href"));
	const FString* LinkText = Metadata.Find(TEXT("text"));
	TriggerHyperlinkClick(Link ? *Link : FString(), LinkText ? *LinkText : FString());
}

EWoWGlueFrameType FWoWGlueFrame::ParseFrameType(const FString& InTypeName)
{
	return ParseGlueFrameTypeName(InTypeName);
}

FWoWGlueUIManager::FWoWGlueUIManager()
{
	LoadGlueRenderPolicy();
	UIParent = MakeShared<FWoWGlueFrame>(EWoWGlueFrameType::Frame, TEXT("Frame"), FName(TEXT("UIParent")), nullptr);
	UIParent->SetSize(1920.0f, 1080.0f);
	FramesByName.Add(TEXT("UIParent"), UIParent.ToSharedRef());
}

FWoWGlueUIManager::~FWoWGlueUIManager()
{
	for (const TPair<FString, UTexture2D*>& Pair : TextureCache)
	{
		if (Pair.Value)
		{
			Pair.Value->RemoveFromRoot();
		}
	}
	TextureCache.Reset();
}

FVector2D FWoWGlueUIManager::GetScreenSize() const
{
	if (GEngine && GEngine->GameViewport)
	{
		FVector2D ViewportSize = FVector2D::ZeroVector;
		GEngine->GameViewport->GetViewportSize(ViewportSize);
		if (ViewportSize.X > 0.0f && ViewportSize.Y > 0.0f)
		{
			return ViewportSize;
		}
	}

	return FVector2D(1920.0f, 1080.0f);
}

void FWoWGlueUIManager::UpdateRootFrameSize()
{
	if (UIParent.IsValid())
	{
		const FVector2D ScreenSize = GetScreenSize();
		UIParent->SetSize(ScreenSize.X, ScreenSize.Y);
	}
}

TSharedRef<FWoWGlueFrame> FWoWGlueUIManager::CreateFrame(const FString& TypeName, const FString& Name, TSharedPtr<FWoWGlueFrame> Parent)
{
	const FString EffectiveName = Name.IsEmpty()
		? FString::Printf(TEXT("$unnamed_%d"), FramesInCreationOrder.Num())
		: Name;
	const FString EffectiveTypeName = TypeName.IsEmpty() ? TEXT("Frame") : TypeName;
	const EWoWGlueFrameType FrameType = ParseGlueFrameTypeName(EffectiveTypeName);
	TSharedPtr<FWoWGlueFrame> EffectiveParent = Parent.IsValid() ? Parent : UIParent;
	TSharedRef<FWoWGlueFrame> Frame = MakeShared<FWoWGlueFrame>(FrameType, EffectiveTypeName, FName(*EffectiveName), EffectiveParent);
	Frame->SetOwnerManager(AsShared());
	if (EffectiveParent.IsValid())
	{
		EffectiveParent->AddChild(Frame);
	}
	FramesByName.Add(EffectiveName, Frame);
	FramesInCreationOrder.Add(Frame);
	return Frame;
}

void FWoWGlueUIManager::SetActiveEditBox(TSharedPtr<FWoWGlueFrame> InEditBox)
{
	if (InEditBox.IsValid() &&
		InEditBox->GetFrameType() == EWoWGlueFrameType::EditBox &&
		InEditBox->KeepsKeyboardFocus())
	{
		ActiveEditBox = InEditBox;
	}
}

TSharedPtr<FWoWGlueFrame> FWoWGlueUIManager::GetActiveEditBox() const
{
	return ActiveEditBox.Pin();
}

void FWoWGlueUIManager::RestoreActiveEditBoxFocus()
{
	TSharedPtr<FWoWGlueFrame> EditBox = ActiveEditBox.Pin();
	if (!EditBox.IsValid() || !EditBox->KeepsKeyboardFocus() || !EditBox->IsShown() || !EditBox->IsEnabled())
	{
		return;
	}

	if (FSlateApplication::IsInitialized())
	{
		const TSharedPtr<SWidget> CurrentFocus = FSlateApplication::Get().GetKeyboardFocusedWidget();
		if (CurrentFocus.IsValid() && CurrentFocus->GetTypeAsString().Contains(TEXT("EditableText")))
		{
			return;
		}
	}

	EditBox->SetKeyboardFocus(false);
}

void FWoWGlueUIManager::RestoreActiveEditBoxFocusDeferred(TSharedRef<SWidget> TimerHost)
{
	TimerHost->RegisterActiveTimer(0.0f, FWidgetActiveTimerDelegate::CreateLambda([WeakManager = TWeakPtr<FWoWGlueUIManager>(AsShared())](double, float)
	{
		if (TSharedPtr<FWoWGlueUIManager> Manager = WeakManager.Pin())
		{
			Manager->RestoreActiveEditBoxFocus();
		}
		return EActiveTimerReturnType::Stop;
	}));
}

TSharedPtr<FWoWGlueFrame> FWoWGlueUIManager::FindFrame(const FString& Name) const
{
	if (const TSharedRef<FWoWGlueFrame>* Found = FramesByName.Find(Name))
	{
		return *Found;
	}
	return nullptr;
}

TSharedPtr<FWoWGlueFrame> FWoWGlueUIManager::ResolveRelativeFrame(const FWoWGlueFrame& Frame) const
{
	const FString& RelativeName = Frame.GetPoint().RelativeTo;
	if (RelativeName.IsEmpty() || RelativeName == TEXT("UIParent") || RelativeName == Frame.GetName().ToString())
	{
		return nullptr;
	}
	return FindFrame(RelativeName);
}

FAnchors FWoWGlueUIManager::ResolveCanvasAnchors(TSharedRef<FWoWGlueFrame> Frame, int32 Depth) const
{
	if (Depth > 32)
	{
		return Frame->GetCanvasAnchors();
	}

	TSharedPtr<FWoWGlueFrame> RelativeFrame = ResolveRelativeFrame(Frame.Get());
	if (!RelativeFrame.IsValid())
	{
		return Frame->GetCanvasAnchors();
	}
	return ResolveCanvasAnchors(RelativeFrame.ToSharedRef(), Depth + 1);
}

FVector2D FWoWGlueUIManager::ResolveCanvasAlignment(TSharedRef<FWoWGlueFrame> Frame, int32 Depth) const
{
	if (Depth > 32)
	{
		return Frame->GetCanvasAlignment();
	}

	TSharedPtr<FWoWGlueFrame> RelativeFrame = ResolveRelativeFrame(Frame.Get());
	if (!RelativeFrame.IsValid())
	{
		return Frame->GetCanvasAlignment();
	}
	return ResolveCanvasAlignment(RelativeFrame.ToSharedRef(), Depth + 1);
}

FVector2D FWoWGlueUIManager::ResolveCanvasOffset(TSharedRef<FWoWGlueFrame> Frame, int32 Depth) const
{
	if (Depth > 32)
	{
		return FVector2D(Frame->GetCanvasOffset().Left, Frame->GetCanvasOffset().Top);
	}

	const FWoWGluePoint& Point = Frame->GetPoint();
	const FVector2D ChildAlignment = GlueAlignmentFromPoint(Point.Point);
	const FVector2D LocalOffset(Point.Offset.X, -Point.Offset.Y);
	TSharedPtr<FWoWGlueFrame> RelativeFrame = ResolveRelativeFrame(Frame.Get());
	if (!RelativeFrame.IsValid())
	{
		return LocalOffset;
	}

	const FVector2D RelativeOffset = ResolveCanvasOffset(RelativeFrame.ToSharedRef(), Depth + 1);
	const FVector2D RelativeCanvasAlignment = ResolveCanvasAlignment(RelativeFrame.ToSharedRef(), Depth + 1);
	const FVector2D RelativeTopLeft = RelativeOffset - RelativeFrame->GetLayoutSize() * RelativeCanvasAlignment;
	const FVector2D RelativeAlignment = GlueAlignmentFromPoint(Point.RelativePoint);
	const FVector2D RelativeAnchor = RelativeTopLeft + RelativeFrame->GetLayoutSize() * RelativeAlignment;
	const FVector2D TargetTopLeft = RelativeAnchor + LocalOffset - Frame->GetLayoutSize() * ChildAlignment;
	return TargetTopLeft + Frame->GetLayoutSize() * ResolveCanvasAlignment(Frame, Depth);
}

bool FWoWGlueUIManager::IsOwnedScrollChild(TSharedRef<FWoWGlueFrame> Frame) const
{
	for (const TSharedRef<FWoWGlueFrame>& Candidate : FramesInCreationOrder)
	{
		const TSharedPtr<FWoWGlueFrame> ScrollChild = Candidate->GetScrollChild();
		if (!ScrollChild.IsValid())
		{
			continue;
		}

		if (ScrollChild.Get() == &Frame.Get())
		{
			return true;
		}

		TSharedPtr<FWoWGlueFrame> Parent = Frame->GetParent();
		while (Parent.IsValid())
		{
			if (Parent.Get() == ScrollChild.Get())
			{
				return true;
			}
			Parent = Parent->GetParent();
		}
	}
	return false;
}

TSharedRef<SWidget> FWoWGlueUIManager::MakeSlateWidget()
{
	UpdateRootFrameSize();
	TSharedRef<SConstraintCanvas> Canvas = SNew(SConstraintCanvas);
	TSet<const FWoWGlueFrame*> OwnedScrollChildren;
	for (const TSharedRef<FWoWGlueFrame>& Candidate : FramesInCreationOrder)
	{
		if (const TSharedPtr<FWoWGlueFrame> ScrollChild = Candidate->GetScrollChild())
		{
			TArray<TSharedRef<FWoWGlueFrame>> Pending;
			Pending.Add(ScrollChild.ToSharedRef());
			while (Pending.Num() > 0)
			{
				TSharedRef<FWoWGlueFrame> Current = Pending.Pop(EAllowShrinking::No);
				OwnedScrollChildren.Add(&Current.Get());
				for (const TSharedRef<FWoWGlueFrame>& Child : Current->GetChildren())
				{
					Pending.Add(Child);
				}
			}
		}
	}
	for (const TSharedRef<FWoWGlueFrame>& Frame : FramesInCreationOrder)
	{
		if (OwnedScrollChildren.Contains(&Frame.Get()))
		{
			continue;
		}

		if (Frame->GetFrameType() == EWoWGlueFrameType::Button)
		{
			// Texture fields are intentionally resolved late so Lua can set size before textures.
		}
		Canvas->AddSlot()
			.Anchors(ResolveCanvasAnchors(Frame))
			.Alignment(ResolveCanvasAlignment(Frame))
			.Offset(TAttribute<FMargin>::Create(TAttribute<FMargin>::FGetter::CreateLambda([this, Frame]()
			{
				const FVector2D ResolvedOffset = ResolveCanvasOffset(Frame);
				const FVector2D LayoutSize = Frame->GetLayoutSize();
				return FMargin(ResolvedOffset.X, ResolvedOffset.Y, LayoutSize.X, LayoutSize.Y);
			})))
			.ZOrder(Frame->GetEffectiveZOrder())
			[
				Frame->BuildSlateWidget()
			];
	}
	return Canvas;
}

FString FWoWGlueUIManager::ResolveDataPath(const FString& WowPath) const
{
	FString Normalized = WowPath;
	Normalized.ReplaceInline(TEXT("\\"), TEXT("/"));
	Normalized.RemoveFromStart(TEXT("/"));
	if (!FPaths::GetExtension(Normalized).Len())
	{
		Normalized += TEXT(".png");
	}

	const FString DataPath = FPaths::ProjectDir() / TEXT("Data") / Normalized;
	if (FPaths::FileExists(DataPath))
	{
		return DataPath;
	}

	return FPaths::ProjectContentDir() / Normalized;
}

UTexture2D* FWoWGlueUIManager::LoadTexture(const FString& WowPath, EWoWGlueAlphaMode AlphaMode)
{
	const FString ResolvedPath = ResolveDataPath(WowPath);
	const FString CacheKey = ResolvedPath + AlphaModeToCacheSuffix(AlphaMode) + MakeRenderPolicyCacheSuffix();
	if (UTexture2D** CachedTexture = TextureCache.Find(CacheKey))
	{
		return *CachedTexture;
	}

	TArray<uint8> CompressedData;
	if (!FFileHelper::LoadFileToArray(CompressedData, *ResolvedPath))
	{
		UE_LOG(LogTemp, Warning, TEXT("Glue texture not found: %s -> %s"), *WowPath, *ResolvedPath);
		return nullptr;
	}

	EImageFormat ImageFormat = EImageFormat::PNG;
	const FString Extension = FPaths::GetExtension(ResolvedPath).ToLower();
	if (Extension == TEXT("jpg") || Extension == TEXT("jpeg"))
	{
		ImageFormat = EImageFormat::JPEG;
	}

	IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
	TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule.CreateImageWrapper(ImageFormat);
	if (!ImageWrapper.IsValid() || !ImageWrapper->SetCompressed(CompressedData.GetData(), CompressedData.Num()))
	{
		UE_LOG(LogTemp, Warning, TEXT("Glue texture decode failed: %s"), *ResolvedPath);
		return nullptr;
	}

	TArray<uint8> RawData;
	if (!ImageWrapper->GetRaw(ERGBFormat::BGRA, 8, RawData))
	{
		UE_LOG(LogTemp, Warning, TEXT("Glue texture raw conversion failed: %s"), *ResolvedPath);
		return nullptr;
	}

	if (AlphaMode == EWoWGlueAlphaMode::Add || AlphaMode == EWoWGlueAlphaMode::AlphaKey)
	{
		for (int32 Index = 0; Index + 3 < RawData.Num(); Index += 4)
		{
			const uint8 Blue = RawData[Index + 0];
			const uint8 Green = RawData[Index + 1];
			const uint8 Red = RawData[Index + 2];
			const uint8 Luma = FMath::Max3(Red, Green, Blue);
			if (AlphaMode == EWoWGlueAlphaMode::Add)
			{
				const float AddStrength = FMath::Max(0.0f, GGlueRenderPolicy.AddDrawTintMultiplier / DefaultAddDrawTintMultiplier);
				const float EffectiveRgbMultiplier = GGlueRenderPolicy.AddLoadRgbMultiplier * FMath::Sqrt(AddStrength);
				const float EffectiveAlphaMultiplier = GGlueRenderPolicy.AddLoadAlphaMultiplier * AddStrength;
				const uint8 AddAlpha = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(static_cast<float>(Luma) * EffectiveAlphaMultiplier), 0, 255));
				RawData[Index + 0] = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(static_cast<float>(Blue) * EffectiveRgbMultiplier), 0, 255));
				RawData[Index + 1] = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(static_cast<float>(Green) * EffectiveRgbMultiplier), 0, 255));
				RawData[Index + 2] = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(static_cast<float>(Red) * EffectiveRgbMultiplier), 0, 255));
				RawData[Index + 3] = AddAlpha;
			}
			else
			{
				RawData[Index + 3] = Luma > 8 ? 255 : 0;
			}
		}
	}
	else if (AlphaMode == EWoWGlueAlphaMode::Disable || AlphaMode == EWoWGlueAlphaMode::Mod)
	{
		for (int32 Index = 0; Index + 3 < RawData.Num(); Index += 4)
		{
			RawData[Index + 3] = 255;
		}
	}

	UTexture2D* Texture = UTexture2D::CreateTransient(ImageWrapper->GetWidth(), ImageWrapper->GetHeight(), PF_B8G8R8A8);
	if (!Texture || !Texture->GetPlatformData() || Texture->GetPlatformData()->Mips.Num() == 0)
	{
		return nullptr;
	}

	void* TextureData = Texture->GetPlatformData()->Mips[0].BulkData.Lock(LOCK_READ_WRITE);
	FMemory::Memcpy(TextureData, RawData.GetData(), RawData.Num());
	Texture->GetPlatformData()->Mips[0].BulkData.Unlock();
	Texture->UpdateResource();
	Texture->AddToRoot();
	TextureCache.Add(CacheKey, Texture);
	return Texture;
}

void SWoWGlueRoot::Construct(const FArguments& InArgs)
{
	GlueManager = InArgs._GlueManager;
	RootMouseButtonDownHandler = InArgs._OnRootMouseButtonDown;
	RootMouseButtonUpHandler = InArgs._OnRootMouseButtonUp;
	RootMouseMoveHandler = InArgs._OnRootMouseMove;
	Refresh();
}

void SWoWGlueRoot::SetGlueManager(TSharedPtr<FWoWGlueUIManager> InGlueManager)
{
	GlueManager = InGlueManager;
	Refresh();
}

void SWoWGlueRoot::Refresh()
{
	const TSharedRef<SWidget> RootWidget = GlueManager.IsValid()
		? GlueManager->MakeSlateWidget()
		: StaticCastSharedRef<SWidget>(SNew(SBox));

	ChildSlot
	[
		RootWidget
	];
}

FReply SWoWGlueRoot::OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if (RootMouseButtonDownHandler.IsBound())
	{
		FReply Reply = RootMouseButtonDownHandler.Execute(MyGeometry, MouseEvent);
		if (Reply.IsEventHandled())
		{
			return Reply;
		}
	}
	if (GlueManager.IsValid())
	{
		GlueManager->RestoreActiveEditBoxFocusDeferred(AsShared());
		return FReply::Handled();
	}
	return SCompoundWidget::OnMouseButtonDown(MyGeometry, MouseEvent);
}

FReply SWoWGlueRoot::OnMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if (RootMouseButtonUpHandler.IsBound())
	{
		return RootMouseButtonUpHandler.Execute(MyGeometry, MouseEvent);
	}
	return SCompoundWidget::OnMouseButtonUp(MyGeometry, MouseEvent);
}

FReply SWoWGlueRoot::OnMouseMove(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if (RootMouseMoveHandler.IsBound())
	{
		return RootMouseMoveHandler.Execute(MyGeometry, MouseEvent);
	}
	return SCompoundWidget::OnMouseMove(MyGeometry, MouseEvent);
}
