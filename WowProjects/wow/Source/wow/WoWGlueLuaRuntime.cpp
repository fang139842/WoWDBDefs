#include "WoWGlueLuaRuntime.h"

#include "HAL/PlatformProcess.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "XmlFile.h"

#include <string>

extern "C"
{
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

namespace
{
constexpr const char* RuntimeRegistryKey = "WoWGlueLuaRuntime";
constexpr const char* FrameMetatableName = "WoWGlueFrame";
constexpr const char* FrameFieldRegistryKey = "WoWGlueFrameFields";
constexpr const char* FrameMethodsMetatableKey = "__methods";
// GetCharacterInfo 返回值中的 ghost/PCC/PRC/PFC 来自角色枚举包。
// ghost 使用角色 flags；PCC/PRC/PFC 使用 at_login flags，对应官方 Glue 里显示的付费服务按钮。
constexpr uint32 CharacterFlagGhost = 0x00002000; // ghost: 角色幽灵/死亡状态，用于角色列表灰化、死亡提示等表现。
constexpr uint32 AtLoginCustomize = 0x00000008; // PCC: Paid/Pending Character Customize，待完成角色外观自定义。
constexpr uint32 AtLoginChangeFaction = 0x00000040; // PFC: Paid/Pending Faction Change，待完成阵营变更。
constexpr uint32 AtLoginChangeRace = 0x00000080; // PRC: Paid/Pending Race Change，待完成种族变更。

struct FWoWGlueLuaFrameUserData
{
	FWoWGlueFrame* Frame = nullptr;
};

FString NormalizeXmlName(const FString& Value)
{
	FString Result = Value;
	Result.ReplaceInline(TEXT("$parent"), TEXT(""));
	return Result;
}

FString ReplaceXmlParentToken(FString Value, const FString& ParentName)
{
	Value.ReplaceInline(TEXT("$parent"), *ParentName);
	return Value;
}

FString MakeXmlScriptFunctionText(const FString& ScriptName, const FString& Body)
{
	if (ScriptName.Equals(TEXT("OnUpdate"), ESearchCase::IgnoreCase))
	{
		return FString::Printf(TEXT("return function(self, elapsed) %s end"), *Body);
	}
	if (ScriptName.Equals(TEXT("OnEvent"), ESearchCase::IgnoreCase))
	{
		return FString::Printf(TEXT("return function(self, event, ...) %s end"), *Body);
	}
	if (ScriptName.Equals(TEXT("OnHyperlinkClick"), ESearchCase::IgnoreCase))
	{
		return FString::Printf(TEXT("return function(self, link, text, button) %s end"), *Body);
	}
	if (ScriptName.Equals(TEXT("OnKeyDown"), ESearchCase::IgnoreCase))
	{
		return FString::Printf(TEXT("return function(self, key) %s end"), *Body);
	}
	return FString::Printf(TEXT("return function(self, ...) %s end"), *Body);
}

bool IsXmlFrameTag(const FString& Tag)
{
	return Tag.Equals(TEXT("Frame"), ESearchCase::IgnoreCase)
		|| Tag.Equals(TEXT("Button"), ESearchCase::IgnoreCase)
		|| Tag.Equals(TEXT("EditBox"), ESearchCase::IgnoreCase)
		|| Tag.Equals(TEXT("FontString"), ESearchCase::IgnoreCase)
		|| Tag.Equals(TEXT("SimpleHTML"), ESearchCase::IgnoreCase)
		|| Tag.Equals(TEXT("Texture"), ESearchCase::IgnoreCase)
		|| Tag.Equals(TEXT("ScrollFrame"), ESearchCase::IgnoreCase);
}

bool ReadLittleEndianUInt32(const TArray<uint8>& Data, int32 Offset, uint32& OutValue)
{
	if (!Data.IsValidIndex(Offset) || Offset + 4 > Data.Num())
	{
		return false;
	}
	OutValue = static_cast<uint32>(Data[Offset])
		| (static_cast<uint32>(Data[Offset + 1]) << 8)
		| (static_cast<uint32>(Data[Offset + 2]) << 16)
		| (static_cast<uint32>(Data[Offset + 3]) << 24);
	return true;
}

FString ReadDbcString(const TArray<uint8>& Data, int32 StringBlockStart, uint32 StringOffset)
{
	const int32 Start = StringBlockStart + static_cast<int32>(StringOffset);
	if (StringOffset == 0 || Start < StringBlockStart || Start >= Data.Num())
	{
		return FString();
	}

	int32 End = Start;
	while (End < Data.Num() && Data[End] != 0)
	{
		++End;
	}
	if (End <= Start)
	{
		return FString();
	}

	const std::string Utf8String(reinterpret_cast<const char*>(Data.GetData() + Start), End - Start);
	return FString(UTF8_TO_TCHAR(Utf8String.c_str()));
}

bool LoadDbcLocalizedNameMap(const FString& RelativeDbcPath, int32 NameFieldIndex, TMap<int32, FString>& OutNames)
{
	OutNames.Reset();

	const TArray<FString> Candidates =
	{
		FPaths::ProjectDir() / TEXT("Data") / RelativeDbcPath,
		FPaths::ProjectDir() / TEXT("Data/DBFilesClient") / FPaths::GetCleanFilename(RelativeDbcPath)
	};

	FString LoadedPath;
	TArray<uint8> Data;
	for (const FString& Candidate : Candidates)
	{
		if (FPaths::FileExists(Candidate) && FFileHelper::LoadFileToArray(Data, *Candidate))
		{
			LoadedPath = Candidate;
			break;
		}
	}

	if (Data.Num() < 20)
	{
		UE_LOG(LogTemp, Warning, TEXT("[GlueLua] DBC not found or too small: %s"), *RelativeDbcPath);
		return false;
	}

	if (Data[0] != 'W' || Data[1] != 'D' || Data[2] != 'B' || Data[3] != 'C')
	{
		UE_LOG(LogTemp, Warning, TEXT("[GlueLua] unsupported DBC magic: %s"), *LoadedPath);
		return false;
	}

	uint32 RecordCount = 0;
	uint32 FieldCount = 0;
	uint32 RecordSize = 0;
	uint32 StringBlockSize = 0;
	ReadLittleEndianUInt32(Data, 4, RecordCount);
	ReadLittleEndianUInt32(Data, 8, FieldCount);
	ReadLittleEndianUInt32(Data, 12, RecordSize);
	ReadLittleEndianUInt32(Data, 16, StringBlockSize);

	if (RecordCount == 0 || FieldCount == 0 || RecordSize == 0 || NameFieldIndex < 0 || NameFieldIndex >= static_cast<int32>(FieldCount))
	{
		UE_LOG(LogTemp, Warning, TEXT("[GlueLua] invalid DBC header: %s records=%u fields=%u size=%u nameField=%d"),
			*LoadedPath,
			RecordCount,
			FieldCount,
			RecordSize,
			NameFieldIndex);
		return false;
	}

	const int32 RecordsStart = 20;
	const int32 StringBlockStart = RecordsStart + static_cast<int32>(RecordCount * RecordSize);
	if (StringBlockStart > Data.Num() || StringBlockStart + static_cast<int32>(StringBlockSize) > Data.Num())
	{
		UE_LOG(LogTemp, Warning, TEXT("[GlueLua] DBC size mismatch: %s"), *LoadedPath);
		return false;
	}

	for (uint32 RecordIndex = 0; RecordIndex < RecordCount; ++RecordIndex)
	{
		const int32 RecordOffset = RecordsStart + static_cast<int32>(RecordIndex * RecordSize);
		uint32 Id = 0;
		uint32 NameOffset = 0;
		if (ReadLittleEndianUInt32(Data, RecordOffset, Id)
			&& ReadLittleEndianUInt32(Data, RecordOffset + NameFieldIndex * 4, NameOffset))
		{
			const FString Name = ReadDbcString(Data, StringBlockStart, NameOffset);
			if (Id > 0 && !Name.IsEmpty())
			{
				OutNames.Add(static_cast<int32>(Id), Name);
			}
		}
	}

	UE_LOG(LogTemp, Display, TEXT("[GlueLua] DBC loaded: %s names=%d"), *LoadedPath, OutNames.Num());
	return OutNames.Num() > 0;
}

FString GetXmlAttr(const FWoWGlueXmlObjectNode& Node, const FString& Name, const FString& DefaultValue = FString())
{
	if (const FString* Found = Node.Attributes.Find(Name))
	{
		return *Found;
	}
	return DefaultValue;
}

bool GetXmlBoolAttr(const FWoWGlueXmlObjectNode& Node, const FString& Name, bool bDefaultValue = false)
{
	const FString Value = GetXmlAttr(Node, Name);
	if (Value.IsEmpty())
	{
		return bDefaultValue;
	}
	return Value.Equals(TEXT("true"), ESearchCase::IgnoreCase) || Value == TEXT("1");
}

float GetXmlFloatAttr(const FWoWGlueXmlObjectNode& Node, const FString& Name, float DefaultValue = 0.0f)
{
	const FString Value = GetXmlAttr(Node, Name);
	return Value.IsEmpty() ? DefaultValue : FCString::Atof(*Value);
}

const FWoWGlueXmlObjectNode* FindXmlChild(const FWoWGlueXmlObjectNode& Node, const FString& Tag)
{
	for (const TSharedPtr<FWoWGlueXmlObjectNode>& Child : Node.Children)
	{
		if (Child.IsValid() && Child->Tag.Equals(Tag, ESearchCase::IgnoreCase))
		{
			return Child.Get();
		}
	}
	return nullptr;
}

void CopyXmlNode(const FXmlNode& Source, FWoWGlueXmlObjectNode& Dest)
{
	Dest.Tag = Source.GetTag();
	Dest.Content = Source.GetContent();
	for (const FXmlAttribute& Attribute : Source.GetAttributes())
	{
		Dest.Attributes.Add(Attribute.GetTag(), Attribute.GetValue());
	}
	for (const FXmlNode* Child : Source.GetChildrenNodes())
	{
		if (!Child)
		{
			continue;
		}
		TSharedPtr<FWoWGlueXmlObjectNode> ChildNode = MakeShared<FWoWGlueXmlObjectNode>();
		CopyXmlNode(*Child, *ChildNode);
		Dest.Children.Add(ChildNode);
	}
}

bool ReadXmlAbsDimension(const FWoWGlueXmlObjectNode& Node, float& OutX, float& OutY)
{
	if (const FWoWGlueXmlObjectNode* AbsDimension = FindXmlChild(Node, TEXT("AbsDimension")))
	{
		OutX = GetXmlFloatAttr(*AbsDimension, TEXT("x"), OutX);
		OutY = GetXmlFloatAttr(*AbsDimension, TEXT("y"), OutY);
		return true;
	}
	return false;
}

bool ReadXmlAbsInset(const FWoWGlueXmlObjectNode& Node, FMargin& OutInsets)
{
	if (const FWoWGlueXmlObjectNode* AbsInset = FindXmlChild(Node, TEXT("AbsInset")))
	{
		OutInsets.Left = GetXmlFloatAttr(*AbsInset, TEXT("left"), OutInsets.Left);
		OutInsets.Right = GetXmlFloatAttr(*AbsInset, TEXT("right"), OutInsets.Right);
		OutInsets.Top = GetXmlFloatAttr(*AbsInset, TEXT("top"), OutInsets.Top);
		OutInsets.Bottom = GetXmlFloatAttr(*AbsInset, TEXT("bottom"), OutInsets.Bottom);
		return true;
	}
	return false;
}

float ReadXmlAbsValue(const FWoWGlueXmlObjectNode& Node, float DefaultValue)
{
	if (const FWoWGlueXmlObjectNode* AbsValue = FindXmlChild(Node, TEXT("AbsValue")))
	{
		return GetXmlFloatAttr(*AbsValue, TEXT("val"), DefaultValue);
	}
	return DefaultValue;
}

bool ReadXmlTexCoords(const FWoWGlueXmlObjectNode& Node, FWoWGlueUVRect& OutTexCoords)
{
	if (const FWoWGlueXmlObjectNode* TexCoords = FindXmlChild(Node, TEXT("TexCoords")))
	{
		// 官方 XML 写法是 left/right/top/bottom，运行时只负责忠实传递这组 UV。
		OutTexCoords.Left = GetXmlFloatAttr(*TexCoords, TEXT("left"), OutTexCoords.Left);
		OutTexCoords.Right = GetXmlFloatAttr(*TexCoords, TEXT("right"), OutTexCoords.Right);
		OutTexCoords.Top = GetXmlFloatAttr(*TexCoords, TEXT("top"), OutTexCoords.Top);
		OutTexCoords.Bottom = GetXmlFloatAttr(*TexCoords, TEXT("bottom"), OutTexCoords.Bottom);
		OutTexCoords.bValid = true;
		return true;
	}
	return false;
}

int32 XmlFrameStrataToDrawLayer(const FString& FrameStrata)
{
	FString Normalized = FrameStrata;
	Normalized.TrimStartAndEndInline();
	Normalized.ToUpperInline();
	if (Normalized == TEXT("BACKGROUND"))
	{
		return -1000;
	}
	if (Normalized == TEXT("LOW"))
	{
		return 0;
	}
	if (Normalized == TEXT("MEDIUM"))
	{
		return 1000;
	}
	if (Normalized == TEXT("HIGH"))
	{
		return 2000;
	}
	if (Normalized == TEXT("DIALOG"))
	{
		return 10000;
	}
	if (Normalized == TEXT("FULLSCREEN"))
	{
		return 20000;
	}
	if (Normalized == TEXT("FULLSCREEN_DIALOG"))
	{
		return 30000;
	}
	if (Normalized == TEXT("TOOLTIP"))
	{
		return 40000;
	}
	return 0;
}

int32 XmlLayerLevelToDrawLayer(const FString& LayerLevel)
{
	FString Normalized = LayerLevel;
	Normalized.TrimStartAndEndInline();
	Normalized.ToUpperInline();
	if (Normalized == TEXT("BACKGROUND"))
	{
		return -30;
	}
	if (Normalized == TEXT("BORDER"))
	{
		return -10;
	}
	if (Normalized == TEXT("ARTWORK"))
	{
		return 10;
	}
	if (Normalized == TEXT("OVERLAY"))
	{
		return 20;
	}
	if (Normalized == TEXT("HIGHLIGHT"))
	{
		return 30;
	}
	return 0;
}

FString SanitizeWoWGlueXmlForUnrealParser(FString XmlText)
{
	const int32 UiTagStart = XmlText.Find(TEXT("<Ui"), ESearchCase::IgnoreCase);
	if (UiTagStart == INDEX_NONE)
	{
		return XmlText;
	}

	const int32 UiTagEnd = XmlText.Find(TEXT(">"), ESearchCase::CaseSensitive, ESearchDir::FromStart, UiTagStart);
	if (UiTagEnd == INDEX_NONE)
	{
		return XmlText;
	}

	const FString UiOpenTag = XmlText.Mid(UiTagStart, UiTagEnd - UiTagStart + 1);
	const int32 AttributeStartInTag = UiOpenTag.Find(TEXT("xsi:schemaLocation="), ESearchCase::CaseSensitive);
	if (AttributeStartInTag == INDEX_NONE)
	{
		return XmlText;
	}

	const int32 AttributeStart = UiTagStart + AttributeStartInTag;
	const int32 QuoteStart = XmlText.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, AttributeStart);
	if (QuoteStart == INDEX_NONE || QuoteStart > UiTagEnd)
	{
		return XmlText;
	}

	const int32 QuoteEnd = XmlText.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, QuoteStart + 1);
	if (QuoteEnd == INDEX_NONE)
	{
		return XmlText;
	}

	int32 RemoveStart = AttributeStart;
	while (RemoveStart > 0 && FChar::IsWhitespace(XmlText[RemoveStart - 1]))
	{
		--RemoveStart;
	}
	XmlText.RemoveAt(RemoveStart, QuoteEnd - RemoveStart + 1, EAllowShrinking::No);
	return XmlText;
}

void OpenGlueLuaLibraries(lua_State* L)
{
	lua_pushcfunction(L, luaopen_base);
	lua_pushstring(L, "");
	lua_call(L, 1, 0);

	lua_pushcfunction(L, luaopen_table);
	lua_pushstring(L, LUA_TABLIBNAME);
	lua_call(L, 1, 0);

	lua_pushcfunction(L, luaopen_string);
	lua_pushstring(L, LUA_STRLIBNAME);
	lua_call(L, 1, 0);

	lua_pushcfunction(L, luaopen_math);
	lua_pushstring(L, LUA_MATHLIBNAME);
	lua_call(L, 1, 0);
}

int32 LuaError(lua_State* L, const TCHAR* Message)
{
	lua_pushstring(L, TCHAR_TO_UTF8(Message));
	return lua_error(L);
}

float LuaOptionalNumberNoThrow(lua_State* L, int32 StackIndex, float DefaultValue, const TCHAR* Context)
{
	if (lua_isnoneornil(L, StackIndex))
	{
		return DefaultValue;
	}
	if (!lua_isnumber(L, StackIndex))
	{
		const FString ActualValue = FString(UTF8_TO_TCHAR(luaL_typename(L, StackIndex)));
		UE_LOG(LogTemp, Warning, TEXT("[GlueLua] %s expected number at arg %d, got %s; using %.2f"),
			Context,
			StackIndex,
			*ActualValue,
			DefaultValue);
		return DefaultValue;
	}
	return static_cast<float>(lua_tonumber(L, StackIndex));
}

EWoWGlueAlphaMode ParseAlphaMode(lua_State* L, int32 StackIndex, EWoWGlueAlphaMode DefaultMode)
{
	if (lua_isnoneornil(L, StackIndex))
	{
		return DefaultMode;
	}

	size_t Length = 0;
	const char* Utf8 = lua_tolstring(L, StackIndex, &Length);
	FString Value = Utf8 ? FString(UTF8_TO_TCHAR(Utf8)) : FString();
	Value.TrimStartAndEndInline();
	Value.ToUpperInline();
	if (Value == TEXT("ADD"))
	{
		return EWoWGlueAlphaMode::Add;
	}
	if (Value == TEXT("ALPHAKEY"))
	{
		return EWoWGlueAlphaMode::AlphaKey;
	}
	if (Value == TEXT("DISABLE"))
	{
		return EWoWGlueAlphaMode::Disable;
	}
	if (Value == TEXT("MOD"))
	{
		return EWoWGlueAlphaMode::Mod;
	}
	return EWoWGlueAlphaMode::Blend;
}

float ReadLuaNumberField(lua_State* L, int32 TableIndex, const char* FieldName, float DefaultValue)
{
	lua_getfield(L, TableIndex, FieldName);
	const float Value = lua_isnumber(L, -1) ? static_cast<float>(lua_tonumber(L, -1)) : DefaultValue;
	lua_pop(L, 1);
	return Value;
}

FString ReadLuaStringField(lua_State* L, int32 TableIndex, const char* FieldName)
{
	lua_getfield(L, TableIndex, FieldName);
	FString Value;
	if (lua_isstring(L, -1))
	{
		size_t Length = 0;
		const char* Utf8 = lua_tolstring(L, -1, &Length);
		Value = Utf8 ? FString(UTF8_TO_TCHAR(Utf8)) : FString();
	}
	lua_pop(L, 1);
	return Value;
}

FWoWGlueUVRect ReadLuaUVRect(lua_State* L, int32 TableIndex, const char* FieldName)
{
	FWoWGlueUVRect UVRect;
	lua_getfield(L, TableIndex, FieldName);
	if (lua_istable(L, -1))
	{
		lua_rawgeti(L, -1, 1);
		const float Left = lua_isnumber(L, -1) ? static_cast<float>(lua_tonumber(L, -1)) : 0.0f;
		lua_pop(L, 1);

		lua_rawgeti(L, -1, 2);
		const float Right = lua_isnumber(L, -1) ? static_cast<float>(lua_tonumber(L, -1)) : 1.0f;
		lua_pop(L, 1);

		lua_rawgeti(L, -1, 3);
		const float Top = lua_isnumber(L, -1) ? static_cast<float>(lua_tonumber(L, -1)) : 0.0f;
		lua_pop(L, 1);

		lua_rawgeti(L, -1, 4);
		const float Bottom = lua_isnumber(L, -1) ? static_cast<float>(lua_tonumber(L, -1)) : 1.0f;
		lua_pop(L, 1);

		UVRect.Left = Left;
		UVRect.Right = Right;
		UVRect.Top = Top;
		UVRect.Bottom = Bottom;
		UVRect.bValid = true;
	}
	lua_pop(L, 1);
	return UVRect;
}

void ReadLuaScrollBarVisualState(lua_State* L, int32 TableIndex, const char* FieldName, FWoWGlueScrollBarVisualState& OutState)
{
	lua_getfield(L, TableIndex, FieldName);
	if (lua_istable(L, -1))
	{
		const int32 StateIndex = lua_gettop(L);
		OutState.ThumbTop = ReadLuaUVRect(L, StateIndex, "thumbTop");
		OutState.ThumbMiddle = ReadLuaUVRect(L, StateIndex, "thumbMiddle");
		OutState.ThumbBottom = ReadLuaUVRect(L, StateIndex, "thumbBottom");
		OutState.UpArrow = ReadLuaUVRect(L, StateIndex, "upArrow");
		OutState.DownArrow = ReadLuaUVRect(L, StateIndex, "downArrow");
	}
	lua_pop(L, 1);
}
}

FWoWGlueLuaRuntime::FWoWGlueLuaRuntime()
{
}

FWoWGlueLuaRuntime::~FWoWGlueLuaRuntime()
{
	Shutdown();
}

void FWoWGlueLuaRuntime::Initialize(TSharedRef<FWoWGlueUIManager> InGlueManager)
{
	Shutdown();
	GlueManager = InGlueManager;
	LuaState = luaL_newstate();
	check(LuaState);

	OpenGlueLuaLibraries(LuaState);
	lua_pushlightuserdata(LuaState, this);
	lua_setfield(LuaState, LUA_REGISTRYINDEX, RuntimeRegistryKey);

	LoadGlueDbcStores();
	RegisterFrameMetatable();
	RegisterGlobals();
	PushFrame(LuaState, GlueManager->GetUIParent());
	lua_setglobal(LuaState, "UIParent");
}

bool FWoWGlueLuaRuntime::LoadFile(const FString& ScriptPath, FString* OutError)
{
	if (!LuaState)
	{
		if (OutError)
		{
			*OutError = TEXT("Lua runtime is not initialized.");
		}
		return false;
	}

	FString ScriptText;
	if (!FFileHelper::LoadFileToString(ScriptText, *ScriptPath))
	{
		if (OutError)
		{
			*OutError = FString::Printf(TEXT("Could not read Lua file: %s"), *ScriptPath);
		}
		return false;
	}

	FTCHARToUTF8 ScriptUtf8(*ScriptText);
	const int32 LoadResult = luaL_loadbuffer(LuaState, ScriptUtf8.Get(), ScriptUtf8.Length(), TCHAR_TO_UTF8(*ScriptPath));
	if (LoadResult != 0)
	{
		if (OutError)
		{
			*OutError = ToFString(LuaState, -1);
		}
		lua_pop(LuaState, 1);
		return false;
	}

	const int32 CallResult = lua_pcall(LuaState, 0, 0, 0);
	if (CallResult != 0)
	{
		if (OutError)
		{
			*OutError = ToFString(LuaState, -1);
		}
		lua_pop(LuaState, 1);
		return false;
	}

	return true;
}

bool FWoWGlueLuaRuntime::LoadXmlScriptFile(const FString& XmlPath, const FString& ScriptFile, FString* OutError)
{
	const FString ScriptPath = FPaths::GetPath(XmlPath) / ScriptFile;
	return LoadFile(ScriptPath, OutError);
}

void FWoWGlueLuaRuntime::RegisterXmlTemplate(const FString& TemplateName, const FWoWGlueXmlObjectNode& Node)
{
	if (TemplateName.IsEmpty())
	{
		return;
	}
	TSharedPtr<FWoWGlueXmlTemplateInfo> Template = MakeShared<FWoWGlueXmlTemplateInfo>();
	Template->Node = Node;
	XmlTemplates.Add(TemplateName, Template);
}

FString FWoWGlueLuaRuntime::ResolveXmlInheritedTexturePath(const FString& TemplateName) const
{
	const TSharedPtr<FWoWGlueXmlTemplateInfo>* Template = XmlTemplates.Find(TemplateName);
	if (!Template || !Template->IsValid())
	{
		return FString();
	}
	return GetXmlAttr((*Template)->Node, TEXT("file"));
}

bool FWoWGlueLuaRuntime::BindXmlFrameScript(TSharedRef<FWoWGlueFrame> Frame, const FString& ScriptName, const FString& Body)
{
	if (!LuaState || Body.TrimStartAndEnd().IsEmpty())
	{
		return true;
	}

	const FString ScriptText = MakeXmlScriptFunctionText(ScriptName, Body.TrimStartAndEnd());
	FTCHARToUTF8 ScriptUtf8(*ScriptText);
	if (luaL_loadbuffer(LuaState, ScriptUtf8.Get(), ScriptUtf8.Length(), TCHAR_TO_UTF8(*ScriptName)) != 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("[GlueXML] script compile failed for %s.%s: %s"), *Frame->GetName().ToString(), *ScriptName, *ToFString(LuaState, -1));
		lua_pop(LuaState, 1);
		return false;
	}

	if (lua_pcall(LuaState, 0, 1, 0) != 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("[GlueXML] script load failed for %s.%s: %s"), *Frame->GetName().ToString(), *ScriptName, *ToFString(LuaState, -1));
		lua_pop(LuaState, 1);
		return false;
	}

	const int32 FunctionIndex = lua_gettop(LuaState);
	FFrameScriptRef& ScriptRef = GetFrameScriptRef(Frame.Get());
	TSharedPtr<FWoWGlueFrame> SharedFrame = GlueManager.IsValid() ? GlueManager->FindFrame(Frame->GetName().ToString()) : nullptr;

	auto StoreRef = [this, FunctionIndex](int32& Ref)
	{
		ReleaseLuaFunctionRef(Ref);
		Ref = StoreLuaFunctionRef(LuaState, FunctionIndex);
	};

	if (ScriptName.Equals(TEXT("OnLoad"), ESearchCase::IgnoreCase))
	{
		StoreRef(ScriptRef.OnLoadRef);
	}
	else if (ScriptName.Equals(TEXT("OnClick"), ESearchCase::IgnoreCase))
	{
		StoreRef(ScriptRef.OnClickRef);
		if (SharedFrame.IsValid())
		{
			Frame->SetScript(TEXT("OnClick"), [this](TSharedRef<FWoWGlueFrame> ClickedFrame)
			{
				FFrameScriptRef& Ref = GetFrameScriptRef(ClickedFrame.Get());
				InvokeFrameScript(ClickedFrame, Ref.OnClickRef);
			});
		}
	}
	else if (ScriptName.Equals(TEXT("OnDoubleClick"), ESearchCase::IgnoreCase))
	{
		StoreRef(ScriptRef.OnDoubleClickRef);
		if (SharedFrame.IsValid())
		{
			Frame->SetScript(TEXT("OnDoubleClick"), [this](TSharedRef<FWoWGlueFrame> ClickedFrame)
			{
				FFrameScriptRef& Ref = GetFrameScriptRef(ClickedFrame.Get());
				InvokeFrameScript(ClickedFrame, Ref.OnDoubleClickRef);
			});
		}
	}
	else if (ScriptName.Equals(TEXT("OnTextChanged"), ESearchCase::IgnoreCase))
	{
		StoreRef(ScriptRef.OnTextChangedRef);
		if (SharedFrame.IsValid())
		{
			Frame->SetScript(TEXT("OnTextChanged"), [this](TSharedRef<FWoWGlueFrame> ChangedFrame)
			{
				FFrameScriptRef& Ref = GetFrameScriptRef(ChangedFrame.Get());
				InvokeFrameScript(ChangedFrame, Ref.OnTextChangedRef);
			});
		}
	}
	else if (ScriptName.Equals(TEXT("OnEditFocusGained"), ESearchCase::IgnoreCase))
	{
		StoreRef(ScriptRef.OnEditFocusGainedRef);
		if (SharedFrame.IsValid())
		{
			Frame->SetScript(TEXT("OnEditFocusGained"), [this](TSharedRef<FWoWGlueFrame> FocusedFrame)
			{
				FFrameScriptRef& Ref = GetFrameScriptRef(FocusedFrame.Get());
				InvokeFrameScript(FocusedFrame, Ref.OnEditFocusGainedRef);
			});
		}
	}
	else if (ScriptName.Equals(TEXT("OnEditFocusLost"), ESearchCase::IgnoreCase))
	{
		StoreRef(ScriptRef.OnEditFocusLostRef);
		if (SharedFrame.IsValid())
		{
			Frame->SetScript(TEXT("OnEditFocusLost"), [this](TSharedRef<FWoWGlueFrame> FocusedFrame)
			{
				FFrameScriptRef& Ref = GetFrameScriptRef(FocusedFrame.Get());
				InvokeFrameScript(FocusedFrame, Ref.OnEditFocusLostRef);
			});
		}
	}
	else if (ScriptName.Equals(TEXT("OnTabPressed"), ESearchCase::IgnoreCase))
	{
		StoreRef(ScriptRef.OnTabPressedRef);
		if (SharedFrame.IsValid())
		{
			Frame->SetScript(TEXT("OnTabPressed"), [this](TSharedRef<FWoWGlueFrame> FocusedFrame)
			{
				FFrameScriptRef& Ref = GetFrameScriptRef(FocusedFrame.Get());
				InvokeFrameScript(FocusedFrame, Ref.OnTabPressedRef);
			});
		}
	}
	else if (ScriptName.Equals(TEXT("OnEnter"), ESearchCase::IgnoreCase))
	{
		StoreRef(ScriptRef.OnEnterRef);
		if (SharedFrame.IsValid())
		{
			Frame->SetScript(TEXT("OnEnter"), [this](TSharedRef<FWoWGlueFrame> EnteredFrame)
			{
				FFrameScriptRef& Ref = GetFrameScriptRef(EnteredFrame.Get());
				InvokeFrameScript(EnteredFrame, Ref.OnEnterRef);
			});
		}
	}
	else if (ScriptName.Equals(TEXT("OnLeave"), ESearchCase::IgnoreCase))
	{
		StoreRef(ScriptRef.OnLeaveRef);
		if (SharedFrame.IsValid())
		{
			Frame->SetScript(TEXT("OnLeave"), [this](TSharedRef<FWoWGlueFrame> LeftFrame)
			{
				FFrameScriptRef& Ref = GetFrameScriptRef(LeftFrame.Get());
				InvokeFrameScript(LeftFrame, Ref.OnLeaveRef);
			});
		}
	}
	else if (ScriptName.Equals(TEXT("OnUpdate"), ESearchCase::IgnoreCase))
	{
		StoreRef(ScriptRef.OnUpdateRef);
	}
	else if (ScriptName.Equals(TEXT("OnShow"), ESearchCase::IgnoreCase))
	{
		StoreRef(ScriptRef.OnShowRef);
	}
	else if (ScriptName.Equals(TEXT("OnHide"), ESearchCase::IgnoreCase))
	{
		StoreRef(ScriptRef.OnHideRef);
	}
	else if (ScriptName.Equals(TEXT("OnEvent"), ESearchCase::IgnoreCase))
	{
		StoreRef(ScriptRef.OnEventRef);
	}
	else if (ScriptName.Equals(TEXT("OnKeyDown"), ESearchCase::IgnoreCase))
	{
		StoreRef(ScriptRef.OnKeyDownRef);
	}
	else if (ScriptName.Equals(TEXT("OnEnterPressed"), ESearchCase::IgnoreCase))
	{
		StoreRef(ScriptRef.OnEnterPressedRef);
		if (SharedFrame.IsValid())
		{
			Frame->SetScript(TEXT("OnEnterPressed"), [this](TSharedRef<FWoWGlueFrame> FocusedFrame)
			{
				FFrameScriptRef& Ref = GetFrameScriptRef(FocusedFrame.Get());
				InvokeFrameScript(FocusedFrame, Ref.OnEnterPressedRef);
			});
		}
	}
	else if (ScriptName.Equals(TEXT("OnEscapePressed"), ESearchCase::IgnoreCase))
	{
		StoreRef(ScriptRef.OnEscapePressedRef);
		if (SharedFrame.IsValid())
		{
			Frame->SetScript(TEXT("OnEscapePressed"), [this](TSharedRef<FWoWGlueFrame> FocusedFrame)
			{
				FFrameScriptRef& Ref = GetFrameScriptRef(FocusedFrame.Get());
				InvokeFrameScript(FocusedFrame, Ref.OnEscapePressedRef);
			});
		}
	}
	else if (ScriptName.Equals(TEXT("OnDisable"), ESearchCase::IgnoreCase))
	{
		StoreRef(ScriptRef.OnDisableRef);
	}
	else if (ScriptName.Equals(TEXT("OnEnable"), ESearchCase::IgnoreCase))
	{
		StoreRef(ScriptRef.OnEnableRef);
	}
	else if (ScriptName.Equals(TEXT("OnHyperlinkClick"), ESearchCase::IgnoreCase))
	{
		StoreRef(ScriptRef.OnHyperlinkClickRef);
		if (SharedFrame.IsValid())
		{
			Frame->SetHyperlinkScript([this](TSharedRef<FWoWGlueFrame> ClickedFrame, const FString& Link, const FString& Text)
			{
				FFrameScriptRef& Ref = GetFrameScriptRef(ClickedFrame.Get());
				InvokeFrameScriptWithHyperlink(ClickedFrame, Ref.OnHyperlinkClickRef, Link, Text);
			});
		}
	}
	else
	{
		UE_LOG(LogTemp, Verbose, TEXT("[GlueXML] unsupported script ignored for now: %s.%s"), *Frame->GetName().ToString(), *ScriptName);
	}

	lua_pop(LuaState, 1);
	return true;
}

void FWoWGlueLuaRuntime::InvokeFrameOnLoad(TSharedRef<FWoWGlueFrame> Frame)
{
	FFrameScriptRef& Ref = GetFrameScriptRef(Frame.Get());
	InvokeFrameScript(Frame, Ref.OnLoadRef);
}

bool FWoWGlueLuaRuntime::ApplyXmlTemplate(const FString& TemplateName, TSharedRef<FWoWGlueFrame> Frame, FString* OutError)
{
	const TSharedPtr<FWoWGlueXmlTemplateInfo>* Template = XmlTemplates.Find(TemplateName);
	if (!Template || !Template->IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("[GlueXML] template not found: %s"), *TemplateName);
		return true;
	}
	return ApplyXmlObjectBody((*Template)->Node, Frame, OutError);
}

bool FWoWGlueLuaRuntime::ApplyXmlObjectBody(const FWoWGlueXmlObjectNode& Node, TSharedRef<FWoWGlueFrame> Frame, FString* OutError)
{
	const FString ParentName = Frame->GetName().ToString();

	const FString Inherits = GetXmlAttr(Node, TEXT("inherits"));
	if (!Inherits.IsEmpty())
	{
		TArray<FString> TemplateNames;
		Inherits.ParseIntoArray(TemplateNames, TEXT(","), true);
		for (FString& TemplateName : TemplateNames)
		{
			TemplateName.TrimStartAndEndInline();
			if (!TemplateName.IsEmpty())
			{
				const TSharedPtr<FWoWGlueXmlTemplateInfo>* Template = XmlTemplates.Find(TemplateName);
				if (Template && Template->IsValid())
				{
					ApplyXmlTemplate(TemplateName, Frame, OutError);
				}
				else if (Node.Tag.Equals(TEXT("FontString"), ESearchCase::IgnoreCase)
					|| Node.Tag.Equals(TEXT("SimpleHTML"), ESearchCase::IgnoreCase))
				{
					Frame->SetFontObject(TemplateName);
				}
				else
				{
					ApplyXmlTemplate(TemplateName, Frame, OutError);
				}
			}
		}
	}

	if (GetXmlBoolAttr(Node, TEXT("hidden"), false))
	{
		Frame->Hide();
	}
	else if (GetXmlBoolAttr(Node, TEXT("hidden"), true) == false)
	{
		Frame->Show();
	}

	const FString FrameStrata = GetXmlAttr(Node, TEXT("frameStrata"));
	if (!FrameStrata.IsEmpty())
	{
		Frame->SetDrawLayer(XmlFrameStrataToDrawLayer(FrameStrata));
	}

	if (GetXmlBoolAttr(Node, TEXT("setAllPoints"), false) && GlueManager.IsValid())
	{
		Frame->SetAllPoints(true);
	}

	if (!GetXmlAttr(Node, TEXT("file")).IsEmpty())
	{
		const FString AlphaModeText = GetXmlAttr(Node, TEXT("alphaMode"));
		EWoWGlueAlphaMode AlphaMode = EWoWGlueAlphaMode::Blend;
		if (AlphaModeText.Equals(TEXT("ADD"), ESearchCase::IgnoreCase))
		{
			AlphaMode = EWoWGlueAlphaMode::Add;
		}
		Frame->SetTexture(GlueManager->LoadTexture(GetXmlAttr(Node, TEXT("file")), AlphaMode), GetXmlAttr(Node, TEXT("file")), AlphaMode);
	}
	FWoWGlueUVRect XmlTexCoords;
	if (ReadXmlTexCoords(Node, XmlTexCoords))
	{
		Frame->SetTexCoord(XmlTexCoords.Left, XmlTexCoords.Right, XmlTexCoords.Top, XmlTexCoords.Bottom);
	}

	if (const FWoWGlueXmlObjectNode* Size = FindXmlChild(Node, TEXT("Size")))
	{
		float Width = Frame->GetSize().X;
		float Height = Frame->GetSize().Y;
		if (ReadXmlAbsDimension(*Size, Width, Height))
		{
			Frame->SetSize(Width, Height);
		}
	}

	if (const FWoWGlueXmlObjectNode* Anchors = FindXmlChild(Node, TEXT("Anchors")))
	{
		if (const FWoWGlueXmlObjectNode* Anchor = FindXmlChild(*Anchors, TEXT("Anchor")))
		{
			float X = 0.0f;
			float Y = 0.0f;
			if (const FWoWGlueXmlObjectNode* Offset = FindXmlChild(*Anchor, TEXT("Offset")))
			{
				ReadXmlAbsDimension(*Offset, X, Y);
			}
			const FString Point = GetXmlAttr(*Anchor, TEXT("point"), TEXT("CENTER"));
			const FString RelativePoint = GetXmlAttr(*Anchor, TEXT("relativePoint"), Point);
			const FString RelativeToName = ReplaceXmlParentToken(GetXmlAttr(*Anchor, TEXT("relativeTo")), ParentName);
			TSharedPtr<FWoWGlueFrame> RelativeTo = RelativeToName.IsEmpty() ? Frame->GetParent() : GlueManager->FindFrame(RelativeToName);
			Frame->SetPoint(Point, RelativeTo, RelativePoint, X, Y);
		}
	}

	if (const FWoWGlueXmlObjectNode* Backdrop = FindXmlChild(Node, TEXT("Backdrop")))
	{
		const FString BackgroundPath = GetXmlAttr(*Backdrop, TEXT("bgFile"));
		const FString EdgePath = GetXmlAttr(*Backdrop, TEXT("edgeFile"));
		float EdgeSize = 16.0f;
		FMargin Insets(10.0f, 4.0f, 5.0f, 9.0f);
		if (const FWoWGlueXmlObjectNode* EdgeSizeNode = FindXmlChild(*Backdrop, TEXT("EdgeSize")))
		{
			EdgeSize = ReadXmlAbsValue(*EdgeSizeNode, EdgeSize);
		}
		if (const FWoWGlueXmlObjectNode* InsetsNode = FindXmlChild(*Backdrop, TEXT("BackgroundInsets")))
		{
			ReadXmlAbsInset(*InsetsNode, Insets);
		}
		Frame->SetBackdrop(
			BackgroundPath.IsEmpty() ? nullptr : GlueManager->LoadTexture(BackgroundPath),
			BackgroundPath,
			EdgePath.IsEmpty() ? nullptr : GlueManager->LoadTexture(EdgePath),
			EdgePath,
			EdgeSize,
			Insets);
	}

	Frame->SetMouseEnabled(GetXmlBoolAttr(Node, TEXT("enableMouse"), false));

	if (const FWoWGlueXmlObjectNode* ButtonText = FindXmlChild(Node, TEXT("ButtonText")))
	{
		if (const FWoWGlueXmlObjectNode* Anchors = FindXmlChild(*ButtonText, TEXT("Anchors")))
		{
			if (const FWoWGlueXmlObjectNode* Anchor = FindXmlChild(*Anchors, TEXT("Anchor")))
			{
				Frame->SetJustifyH(GetXmlAttr(*Anchor, TEXT("point"), TEXT("CENTER")));
			}
		}
	}

	if (Node.Tag.Equals(TEXT("SimpleHTML"), ESearchCase::IgnoreCase))
	{
		if (const FWoWGlueXmlObjectNode* HtmlFont = FindXmlChild(Node, TEXT("FontString")))
		{
			const FString SpacingAttr = GetXmlAttr(*HtmlFont, TEXT("spacing"));
			if (!SpacingAttr.IsEmpty())
			{
				Frame->SetSimpleHTMLLineSpacing(static_cast<float>(FCString::Atof(*SpacingAttr)));
			}
		}
	}

	if (const FWoWGlueXmlObjectNode* NormalTexture = FindXmlChild(Node, TEXT("NormalTexture")))
	{
		const FString Path = GetXmlAttr(*NormalTexture, TEXT("file"), ResolveXmlInheritedTexturePath(GetXmlAttr(*NormalTexture, TEXT("inherits"))));
		if (!Path.IsEmpty())
		{
			Frame->SetNormalTexture(GlueManager->LoadTexture(Path), Path);
		}
	}
	if (const FWoWGlueXmlObjectNode* PushedTexture = FindXmlChild(Node, TEXT("PushedTexture")))
	{
		const FString Path = GetXmlAttr(*PushedTexture, TEXT("file"), ResolveXmlInheritedTexturePath(GetXmlAttr(*PushedTexture, TEXT("inherits"))));
		if (!Path.IsEmpty())
		{
			Frame->SetPushedTexture(GlueManager->LoadTexture(Path), Path);
		}
	}
	if (const FWoWGlueXmlObjectNode* HighlightTexture = FindXmlChild(Node, TEXT("HighlightTexture")))
	{
		const FString Path = GetXmlAttr(*HighlightTexture, TEXT("file"), ResolveXmlInheritedTexturePath(GetXmlAttr(*HighlightTexture, TEXT("inherits"))));
		if (!Path.IsEmpty())
		{
			Frame->SetHighlightTexture(GlueManager->LoadTexture(Path, EWoWGlueAlphaMode::Add), Path, EWoWGlueAlphaMode::Add);
		}
	}

	if (const FWoWGlueXmlObjectNode* Layers = FindXmlChild(Node, TEXT("Layers")))
	{
		for (const TSharedPtr<FWoWGlueXmlObjectNode>& Layer : Layers->Children)
		{
			if (!Layer.IsValid() || !Layer->Tag.Equals(TEXT("Layer"), ESearchCase::IgnoreCase))
			{
				continue;
			}
			const int32 LayerDrawOffset = XmlLayerLevelToDrawLayer(GetXmlAttr(*Layer, TEXT("level")));
			for (const TSharedPtr<FWoWGlueXmlObjectNode>& LayerChild : Layer->Children)
			{
				if (LayerChild.IsValid() && IsXmlFrameTag(LayerChild->Tag))
				{
					TSharedPtr<FWoWGlueFrame> Ignored;
					if (BuildXmlObject(*LayerChild, Frame, Ignored, OutError) && Ignored.IsValid())
					{
						const int32 TextBias = LayerChild->Tag.Equals(TEXT("FontString"), ESearchCase::IgnoreCase)
							|| LayerChild->Tag.Equals(TEXT("SimpleHTML"), ESearchCase::IgnoreCase)
							? 5
							: 0;
						Ignored->SetDrawLayer(Ignored->GetDrawLayer() + LayerDrawOffset + TextBias);
					}
				}
			}
		}
	}

	if (const FWoWGlueXmlObjectNode* Frames = FindXmlChild(Node, TEXT("Frames")))
	{
		for (const TSharedPtr<FWoWGlueXmlObjectNode>& Child : Frames->Children)
		{
			if (Child.IsValid() && IsXmlFrameTag(Child->Tag))
			{
				TSharedPtr<FWoWGlueFrame> Ignored;
				BuildXmlObject(*Child, Frame, Ignored, OutError);
			}
		}
	}

	if (const FWoWGlueXmlObjectNode* Scripts = FindXmlChild(Node, TEXT("Scripts")))
	{
		for (const TSharedPtr<FWoWGlueXmlObjectNode>& Script : Scripts->Children)
		{
			if (!Script.IsValid())
			{
				continue;
			}
			const FString ScriptName = Script->Tag;
			const FString Body = Script->Content.TrimStartAndEnd();
			if (!Body.IsEmpty())
			{
				BindXmlFrameScript(Frame, ScriptName, Body);
			}
		}
	}

	return true;
}

bool FWoWGlueLuaRuntime::BuildXmlObject(const FWoWGlueXmlObjectNode& Node, TSharedPtr<FWoWGlueFrame> Parent, TSharedPtr<FWoWGlueFrame>& OutFrame, FString* OutError)
{
	if (!GlueManager.IsValid() || !IsXmlFrameTag(Node.Tag))
	{
		return false;
	}

	const FString ParentName = Parent.IsValid() ? Parent->GetName().ToString() : TEXT("UIParent");
	const FString RawName = GetXmlAttr(Node, TEXT("name"));
	const FString FrameName = ReplaceXmlParentToken(RawName, ParentName);
	TSharedPtr<FWoWGlueFrame> EffectiveParent = Parent;
	const FString ParentAttr = ReplaceXmlParentToken(GetXmlAttr(Node, TEXT("parent")), ParentName);
	if (!ParentAttr.IsEmpty())
	{
		EffectiveParent = GlueManager->FindFrame(ParentAttr);
	}
	TSharedRef<FWoWGlueFrame> Frame = GlueManager->CreateFrame(Node.Tag, FrameName, EffectiveParent);
	RegisterFrameGlobal(Frame);

	const FString Id = GetXmlAttr(Node, TEXT("id"));
	if (!Id.IsEmpty())
	{
		PushFrame(LuaState, Frame);
		lua_pushstring(LuaState, "id");
		lua_pushinteger(LuaState, FCString::Atoi(*Id));
		LuaFrameNewIndex(LuaState);
	}

	ApplyXmlObjectBody(Node, Frame, OutError);
	InvokeFrameOnLoad(Frame);
	OutFrame = Frame;
	return true;
}

bool FWoWGlueLuaRuntime::LoadXML(const FString& XmlPath, FString* OutError)
{
	FString XmlText;
	if (!FFileHelper::LoadFileToString(XmlText, *XmlPath))
	{
		if (OutError)
		{
			*OutError = FString::Printf(TEXT("Could not read XML file: %s"), *XmlPath);
		}
		return false;
	}

	FXmlFile XmlFile(SanitizeWoWGlueXmlForUnrealParser(XmlText), EConstructMethod::ConstructFromBuffer);
	if (!XmlFile.IsValid())
	{
		if (OutError)
		{
			*OutError = FString::Printf(TEXT("Could not parse XML file: %s\n%s"), *XmlPath, *XmlFile.GetLastError());
		}
		return false;
	}

	const FXmlNode* Root = XmlFile.GetRootNode();
	if (!Root)
	{
		return true;
	}

	for (const FXmlNode* Child : Root->GetChildrenNodes())
	{
		if (!Child)
		{
			continue;
		}

		FWoWGlueXmlObjectNode Node;
		CopyXmlNode(*Child, Node);
		if (Node.Tag.Equals(TEXT("Script"), ESearchCase::IgnoreCase))
		{
			const FString ScriptFile = GetXmlAttr(Node, TEXT("file"));
			if (!ScriptFile.IsEmpty())
			{
				FString LoadError;
				if (!LoadXmlScriptFile(XmlPath, ScriptFile, &LoadError))
				{
					if (OutError)
					{
						*OutError = LoadError;
					}
					return false;
				}
			}
			continue;
		}

		if (!IsXmlFrameTag(Node.Tag))
		{
			continue;
		}

		const FString Name = GetXmlAttr(Node, TEXT("name"));
		if (GetXmlBoolAttr(Node, TEXT("virtual"), false))
		{
			RegisterXmlTemplate(Name, Node);
			continue;
		}

		TSharedPtr<FWoWGlueFrame> BuiltFrame;
		BuildXmlObject(Node, nullptr, BuiltFrame, OutError);
	}

	return true;
}

bool FWoWGlueLuaRuntime::LoadTOC(const FString& TocPath, FString* OutError)
{
	if (!LuaState)
	{
		if (OutError)
		{
			*OutError = TEXT("Lua runtime is not initialized.");
		}
		return false;
	}

	FString TocText;
	if (!FFileHelper::LoadFileToString(TocText, *TocPath))
	{
		if (OutError)
		{
			*OutError = FString::Printf(TEXT("Could not read TOC file: %s"), *TocPath);
		}
		return false;
	}

	TArray<FString> Lines;
	TocText.ParseIntoArrayLines(Lines, false);
	const FString TocDirectory = FPaths::GetPath(TocPath);
	int32 LoadedLuaCount = 0;
	int32 SkippedXmlCount = 0;

	for (int32 LineIndex = 0; LineIndex < Lines.Num(); ++LineIndex)
	{
		FString Entry = Lines[LineIndex];
		Entry.TrimStartAndEndInline();
		if (Entry.IsEmpty() || Entry.StartsWith(TEXT("#")) || Entry.StartsWith(TEXT("##")))
		{
			continue;
		}

		Entry.ReplaceInline(TEXT("\\"), TEXT("/"));
		FString EntryPath = FPaths::IsRelative(Entry)
			? TocDirectory / Entry
			: Entry;
		FPaths::NormalizeFilename(EntryPath);

		const FString Extension = FPaths::GetExtension(EntryPath).ToLower();
		if (Extension == TEXT("lua"))
		{
			FString LoadError;
			if (!LoadFile(EntryPath, &LoadError))
			{
				if (OutError)
				{
					*OutError = FString::Printf(
						TEXT("%s(%d): failed to load %s\n%s"),
						*TocPath,
						LineIndex + 1,
						*Entry,
						*LoadError);
				}
				return false;
			}
			++LoadedLuaCount;
		}
		else if (Extension == TEXT("xml"))
		{
			FString LoadError;
			if (!LoadXML(EntryPath, &LoadError))
			{
				if (OutError)
				{
					*OutError = FString::Printf(
						TEXT("%s(%d): failed to load %s\n%s"),
						*TocPath,
						LineIndex + 1,
						*Entry,
						*LoadError);
				}
				return false;
			}
			++SkippedXmlCount;
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("[GlueLua] TOC unsupported entry ignored: %s"), *EntryPath);
		}
	}

	UE_LOG(LogTemp, Display, TEXT("[GlueLua] TOC loaded: %s lua=%d xmlPending=%d"), *TocPath, LoadedLuaCount, SkippedXmlCount);
	return true;
}

void FWoWGlueLuaRuntime::Shutdown()
{
	FrameScriptRefs.Reset();
	if (LuaState)
	{
		lua_close(LuaState);
		LuaState = nullptr;
	}
	GlueManager.Reset();
	GlueLoginHandler = nullptr;
	GlueScreenHandler = nullptr;
	CharacterSelectHandler = nullptr;
	EnterWorldHandler = nullptr;
	CharacterCreateHandler = nullptr;
	RealmSelectHandler = nullptr;
	RealmCancelHandler = nullptr;
	ErrorHandler = nullptr;
	BackgroundModelHandler = nullptr;
	DebugLogHandler = nullptr;
}

void FWoWGlueLuaRuntime::SetGlueLoginHandler(FGlueLoginHandler Handler)
{
	GlueLoginHandler = MoveTemp(Handler);
}

void FWoWGlueLuaRuntime::SetGlueScreenHandler(FGlueScreenHandler Handler)
{
	GlueScreenHandler = MoveTemp(Handler);
}

void FWoWGlueLuaRuntime::SetCharacterSelectHandler(FGlueCharacterSelectHandler Handler)
{
	CharacterSelectHandler = MoveTemp(Handler);
}

void FWoWGlueLuaRuntime::SetEnterWorldHandler(FGlueEnterWorldHandler Handler)
{
	EnterWorldHandler = MoveTemp(Handler);
}

void FWoWGlueLuaRuntime::SetCharacterCreateHandler(FGlueCharacterCreateHandler Handler)
{
	CharacterCreateHandler = MoveTemp(Handler);
}

void FWoWGlueLuaRuntime::SetRealmSelectHandler(FGlueRealmSelectHandler Handler)
{
	RealmSelectHandler = MoveTemp(Handler);
}

void FWoWGlueLuaRuntime::SetRealmCancelHandler(FGlueRealmCancelHandler Handler)
{
	RealmCancelHandler = MoveTemp(Handler);
}

void FWoWGlueLuaRuntime::SetErrorHandler(FGlueLuaErrorHandler Handler)
{
	ErrorHandler = MoveTemp(Handler);
}

void FWoWGlueLuaRuntime::SetBackgroundModelHandler(FGlueBackgroundModelHandler Handler)
{
	BackgroundModelHandler = MoveTemp(Handler);
}

void FWoWGlueLuaRuntime::SetDebugLogHandler(FGlueDebugLogHandler Handler)
{
	DebugLogHandler = MoveTemp(Handler);
}

void FWoWGlueLuaRuntime::SetCurrentGlueScreen(const FString& ScreenName)
{
	CurrentGlueScreen = ScreenName.IsEmpty() ? TEXT("login") : ScreenName;
}

void FWoWGlueLuaRuntime::SetAccountErrorText(const FString& ErrorText)
{
	AccountErrorText = ErrorText;
}

void FWoWGlueLuaRuntime::SetCharacterList(const TArray<FString>& CharacterRowsTsv)
{
	CharacterListRowsTsv = CharacterRowsTsv;
}

void FWoWGlueLuaRuntime::SetRealmList(const TArray<FString>& RealmRowsTsv)
{
	RealmListRowsTsv = RealmRowsTsv;
}

void FWoWGlueLuaRuntime::SetCharacterCreateDefaults(int32 Race, int32 ClassId, int32 Gender)
{
	DefaultCharacterCreateRace = Race;
	DefaultCharacterCreateClass = ClassId;
	DefaultCharacterCreateGender = Gender;
}

void FWoWGlueLuaRuntime::LoadGlueDbcStores()
{
	LoadDbcLocalizedNameMap(TEXT("DBFilesClient/ChrRaces.dbc"), 18, DbcRaceNames);
	LoadDbcLocalizedNameMap(TEXT("DBFilesClient/ChrClasses.dbc"), 8, DbcClassNames);
	LoadDbcLocalizedNameMap(TEXT("DBFilesClient/AreaTable.dbc"), 15, DbcAreaNames);
}

bool FWoWGlueLuaRuntime::GetGlobalString(const FString& Name, FString& OutValue) const
{
	OutValue.Empty();
	if (!LuaState || Name.IsEmpty())
	{
		return false;
	}

	FTCHARToUTF8 NameUtf8(*Name);
	lua_getglobal(LuaState, NameUtf8.Get());
	if (!lua_isstring(LuaState, -1))
	{
		lua_pop(LuaState, 1);
		return false;
	}

	OutValue = ToFString(LuaState, -1);
	lua_pop(LuaState, 1);
	return !OutValue.IsEmpty();
}

bool FWoWGlueLuaRuntime::CallGlobalFunction(const FString& FunctionName, const TArray<FString>& StringArgs, FString* OutError)
{
	if (!LuaState)
	{
		if (OutError)
		{
			*OutError = TEXT("Lua runtime is not initialized.");
		}
		return false;
	}

	FTCHARToUTF8 FunctionNameUtf8(*FunctionName);
	lua_getglobal(LuaState, FunctionNameUtf8.Get());
	if (!lua_isfunction(LuaState, -1))
	{
		lua_pop(LuaState, 1);
		if (OutError)
		{
			*OutError = FString::Printf(TEXT("Lua global is not a function: %s"), *FunctionName);
		}
		return false;
	}

	for (const FString& Arg : StringArgs)
	{
		PushFString(LuaState, Arg);
	}

	const int32 CallResult = lua_pcall(LuaState, StringArgs.Num(), 0, 0);
	if (CallResult != 0)
	{
		const FString Error = ToFString(LuaState, -1);
		lua_pop(LuaState, 1);
		if (OutError)
		{
			*OutError = Error;
		}
		UE_LOG(LogTemp, Error, TEXT("[GlueLua] global call failed: %s: %s"), *FunctionName, *Error);
		if (ErrorHandler)
		{
			ErrorHandler(Error);
		}
		return false;
	}

	return true;
}

FWoWGlueLuaRuntime* FWoWGlueLuaRuntime::GetRuntime(lua_State* L)
{
	lua_getfield(L, LUA_REGISTRYINDEX, RuntimeRegistryKey);
	FWoWGlueLuaRuntime* Runtime = static_cast<FWoWGlueLuaRuntime*>(lua_touserdata(L, -1));
	lua_pop(L, 1);
	return Runtime;
}

FWoWGlueFrame* FWoWGlueLuaRuntime::CheckFrame(lua_State* L, int32 StackIndex)
{
	FWoWGlueLuaFrameUserData* UserData = static_cast<FWoWGlueLuaFrameUserData*>(luaL_checkudata(L, StackIndex, FrameMetatableName));
	return UserData ? UserData->Frame : nullptr;
}

void FWoWGlueLuaRuntime::PushFrame(lua_State* L, TSharedRef<FWoWGlueFrame> Frame)
{
	FWoWGlueLuaFrameUserData* UserData = static_cast<FWoWGlueLuaFrameUserData*>(lua_newuserdata(L, sizeof(FWoWGlueLuaFrameUserData)));
	UserData->Frame = &Frame.Get();
	luaL_getmetatable(L, FrameMetatableName);
	lua_setmetatable(L, -2);

	lua_getfield(L, LUA_REGISTRYINDEX, FrameFieldRegistryKey);
	if (!lua_istable(L, -1))
	{
		lua_pop(L, 1);
		lua_newtable(L);
		lua_pushvalue(L, -1);
		lua_setfield(L, LUA_REGISTRYINDEX, FrameFieldRegistryKey);
	}

	lua_pushlightuserdata(L, UserData->Frame);
	lua_rawget(L, -2);
	if (!lua_istable(L, -1))
	{
		lua_pop(L, 1);
		lua_newtable(L);
		lua_pushlightuserdata(L, UserData->Frame);
		lua_pushvalue(L, -2);
		lua_rawset(L, -4);
	}

	lua_setfenv(L, -3);
	lua_pop(L, 1);
}

FString FWoWGlueLuaRuntime::ToFString(lua_State* L, int32 StackIndex)
{
	size_t Length = 0;
	const char* Utf8 = lua_tolstring(L, StackIndex, &Length);
	return Utf8 ? FString(UTF8_TO_TCHAR(Utf8)) : FString();
}

void FWoWGlueLuaRuntime::PushFString(lua_State* L, const FString& Value)
{
	FTCHARToUTF8 Utf8(*Value);
	lua_pushlstring(L, Utf8.Get(), Utf8.Length());
}

int32 FWoWGlueLuaRuntime::LuaFrameIndex(lua_State* L)
{
	luaL_checkudata(L, 1, FrameMetatableName);

	lua_getmetatable(L, 1);
	lua_getfield(L, -1, FrameMethodsMetatableKey);
	lua_pushvalue(L, 2);
	lua_rawget(L, -2);
	if (!lua_isnil(L, -1))
	{
		return 1;
	}
	lua_pop(L, 3);

	lua_getfenv(L, 1);
	lua_pushvalue(L, 2);
	lua_rawget(L, -2);
	return 1;
}

int32 FWoWGlueLuaRuntime::LuaFrameNewIndex(lua_State* L)
{
	luaL_checkudata(L, 1, FrameMetatableName);

	lua_getfenv(L, 1);
	lua_pushvalue(L, 2);
	lua_pushvalue(L, 3);
	lua_rawset(L, -3);
	return 0;
}

int32 FWoWGlueLuaRuntime::LuaPrint(lua_State* L)
{
	FWoWGlueLuaRuntime* Runtime = GetRuntime(L);
	FString Message;
	const int32 ArgCount = lua_gettop(L);
	for (int32 Index = 1; Index <= ArgCount; ++Index)
	{
		if (Index > 1)
		{
			Message += TEXT("\t");
		}
		Message += ToFString(L, Index);
	}
	UE_LOG(LogTemp, Display, TEXT("[GlueLua] %s"), *Message);
	if (Runtime && Runtime->DebugLogHandler)
	{
		Runtime->DebugLogHandler(Message);
	}
	return 0;
}

int32 FWoWGlueLuaRuntime::LuaCreateFrame(lua_State* L)
{
	FWoWGlueLuaRuntime* Runtime = GetRuntime(L);
	if (!Runtime || !Runtime->GlueManager.IsValid())
	{
		return LuaError(L, TEXT("CreateFrame called before GlueManager is ready."));
	}

	const FString TypeName = ToFString(L, 1);
	const FString Name = lua_isnoneornil(L, 2) ? FString() : ToFString(L, 2);
	TSharedPtr<FWoWGlueFrame> Parent = Runtime->ResolveFrameFromLua(L, 3);
	TSharedRef<FWoWGlueFrame> Frame = Runtime->GlueManager->CreateFrame(TypeName, Name, Parent);
	Runtime->RegisterFrameGlobal(Frame);
	PushFrame(L, Frame);
	return 1;
}

int32 FWoWGlueLuaRuntime::LuaGlueLogin(lua_State* L)
{
	FWoWGlueLuaRuntime* Runtime = GetRuntime(L);
	if (!Runtime)
	{
		return 0;
	}

	const FString AccountName = lua_isnoneornil(L, 1) ? FString() : ToFString(L, 1);
	const FString Password = lua_isnoneornil(L, 2) ? FString() : ToFString(L, 2);
	if (Runtime->GlueLoginHandler)
	{
		Runtime->GlueLoginHandler(AccountName, Password);
	}
	else
	{
		UE_LOG(LogTemp, Display, TEXT("[GlueLua] GlueLogin account=%s passwordLength=%d"), *AccountName, Password.Len());
	}
	return 0;
}

int32 FWoWGlueLuaRuntime::LuaLaunchURL(lua_State* L)
{
	const FString Url = lua_isnoneornil(L, 1) ? FString() : ToFString(L, 1);
	if (Url.IsEmpty() || Url == TEXT("#"))
	{
		UE_LOG(LogTemp, Display, TEXT("[GlueLua] LaunchURL ignored empty placeholder URL: %s"), *Url);
		return 0;
	}

	UE_LOG(LogTemp, Display, TEXT("[GlueLua] LaunchURL %s"), *Url);
	FPlatformProcess::LaunchURL(*Url, nullptr, nullptr);
	return 0;
}

int32 FWoWGlueLuaRuntime::LuaSetGlueScreen(lua_State* L)
{
	FWoWGlueLuaRuntime* Runtime = GetRuntime(L);
	if (!Runtime)
	{
		return 0;
	}

	const FString ScreenName = lua_isnoneornil(L, 1) ? FString() : ToFString(L, 1);
	Runtime->SetCurrentGlueScreen(ScreenName);
	if (Runtime->GlueScreenHandler)
	{
		Runtime->GlueScreenHandler(Runtime->GetCurrentGlueScreen());
	}
	return 0;
}

int32 FWoWGlueLuaRuntime::LuaGetGlueScreen(lua_State* L)
{
	if (FWoWGlueLuaRuntime* Runtime = GetRuntime(L))
	{
		PushFString(L, Runtime->GetCurrentGlueScreen());
		return 1;
	}

	lua_pushstring(L, "login");
	return 1;
}

int32 FWoWGlueLuaRuntime::LuaGetScreenWidth(lua_State* L)
{
	if (FWoWGlueLuaRuntime* Runtime = GetRuntime(L); Runtime && Runtime->GlueManager.IsValid())
	{
		lua_pushnumber(L, Runtime->GlueManager->GetScreenSize().X);
		return 1;
	}

	lua_pushnumber(L, 1920.0);
	return 1;
}

int32 FWoWGlueLuaRuntime::LuaGetScreenHeight(lua_State* L)
{
	if (FWoWGlueLuaRuntime* Runtime = GetRuntime(L); Runtime && Runtime->GlueManager.IsValid())
	{
		lua_pushnumber(L, Runtime->GlueManager->GetScreenSize().Y);
		return 1;
	}

	lua_pushnumber(L, 1080.0);
	return 1;
}

int32 FWoWGlueLuaRuntime::LuaGetAccountError(lua_State* L)
{
	if (FWoWGlueLuaRuntime* Runtime = GetRuntime(L))
	{
		PushFString(L, Runtime->AccountErrorText);
		return 1;
	}

	lua_pushstring(L, "");
	return 1;
}

int32 FWoWGlueLuaRuntime::LuaGetNumCharacters(lua_State* L)
{
	if (FWoWGlueLuaRuntime* Runtime = GetRuntime(L))
	{
		lua_pushinteger(L, Runtime->CharacterListRowsTsv.Num());
		return 1;
	}

	lua_pushinteger(L, 0);
	return 1;
}

int32 FWoWGlueLuaRuntime::LuaGetCharacterInfo(lua_State* L)
{
	FWoWGlueLuaRuntime* Runtime = GetRuntime(L);
	if (!Runtime)
	{
		return 0;
	}

	const int32 Index = static_cast<int32>(luaL_checkinteger(L, 1)) - 1;
	if (!Runtime->CharacterListRowsTsv.IsValidIndex(Index))
	{
		return 0;
	}

	TArray<FString> Columns;
	Runtime->CharacterListRowsTsv[Index].ParseIntoArray(Columns, TEXT("\t"), false);
	if (Columns.Num() < 6)
	{
		return 0;
	}

	const FString& Name = Columns[1];
	const int32 RaceId = FCString::Atoi(*Columns[2]);
	const int32 ClassId = FCString::Atoi(*Columns[3]);
	const int32 RawGender = FCString::Atoi(*Columns[4]);
	const int32 Sex = RawGender == 0 ? 2 : (RawGender == 1 ? 3 : 1);
	const int32 Level = FCString::Atoi(*Columns[5]);
	const int32 ZoneId = Columns.Num() > 12 ? FCString::Atoi(*Columns[12]) : 0;
	const uint32 CharacterFlags = Columns.Num() > 14 ? static_cast<uint32>(FCString::Strtoui64(*Columns[14], nullptr, 10)) : 0;
	const uint32 AtLoginFlags = Columns.Num() > 15 ? static_cast<uint32>(FCString::Strtoui64(*Columns[15], nullptr, 10)) : 0;
	const FString RaceName = Runtime->DbcRaceNames.Contains(RaceId)
		? Runtime->DbcRaceNames[RaceId]
		: FString::Printf(TEXT("Race:%d"), RaceId);
	const FString ClassName = Runtime->DbcClassNames.Contains(ClassId)
		? Runtime->DbcClassNames[ClassId]
		: FString::Printf(TEXT("Class:%d"), ClassId);
	const FString Zone = Runtime->DbcAreaNames.Contains(ZoneId)
		? Runtime->DbcAreaNames[ZoneId]
		: FString(TEXT(""));

	PushFString(L, Name);
	PushFString(L, RaceName);
	PushFString(L, ClassName);
	lua_pushinteger(L, FMath::Max(1, Level));
	PushFString(L, Zone);
	lua_pushinteger(L, Sex);
	lua_pushboolean(L, (CharacterFlags & CharacterFlagGhost) != 0);
	lua_pushboolean(L, (AtLoginFlags & AtLoginCustomize) != 0);
	lua_pushboolean(L, (AtLoginFlags & AtLoginChangeRace) != 0);
	lua_pushboolean(L, (AtLoginFlags & AtLoginChangeFaction) != 0);
	return 10;
}

int32 FWoWGlueLuaRuntime::LuaGetCharacterSummary(lua_State* L)
{
	FWoWGlueLuaRuntime* Runtime = GetRuntime(L);
	if (!Runtime)
	{
		return 0;
	}

	const int32 Index = static_cast<int32>(luaL_checkinteger(L, 1)) - 1;
	if (!Runtime->CharacterListRowsTsv.IsValidIndex(Index))
	{
		return 0;
	}

	TArray<FString> Columns;
	Runtime->CharacterListRowsTsv[Index].ParseIntoArray(Columns, TEXT("\t"), false);
	for (const FString& Column : Columns)
	{
		PushFString(L, Column);
	}
	return Columns.Num();
}

FString FWoWGlueLuaRuntime::BuildCharacterInfoDebugDump() const
{
	FString Dump;
	Dump += FString::Printf(TEXT("GetCharacterInfo 调试: 角色数量=%d\n"), CharacterListRowsTsv.Num());
	Dump += TEXT("返回顺序: name, race, class, level, zone, sex, ghost, PCC, PRC, PFC\n");
	Dump += TEXT("说明: sex 使用官方 Glue 常量 SEX_NONE=1, SEX_MALE=2, SEX_FEMALE=3\n");
	Dump += TEXT("装备列: slot:displayId:inventoryType:enchantId；来自 SMSG_CHAR_ENUM 23 槽外观块，不是 UE 猜测。\n");

	for (int32 Index = 0; Index < CharacterListRowsTsv.Num(); ++Index)
	{
		TArray<FString> Columns;
		CharacterListRowsTsv[Index].ParseIntoArray(Columns, TEXT("\t"), false);
		if (Columns.Num() < 6)
		{
			Dump += FString::Printf(TEXT("[%d] TSV列不足: %s\n"), Index + 1, *CharacterListRowsTsv[Index]);
			continue;
		}

		const FString& Name = Columns[1];
		const int32 RaceId = FCString::Atoi(*Columns[2]);
		const int32 ClassId = FCString::Atoi(*Columns[3]);
		const int32 RawGender = FCString::Atoi(*Columns[4]);
		const int32 Sex = RawGender == 0 ? 2 : (RawGender == 1 ? 3 : 1);
		const int32 Level = FCString::Atoi(*Columns[5]);
		const int32 ZoneId = Columns.Num() > 12 ? FCString::Atoi(*Columns[12]) : 0;
		const uint32 CharacterFlags = Columns.Num() > 14 ? static_cast<uint32>(FCString::Strtoui64(*Columns[14], nullptr, 10)) : 0;
		const uint32 AtLoginFlags = Columns.Num() > 15 ? static_cast<uint32>(FCString::Strtoui64(*Columns[15], nullptr, 10)) : 0;
		const FString EquipmentSummary = Columns.Num() > 16 ? Columns[16] : FString();
		const FString RaceName = DbcRaceNames.Contains(RaceId)
			? DbcRaceNames[RaceId]
			: FString::Printf(TEXT("Race:%d"), RaceId);
		const FString ClassName = DbcClassNames.Contains(ClassId)
			? DbcClassNames[ClassId]
			: FString::Printf(TEXT("Class:%d"), ClassId);
		const FString Zone = DbcAreaNames.Contains(ZoneId)
			? DbcAreaNames[ZoneId]
			: FString();

		Dump += FString::Printf(
			TEXT("[%d] name=%s | race=%s(%d) | class=%s(%d) | level=%d | zone=%s(%d) | sex=%d(raw=%d) | ghost=%s flags=0x%08X | PCC=%s PRC=%s PFC=%s atLogin=0x%08X | equipment=%s\n"),
			Index + 1,
			*Name,
			*RaceName,
			RaceId,
			*ClassName,
			ClassId,
			FMath::Max(1, Level),
			*Zone,
			ZoneId,
			Sex,
			RawGender,
			(CharacterFlags & CharacterFlagGhost) != 0 ? TEXT("true") : TEXT("false"),
			CharacterFlags,
			(AtLoginFlags & AtLoginCustomize) != 0 ? TEXT("true") : TEXT("false"),
			(AtLoginFlags & AtLoginChangeRace) != 0 ? TEXT("true") : TEXT("false"),
			(AtLoginFlags & AtLoginChangeFaction) != 0 ? TEXT("true") : TEXT("false"),
			AtLoginFlags,
			EquipmentSummary.IsEmpty() ? TEXT("(空)") : *EquipmentSummary);
	}

	return Dump;
}

int32 FWoWGlueLuaRuntime::LuaGetNumRealms(lua_State* L)
{
	if (FWoWGlueLuaRuntime* Runtime = GetRuntime(L))
	{
		lua_pushinteger(L, Runtime->RealmListRowsTsv.Num());
		return 1;
	}

	lua_pushinteger(L, 0);
	return 1;
}

int32 FWoWGlueLuaRuntime::LuaGetRealmSummary(lua_State* L)
{
	FWoWGlueLuaRuntime* Runtime = GetRuntime(L);
	if (!Runtime)
	{
		return 0;
	}

	const int32 Index = static_cast<int32>(luaL_checkinteger(L, 1)) - 1;
	if (!Runtime->RealmListRowsTsv.IsValidIndex(Index))
	{
		return 0;
	}

	TArray<FString> Columns;
	Runtime->RealmListRowsTsv[Index].ParseIntoArray(Columns, TEXT("\t"), false);
	for (const FString& Column : Columns)
	{
		PushFString(L, Column);
	}
	return Columns.Num();
}

int32 FWoWGlueLuaRuntime::LuaSelectRealm(lua_State* L)
{
	FWoWGlueLuaRuntime* Runtime = GetRuntime(L);
	if (!Runtime || !Runtime->RealmSelectHandler)
	{
		return 0;
	}

	const int32 RealmIndex = static_cast<int32>(luaL_checkinteger(L, 1));
	Runtime->RealmSelectHandler(RealmIndex);
	return 0;
}

int32 FWoWGlueLuaRuntime::LuaChangeRealm(lua_State* L)
{
	FWoWGlueLuaRuntime* Runtime = GetRuntime(L);
	if (!Runtime || !Runtime->RealmSelectHandler)
	{
		return 0;
	}

	const int32 RealmIndex = lua_isnoneornil(L, 2)
		? static_cast<int32>(luaL_checkinteger(L, 1))
		: static_cast<int32>(luaL_checkinteger(L, 2));
	Runtime->RealmSelectHandler(RealmIndex);
	return 0;
}

int32 FWoWGlueLuaRuntime::LuaCancelRealmList(lua_State* L)
{
	FWoWGlueLuaRuntime* Runtime = GetRuntime(L);
	if (Runtime && Runtime->RealmCancelHandler)
	{
		Runtime->RealmCancelHandler();
	}
	return 0;
}

int32 FWoWGlueLuaRuntime::LuaSelectCharacter(lua_State* L)
{
	FWoWGlueLuaRuntime* Runtime = GetRuntime(L);
	if (!Runtime || !Runtime->CharacterSelectHandler)
	{
		return 0;
	}

	const int32 Guid = static_cast<int32>(luaL_checkinteger(L, 1));
	Runtime->CharacterSelectHandler(Guid);
	return 0;
}

int32 FWoWGlueLuaRuntime::LuaEnterWorld(lua_State* L)
{
	FWoWGlueLuaRuntime* Runtime = GetRuntime(L);
	if (Runtime && Runtime->EnterWorldHandler)
	{
		Runtime->EnterWorldHandler();
	}
	return 0;
}

int32 FWoWGlueLuaRuntime::LuaCreateAzerothCharacter(lua_State* L)
{
	FWoWGlueLuaRuntime* Runtime = GetRuntime(L);
	if (!Runtime || !Runtime->CharacterCreateHandler)
	{
		return 0;
	}

	const FString CharacterName = ToFString(L, 1);
	const int32 Race = static_cast<int32>(luaL_checkinteger(L, 2));
	const int32 ClassId = static_cast<int32>(luaL_checkinteger(L, 3));
	const int32 Gender = static_cast<int32>(luaL_checkinteger(L, 4));
	const int32 Skin = static_cast<int32>(luaL_optinteger(L, 5, 0));
	const int32 Face = static_cast<int32>(luaL_optinteger(L, 6, 0));
	const int32 HairStyle = static_cast<int32>(luaL_optinteger(L, 7, 0));
	const int32 HairColor = static_cast<int32>(luaL_optinteger(L, 8, 0));
	const int32 FacialStyle = static_cast<int32>(luaL_optinteger(L, 9, 0));

	Runtime->CharacterCreateHandler(CharacterName, Race, ClassId, Gender, Skin, Face, HairStyle, HairColor, FacialStyle);
	return 0;
}

int32 FWoWGlueLuaRuntime::LuaSetBackgroundModel(lua_State* L)
{
	FWoWGlueLuaRuntime* Runtime = GetRuntime(L);
	if (!Runtime || !Runtime->BackgroundModelHandler)
	{
		return 0;
	}

	int32 PathIndex = 1;
	if (!lua_isstring(L, PathIndex) && lua_gettop(L) >= 2)
	{
		PathIndex = 2;
	}

	const FString ModelPath = lua_isnoneornil(L, PathIndex) ? FString() : ToFString(L, PathIndex);
	const int32 Race = static_cast<int32>(luaL_optinteger(L, PathIndex + 1, 0));
	const int32 GenderIndex = PathIndex == 1 ? 3 : PathIndex + 3;
	const int32 Gender = static_cast<int32>(luaL_optinteger(L, GenderIndex, 0));
	Runtime->BackgroundModelHandler(ModelPath, Race, Gender);
	return 0;
}

int32 FWoWGlueLuaRuntime::LuaSetCharCustomizeBackground(lua_State* L)
{
	FWoWGlueLuaRuntime* Runtime = GetRuntime(L);
	if (!Runtime || !Runtime->BackgroundModelHandler)
	{
		return 0;
	}

	const FString ModelPath = lua_isnoneornil(L, 1) ? FString() : ToFString(L, 1);
	Runtime->BackgroundModelHandler(ModelPath, 0, 0);
	return 0;
}

int32 FWoWGlueLuaRuntime::LuaGetDefaultCharacterCreateRace(lua_State* L)
{
	if (FWoWGlueLuaRuntime* Runtime = GetRuntime(L))
	{
		lua_pushinteger(L, Runtime->DefaultCharacterCreateRace);
		return 1;
	}

	lua_pushinteger(L, 1);
	return 1;
}

int32 FWoWGlueLuaRuntime::LuaGetDefaultCharacterCreateClass(lua_State* L)
{
	if (FWoWGlueLuaRuntime* Runtime = GetRuntime(L))
	{
		lua_pushinteger(L, Runtime->DefaultCharacterCreateClass);
		return 1;
	}

	lua_pushinteger(L, 1);
	return 1;
}

int32 FWoWGlueLuaRuntime::LuaGetDefaultCharacterCreateGender(lua_State* L)
{
	if (FWoWGlueLuaRuntime* Runtime = GetRuntime(L))
	{
		lua_pushinteger(L, Runtime->DefaultCharacterCreateGender);
		return 1;
	}

	lua_pushinteger(L, 0);
	return 1;
}

int32 FWoWGlueLuaRuntime::LuaFrameSetPoint(lua_State* L)
{
	FWoWGlueFrame* Frame = CheckFrame(L, 1);
	FWoWGlueLuaRuntime* Runtime = GetRuntime(L);
	if (!Frame || !Runtime)
	{
		return LuaError(L, TEXT("SetPoint expected a frame."));
	}

	const FString Point = ToFString(L, 2);
	TSharedPtr<FWoWGlueFrame> RelativeTo = Runtime->ResolveFrameFromLua(L, 3);
	const FString RelativePoint = lua_isnoneornil(L, 4) ? Point : ToFString(L, 4);
	const float XOffset = static_cast<float>(luaL_optnumber(L, 5, 0.0));
	const float YOffset = static_cast<float>(luaL_optnumber(L, 6, 0.0));
	Frame->SetPoint(Point, RelativeTo, RelativePoint, XOffset, YOffset);
	return 0;
}

int32 FWoWGlueLuaRuntime::LuaFrameSetSize(lua_State* L)
{
	FWoWGlueFrame* Frame = CheckFrame(L, 1);
	if (!Frame)
	{
		return LuaError(L, TEXT("SetSize expected a frame."));
	}
	Frame->SetSize(static_cast<float>(luaL_checknumber(L, 2)), static_cast<float>(luaL_checknumber(L, 3)));
	return 0;
}

int32 FWoWGlueLuaRuntime::LuaFrameGetSize(lua_State* L)
{
	FWoWGlueFrame* Frame = CheckFrame(L, 1);
	if (!Frame)
	{
		return LuaError(L, TEXT("GetSize expected a frame."));
	}
	const FVector2D Size = Frame->GetSize();
	lua_pushnumber(L, Size.X);
	lua_pushnumber(L, Size.Y);
	return 2;
}

int32 FWoWGlueLuaRuntime::LuaFrameSetWidth(lua_State* L)
{
	FWoWGlueFrame* Frame = CheckFrame(L, 1);
	if (!Frame)
	{
		return LuaError(L, TEXT("SetWidth expected a frame."));
	}
	const FVector2D Size = Frame->GetSize();
	Frame->SetSize(static_cast<float>(luaL_checknumber(L, 2)), Size.Y);
	return 0;
}

int32 FWoWGlueLuaRuntime::LuaFrameSetHeight(lua_State* L)
{
	FWoWGlueFrame* Frame = CheckFrame(L, 1);
	if (!Frame)
	{
		return LuaError(L, TEXT("SetHeight expected a frame."));
	}
	const FVector2D Size = Frame->GetSize();
	Frame->SetSize(Size.X, static_cast<float>(luaL_checknumber(L, 2)));
	return 0;
}

int32 FWoWGlueLuaRuntime::LuaFrameGetWidth(lua_State* L)
{
	FWoWGlueFrame* Frame = CheckFrame(L, 1);
	if (!Frame)
	{
		return LuaError(L, TEXT("GetWidth expected a frame."));
	}
	lua_pushnumber(L, Frame->GetLuaReportedSize().X);
	return 1;
}

int32 FWoWGlueLuaRuntime::LuaFrameGetHeight(lua_State* L)
{
	FWoWGlueFrame* Frame = CheckFrame(L, 1);
	if (!Frame)
	{
		return LuaError(L, TEXT("GetHeight expected a frame."));
	}
	lua_pushnumber(L, Frame->GetLuaReportedSize().Y);
	return 1;
}

int32 FWoWGlueLuaRuntime::LuaFrameSetText(lua_State* L)
{
	FWoWGlueFrame* Frame = CheckFrame(L, 1);
	if (!Frame)
	{
		return LuaError(L, TEXT("SetText expected a frame."));
	}
	const FString Text = ToFString(L, 2);
	if (Frame->GetName().ToString() == TEXT("GlueDialogText"))
	{
		const FVector2D LayoutSize = Frame->GetLayoutSize();
		UE_LOG(LogTemp, Display, TEXT("[GlueLua] GlueDialogText:SetText '%s' size=%s fontPath=%s fontSize=%.1f"),
			*Text,
			*LayoutSize.ToString(),
			*Frame->GetFontPath(),
			Frame->GetFontSize());
	}
	Frame->SetText(Text);
	return 0;
}

int32 FWoWGlueLuaRuntime::LuaFrameGetText(lua_State* L)
{
	FWoWGlueFrame* Frame = CheckFrame(L, 1);
	if (!Frame)
	{
		return LuaError(L, TEXT("GetText expected a frame."));
	}
	PushFString(L, Frame->GetTextString());
	return 1;
}

int32 FWoWGlueLuaRuntime::LuaFrameGetName(lua_State* L)
{
	FWoWGlueFrame* Frame = CheckFrame(L, 1);
	if (!Frame)
	{
		return LuaError(L, TEXT("GetName expected a frame."));
	}
	PushFString(L, Frame->GetName().ToString());
	return 1;
}

int32 FWoWGlueLuaRuntime::LuaFrameGetID(lua_State* L)
{
	CheckFrame(L, 1);
	lua_getfenv(L, 1);
	lua_getfield(L, -1, "id");
	if (!lua_isnumber(L, -1))
	{
		lua_pop(L, 2);
		lua_pushinteger(L, 0);
		return 1;
	}
	const lua_Integer Id = lua_tointeger(L, -1);
	lua_pop(L, 2);
	lua_pushinteger(L, Id);
	return 1;
}

int32 FWoWGlueLuaRuntime::LuaFrameSetFormattedText(lua_State* L)
{
	FWoWGlueFrame* Frame = CheckFrame(L, 1);
	if (!Frame)
	{
		return LuaError(L, TEXT("SetFormattedText expected a frame."));
	}

	FString Text = ToFString(L, 2);
	const int32 ArgCount = lua_gettop(L);
	for (int32 ArgIndex = 3; ArgIndex <= ArgCount; ++ArgIndex)
	{
		const FString ArgText = ToFString(L, ArgIndex);
		if (Text.Contains(TEXT("%s")))
		{
			Text.ReplaceInline(TEXT("%s"), *ArgText, ESearchCase::CaseSensitive);
		}
		else if (Text.Contains(TEXT("%d")))
		{
			Text.ReplaceInline(TEXT("%d"), *ArgText, ESearchCase::CaseSensitive);
		}
		else
		{
			const FString Token = FString::Printf(TEXT("{%d}"), ArgIndex - 3);
			Text.ReplaceInline(*Token, *ArgText);
		}
	}
	Frame->SetText(Text);
	return 0;
}

int32 FWoWGlueLuaRuntime::LuaFrameGetBoundsRect(lua_State* L)
{
	FWoWGlueFrame* Frame = CheckFrame(L, 1);
	if (!Frame)
	{
		return LuaError(L, TEXT("GetBoundsRect expected a frame."));
	}

	const FVector2D Size = Frame->MeasureDesiredContentSize();
	lua_pushnumber(L, 0.0);
	lua_pushnumber(L, 0.0);
	lua_pushnumber(L, Size.X);
	lua_pushnumber(L, Size.Y);
	return 4;
}

int32 FWoWGlueLuaRuntime::LuaFrameSetAlpha(lua_State* L)
{
	FWoWGlueFrame* Frame = CheckFrame(L, 1);
	if (!Frame)
	{
		return LuaError(L, TEXT("SetAlpha expected a frame."));
	}
	Frame->SetAlpha(static_cast<float>(luaL_optnumber(L, 2, 1.0)));
	return 0;
}

int32 FWoWGlueLuaRuntime::LuaFrameGetAlpha(lua_State* L)
{
	FWoWGlueFrame* Frame = CheckFrame(L, 1);
	if (!Frame)
	{
		return LuaError(L, TEXT("GetAlpha expected a frame."));
	}
	lua_pushnumber(L, Frame->GetAlpha());
	return 1;
}

int32 FWoWGlueLuaRuntime::LuaFrameSetVertexColor(lua_State* L)
{
	FWoWGlueFrame* Frame = CheckFrame(L, 1);
	if (!Frame)
	{
		return LuaError(L, TEXT("SetVertexColor expected a frame."));
	}
	Frame->SetVertexColor(
		static_cast<float>(luaL_optnumber(L, 2, 1.0)),
		static_cast<float>(luaL_optnumber(L, 3, 1.0)),
		static_cast<float>(luaL_optnumber(L, 4, 1.0)),
		static_cast<float>(luaL_optnumber(L, 5, 1.0)));
	return 0;
}

int32 FWoWGlueLuaRuntime::LuaFrameSetPassword(lua_State* L)
{
	FWoWGlueFrame* Frame = CheckFrame(L, 1);
	if (!Frame)
	{
		return LuaError(L, TEXT("SetPassword expected a frame."));
	}
	Frame->SetPassword(lua_toboolean(L, 2) != 0);
	return 0;
}

int32 FWoWGlueLuaRuntime::LuaFrameSetPlaceholderText(lua_State* L)
{
	FWoWGlueFrame* Frame = CheckFrame(L, 1);
	if (!Frame)
	{
		return LuaError(L, TEXT("SetPlaceholderText expected a frame."));
	}

	Frame->SetPlaceholderText(lua_isnoneornil(L, 2) ? FString() : ToFString(L, 2));
	return 0;
}

int32 FWoWGlueLuaRuntime::LuaFrameSetPlaceholderTextColor(lua_State* L)
{
	FWoWGlueFrame* Frame = CheckFrame(L, 1);
	if (!Frame)
	{
		return LuaError(L, TEXT("SetPlaceholderTextColor expected a frame."));
	}

	const float R = static_cast<float>(luaL_optnumber(L, 2, 0.45));
	const float G = static_cast<float>(luaL_optnumber(L, 3, 0.45));
	const float B = static_cast<float>(luaL_optnumber(L, 4, 0.45));
	const float A = static_cast<float>(luaL_optnumber(L, 5, 1.0));
	Frame->SetPlaceholderTextColor(R, G, B, A);
	return 0;
}

int32 FWoWGlueLuaRuntime::LuaFrameSetPlaceholderJustifyH(lua_State* L)
{
	FWoWGlueFrame* Frame = CheckFrame(L, 1);
	if (!Frame)
	{
		return LuaError(L, TEXT("SetPlaceholderJustifyH expected a frame."));
	}

	Frame->SetPlaceholderJustifyH(ToFString(L, 2));
	return 0;
}

int32 FWoWGlueLuaRuntime::LuaFrameSetTextInsets(lua_State* L)
{
	FWoWGlueFrame* Frame = CheckFrame(L, 1);
	if (!Frame)
	{
		return LuaError(L, TEXT("SetTextInsets expected a frame."));
	}

	const float Left = static_cast<float>(luaL_optnumber(L, 2, 0.0));
	const float Right = static_cast<float>(luaL_optnumber(L, 3, 0.0));
	const float Top = static_cast<float>(luaL_optnumber(L, 4, 0.0));
	const float Bottom = static_cast<float>(luaL_optnumber(L, 5, 0.0));
	Frame->SetTextInsets(Left, Right, Top, Bottom);
	return 0;
}

int32 FWoWGlueLuaRuntime::LuaFrameSetTextColor(lua_State* L)
{
	FWoWGlueFrame* Frame = CheckFrame(L, 1);
	if (!Frame)
	{
		return LuaError(L, TEXT("SetTextColor expected a frame."));
	}

	const float R = static_cast<float>(luaL_optnumber(L, 2, 1.0));
	const float G = static_cast<float>(luaL_optnumber(L, 3, 1.0));
	const float B = static_cast<float>(luaL_optnumber(L, 4, 1.0));
	const float A = static_cast<float>(luaL_optnumber(L, 5, 1.0));
	Frame->SetTextColor(R, G, B, A);
	return 0;
}

int32 FWoWGlueLuaRuntime::LuaFrameSetFont(lua_State* L)
{
	FWoWGlueFrame* Frame = CheckFrame(L, 1);
	if (!Frame)
	{
		return LuaError(L, TEXT("SetFont expected a frame."));
	}

	const FString FontPath = lua_isnoneornil(L, 2) ? FString() : ToFString(L, 2);
	const float FontSize = LuaOptionalNumberNoThrow(L, 3, 14.0f, TEXT("SetFont"));
	const FString Flags = lua_isnoneornil(L, 4) ? FString() : ToFString(L, 4);
	Frame->SetFont(FontPath, FontSize, Flags);
	return 0;
}

int32 FWoWGlueLuaRuntime::LuaFrameSetFontObject(lua_State* L)
{
	FWoWGlueFrame* Frame = CheckFrame(L, 1);
	if (!Frame)
	{
		return LuaError(L, TEXT("SetFontObject expected a frame."));
	}

	Frame->SetFontObject(ToFString(L, 2));
	return 0;
}

int32 FWoWGlueLuaRuntime::LuaFrameSetShadowColor(lua_State* L)
{
	FWoWGlueFrame* Frame = CheckFrame(L, 1);
	if (!Frame)
	{
		return LuaError(L, TEXT("SetShadowColor expected a frame."));
	}

	const float R = static_cast<float>(luaL_optnumber(L, 2, 0.0));
	const float G = static_cast<float>(luaL_optnumber(L, 3, 0.0));
	const float B = static_cast<float>(luaL_optnumber(L, 4, 0.0));
	const float A = static_cast<float>(luaL_optnumber(L, 5, 1.0));
	Frame->SetShadowColor(R, G, B, A);
	return 0;
}

int32 FWoWGlueLuaRuntime::LuaFrameSetShadowOffset(lua_State* L)
{
	FWoWGlueFrame* Frame = CheckFrame(L, 1);
	if (!Frame)
	{
		return LuaError(L, TEXT("SetShadowOffset expected a frame."));
	}

	const float XOffset = static_cast<float>(luaL_optnumber(L, 2, 0.0));
	const float YOffset = static_cast<float>(luaL_optnumber(L, 3, 0.0));
	Frame->SetShadowOffset(XOffset, YOffset);
	return 0;
}

int32 FWoWGlueLuaRuntime::LuaFrameSetJustifyH(lua_State* L)
{
	FWoWGlueFrame* Frame = CheckFrame(L, 1);
	if (!Frame)
	{
		return LuaError(L, TEXT("SetJustifyH expected a frame."));
	}

	Frame->SetJustifyH(ToFString(L, 2));
	return 0;
}

int32 FWoWGlueLuaRuntime::LuaFrameSetJustifyV(lua_State* L)
{
	FWoWGlueFrame* Frame = CheckFrame(L, 1);
	if (!Frame)
	{
		return LuaError(L, TEXT("SetJustifyV expected a frame."));
	}

	Frame->SetJustifyV(ToFString(L, 2));
	return 0;
}

int32 FWoWGlueLuaRuntime::LuaFrameSetTextLayout(lua_State* L)
{
	FWoWGlueFrame* Frame = CheckFrame(L, 1);
	if (!Frame)
	{
		return LuaError(L, TEXT("SetTextLayout expected a frame."));
	}

	if (!lua_istable(L, 2))
	{
		return LuaError(L, TEXT("SetTextLayout expected an options table."));
	}

	lua_getfield(L, 2, "justifyH");
	if (lua_isstring(L, -1))
	{
		Frame->SetJustifyH(ToFString(L, -1));
	}
	lua_pop(L, 1);

	lua_getfield(L, 2, "firstLineIndent");
	if (lua_isnumber(L, -1))
	{
		Frame->SetTextFirstLineIndent(static_cast<float>(lua_tonumber(L, -1)));
	}
	lua_pop(L, 1);

	lua_getfield(L, 2, "firstLineIndentChars");
	if (lua_isnumber(L, -1))
	{
		Frame->SetTextFirstLineIndentChars(static_cast<float>(lua_tonumber(L, -1)));
	}
	lua_pop(L, 1);

	lua_getfield(L, 2, "paragraphSpacing");
	if (lua_isnumber(L, -1))
	{
		Frame->SetTextParagraphSpacing(static_cast<float>(lua_tonumber(L, -1)));
	}
	lua_pop(L, 1);

	lua_getfield(L, 2, "lineHeight");
	if (lua_isnumber(L, -1))
	{
		Frame->SetTextLineHeightPercentage(static_cast<float>(lua_tonumber(L, -1)));
	}
	lua_pop(L, 1);

	lua_getfield(L, 2, "lineHeightPercentage");
	if (lua_isnumber(L, -1))
	{
		Frame->SetTextLineHeightPercentage(static_cast<float>(lua_tonumber(L, -1)));
	}
	lua_pop(L, 1);

	lua_getfield(L, 2, "justifyLastLine");
	if (lua_isboolean(L, -1))
	{
		Frame->SetTextJustifyLastLine(lua_toboolean(L, -1) != 0);
	}
	lua_pop(L, 1);

	return 0;
}

int32 FWoWGlueLuaRuntime::LuaFrameSetTextFirstLineIndent(lua_State* L)
{
	FWoWGlueFrame* Frame = CheckFrame(L, 1);
	if (!Frame)
	{
		return LuaError(L, TEXT("SetTextFirstLineIndent expected a frame."));
	}

	Frame->SetTextFirstLineIndent(static_cast<float>(luaL_optnumber(L, 2, 0.0)));
	return 0;
}

int32 FWoWGlueLuaRuntime::LuaFrameSetTextFirstLineIndentChars(lua_State* L)
{
	FWoWGlueFrame* Frame = CheckFrame(L, 1);
	if (!Frame)
	{
		return LuaError(L, TEXT("SetTextFirstLineIndentChars expected a frame."));
	}

	Frame->SetTextFirstLineIndentChars(static_cast<float>(luaL_optnumber(L, 2, 0.0)));
	return 0;
}

int32 FWoWGlueLuaRuntime::LuaFrameSetTextParagraphSpacing(lua_State* L)
{
	FWoWGlueFrame* Frame = CheckFrame(L, 1);
	if (!Frame)
	{
		return LuaError(L, TEXT("SetTextParagraphSpacing expected a frame."));
	}

	Frame->SetTextParagraphSpacing(static_cast<float>(luaL_optnumber(L, 2, 0.0)));
	return 0;
}

int32 FWoWGlueLuaRuntime::LuaFrameSetTextLineHeight(lua_State* L)
{
	FWoWGlueFrame* Frame = CheckFrame(L, 1);
	if (!Frame)
	{
		return LuaError(L, TEXT("SetTextLineHeight expected a frame."));
	}

	Frame->SetTextLineHeightPercentage(static_cast<float>(luaL_optnumber(L, 2, 1.08)));
	return 0;
}

int32 FWoWGlueLuaRuntime::LuaFrameSetTextJustifyLastLine(lua_State* L)
{
	FWoWGlueFrame* Frame = CheckFrame(L, 1);
	if (!Frame)
	{
		return LuaError(L, TEXT("SetTextJustifyLastLine expected a frame."));
	}

	Frame->SetTextJustifyLastLine(lua_toboolean(L, 2) != 0);
	return 0;
}

int32 FWoWGlueLuaRuntime::LuaFrameSetScrollChild(lua_State* L)
{
	FWoWGlueFrame* Frame = CheckFrame(L, 1);
	FWoWGlueLuaRuntime* Runtime = GetRuntime(L);
	if (!Frame || !Runtime)
	{
		return LuaError(L, TEXT("SetScrollChild expected a frame."));
	}

	Frame->SetScrollChild(Runtime->ResolveFrameFromLua(L, 2));
	return 0;
}

int32 FWoWGlueLuaRuntime::LuaFrameSetVerticalScroll(lua_State* L)
{
	FWoWGlueFrame* Frame = CheckFrame(L, 1);
	if (!Frame)
	{
		return LuaError(L, TEXT("SetVerticalScroll expected a frame."));
	}

	Frame->SetVerticalScroll(static_cast<float>(luaL_optnumber(L, 2, 0.0)));
	return 0;
}

int32 FWoWGlueLuaRuntime::LuaFrameGetVerticalScroll(lua_State* L)
{
	FWoWGlueFrame* Frame = CheckFrame(L, 1);
	if (!Frame)
	{
		return LuaError(L, TEXT("GetVerticalScroll expected a frame."));
	}

	lua_pushnumber(L, Frame->GetVerticalScroll());
	return 1;
}

int32 FWoWGlueLuaRuntime::LuaFrameGetVerticalScrollRange(lua_State* L)
{
	FWoWGlueFrame* Frame = CheckFrame(L, 1);
	if (!Frame)
	{
		return LuaError(L, TEXT("GetVerticalScrollRange expected a frame."));
	}

	lua_pushnumber(L, Frame->GetVerticalScrollRange());
	return 1;
}

int32 FWoWGlueLuaRuntime::LuaFrameSetScrollBarStyle(lua_State* L)
{
	FWoWGlueFrame* Frame = CheckFrame(L, 1);
	FWoWGlueLuaRuntime* Runtime = GetRuntime(L);
	if (!Frame || !Runtime || !Runtime->GlueManager.IsValid())
	{
		return LuaError(L, TEXT("SetScrollBarStyle expected a frame."));
	}

	if (!lua_istable(L, 2))
	{
		return LuaError(L, TEXT("SetScrollBarStyle expected an options table."));
	}

	FWoWGlueScrollBarStyle Style = Frame->GetScrollBarStyle();
	const int32 OptionsIndex = 2;
	Style.Width = ReadLuaNumberField(L, OptionsIndex, "width", Style.Width);
	Style.ThumbWidth = ReadLuaNumberField(L, OptionsIndex, "thumbWidth", Style.ThumbWidth);
	Style.ThumbMinHeight = ReadLuaNumberField(L, OptionsIndex, "thumbMinHeight", Style.ThumbMinHeight);
	Style.ThumbCapHeight = ReadLuaNumberField(L, OptionsIndex, "thumbCapHeight", Style.ThumbCapHeight);
	Style.ButtonHeight = ReadLuaNumberField(L, OptionsIndex, "buttonHeight", Style.ButtonHeight);
	Style.TrackInset = ReadLuaNumberField(L, OptionsIndex, "trackInset", Style.TrackInset);
	Style.TrackBackgroundWidth = ReadLuaNumberField(L, OptionsIndex, "trackBackgroundWidth", Style.TrackBackgroundWidth);
	Style.TrackBackgroundAlpha = ReadLuaNumberField(L, OptionsIndex, "trackBackgroundAlpha", Style.TrackBackgroundAlpha);
	Style.TrackBackgroundWidth = ReadLuaNumberField(L, OptionsIndex, "trackRailWidth", Style.TrackBackgroundWidth);
	Style.Step = ReadLuaNumberField(L, OptionsIndex, "scrollStep", Style.Step);

	const FString ThumbTopBottomTexturePath = ReadLuaStringField(L, OptionsIndex, "thumbTopBottomTexture");
	if (!ThumbTopBottomTexturePath.IsEmpty())
	{
		Style.ThumbTopBottomTexturePath = ThumbTopBottomTexturePath;
		Style.ThumbTopBottomTexture = Runtime->GlueManager->LoadTexture(ThumbTopBottomTexturePath);
	}

	const FString ThumbMiddleTexturePath = ReadLuaStringField(L, OptionsIndex, "thumbMiddleTexture");
	if (!ThumbMiddleTexturePath.IsEmpty())
	{
		Style.ThumbMiddleTexturePath = ThumbMiddleTexturePath;
		Style.ThumbMiddleTexture = Runtime->GlueManager->LoadTexture(ThumbMiddleTexturePath);
	}

	const FString ArrowTexturePath = ReadLuaStringField(L, OptionsIndex, "arrowTexture");
	if (!ArrowTexturePath.IsEmpty())
	{
		Style.ArrowTexturePath = ArrowTexturePath;
		Style.ArrowTexture = Runtime->GlueManager->LoadTexture(ArrowTexturePath);
	}

	ReadLuaScrollBarVisualState(L, OptionsIndex, "normal", Style.Normal);
	ReadLuaScrollBarVisualState(L, OptionsIndex, "highlight", Style.Highlight);
	ReadLuaScrollBarVisualState(L, OptionsIndex, "pushed", Style.Pushed);

	Frame->SetScrollBarStyle(Style);
	return 0;
}

int32 FWoWGlueLuaRuntime::LuaFrameSetTextGradient(lua_State* L)
{
	FWoWGlueFrame* Frame = CheckFrame(L, 1);
	if (!Frame)
	{
		return LuaError(L, TEXT("SetTextGradient expected a frame."));
	}

	const FString Direction = lua_isnoneornil(L, 2) ? TEXT("VERTICAL") : ToFString(L, 2);
	const FLinearColor StartColor(
		static_cast<float>(luaL_optnumber(L, 3, 1.0)),
		static_cast<float>(luaL_optnumber(L, 4, 1.0)),
		static_cast<float>(luaL_optnumber(L, 5, 1.0)),
		static_cast<float>(luaL_optnumber(L, 6, 1.0)));
	const FLinearColor EndColor(
		static_cast<float>(luaL_optnumber(L, 7, 1.0)),
		static_cast<float>(luaL_optnumber(L, 8, 1.0)),
		static_cast<float>(luaL_optnumber(L, 9, 1.0)),
		static_cast<float>(luaL_optnumber(L, 10, 1.0)));
	Frame->SetTextGradient(Direction, StartColor, EndColor);
	return 0;
}

int32 FWoWGlueLuaRuntime::LuaFrameSetTexture(lua_State* L)
{
	FWoWGlueFrame* Frame = CheckFrame(L, 1);
	FWoWGlueLuaRuntime* Runtime = GetRuntime(L);
	if (!Frame || !Runtime || !Runtime->GlueManager.IsValid())
	{
		return LuaError(L, TEXT("SetTexture expected a frame."));
	}

	const FString TexturePath = ToFString(L, 2);
	const EWoWGlueAlphaMode AlphaMode = ParseAlphaMode(L, 3, EWoWGlueAlphaMode::Blend);
	Frame->SetTexture(Runtime->GlueManager->LoadTexture(TexturePath, AlphaMode), TexturePath, AlphaMode);
	return 0;
}

int32 FWoWGlueLuaRuntime::LuaFrameSetTexCoord(lua_State* L)
{
	FWoWGlueFrame* Frame = CheckFrame(L, 1);
	if (!Frame)
	{
		return LuaError(L, TEXT("SetTexCoord expected a frame."));
	}

	const float Left = static_cast<float>(luaL_checknumber(L, 2));
	const float Right = static_cast<float>(luaL_checknumber(L, 3));
	const float Top = static_cast<float>(luaL_checknumber(L, 4));
	const float Bottom = static_cast<float>(luaL_checknumber(L, 5));
	Frame->SetTexCoord(Left, Right, Top, Bottom);
	return 0;
}

int32 FWoWGlueLuaRuntime::LuaFrameSetBackdrop(lua_State* L)
{
	FWoWGlueFrame* Frame = CheckFrame(L, 1);
	FWoWGlueLuaRuntime* Runtime = GetRuntime(L);
	if (!Frame || !Runtime || !Runtime->GlueManager.IsValid())
	{
		return LuaError(L, TEXT("SetBackdrop expected a frame."));
	}

	FString BackgroundPath;
	FString EdgePath;
	float EdgeSize = 16.0f;
	FMargin Insets(10.0f, 4.0f, 5.0f, 9.0f);

	if (lua_istable(L, 2))
	{
		lua_getfield(L, 2, "bgFile");
		BackgroundPath = ToFString(L, -1);
		lua_pop(L, 1);

		lua_getfield(L, 2, "edgeFile");
		EdgePath = ToFString(L, -1);
		lua_pop(L, 1);

		lua_getfield(L, 2, "edgeSize");
		if (lua_isnumber(L, -1))
		{
			EdgeSize = static_cast<float>(lua_tonumber(L, -1));
		}
		lua_pop(L, 1);

		lua_getfield(L, 2, "insets");
		if (lua_istable(L, -1))
		{
			lua_getfield(L, -1, "left");
			const float Left = lua_isnumber(L, -1) ? static_cast<float>(lua_tonumber(L, -1)) : Insets.Left;
			lua_pop(L, 1);

			lua_getfield(L, -1, "right");
			const float Right = lua_isnumber(L, -1) ? static_cast<float>(lua_tonumber(L, -1)) : Insets.Right;
			lua_pop(L, 1);

			lua_getfield(L, -1, "top");
			const float Top = lua_isnumber(L, -1) ? static_cast<float>(lua_tonumber(L, -1)) : Insets.Top;
			lua_pop(L, 1);

			lua_getfield(L, -1, "bottom");
			const float Bottom = lua_isnumber(L, -1) ? static_cast<float>(lua_tonumber(L, -1)) : Insets.Bottom;
			lua_pop(L, 1);

			Insets = FMargin(Left, Top, Right, Bottom);
		}
		lua_pop(L, 1);
	}
	else
	{
		BackgroundPath = lua_isnoneornil(L, 2) ? FString() : ToFString(L, 2);
		EdgePath = lua_isnoneornil(L, 3) ? FString() : ToFString(L, 3);
		EdgeSize = static_cast<float>(luaL_optnumber(L, 4, EdgeSize));
	}

	UTexture2D* BackgroundTexture = BackgroundPath.IsEmpty() ? nullptr : Runtime->GlueManager->LoadTexture(BackgroundPath);
	UTexture2D* EdgeTexture = EdgePath.IsEmpty() ? nullptr : Runtime->GlueManager->LoadTexture(EdgePath);
	Frame->SetBackdrop(BackgroundTexture, BackgroundPath, EdgeTexture, EdgePath, EdgeSize, Insets);
	return 0;
}

int32 FWoWGlueLuaRuntime::LuaFrameSetNormalTexture(lua_State* L)
{
	FWoWGlueFrame* Frame = CheckFrame(L, 1);
	FWoWGlueLuaRuntime* Runtime = GetRuntime(L);
	if (!Frame || !Runtime || !Runtime->GlueManager.IsValid())
	{
		return LuaError(L, TEXT("SetNormalTexture expected a frame."));
	}

	const FString TexturePath = ToFString(L, 2);
	const EWoWGlueAlphaMode AlphaMode = ParseAlphaMode(L, 3, EWoWGlueAlphaMode::Blend);
	Frame->SetNormalTexture(Runtime->GlueManager->LoadTexture(TexturePath, AlphaMode), TexturePath);
	return 0;
}

int32 FWoWGlueLuaRuntime::LuaFrameSetPushedTexture(lua_State* L)
{
	FWoWGlueFrame* Frame = CheckFrame(L, 1);
	FWoWGlueLuaRuntime* Runtime = GetRuntime(L);
	if (!Frame || !Runtime || !Runtime->GlueManager.IsValid())
	{
		return LuaError(L, TEXT("SetPushedTexture expected a frame."));
	}

	const FString TexturePath = ToFString(L, 2);
	const EWoWGlueAlphaMode AlphaMode = ParseAlphaMode(L, 3, EWoWGlueAlphaMode::Blend);
	Frame->SetPushedTexture(Runtime->GlueManager->LoadTexture(TexturePath, AlphaMode), TexturePath);
	return 0;
}

int32 FWoWGlueLuaRuntime::LuaFrameSetHighlightTexture(lua_State* L)
{
	FWoWGlueFrame* Frame = CheckFrame(L, 1);
	FWoWGlueLuaRuntime* Runtime = GetRuntime(L);
	if (!Frame || !Runtime || !Runtime->GlueManager.IsValid())
	{
		return LuaError(L, TEXT("SetHighlightTexture expected a frame."));
	}

	const FString TexturePath = ToFString(L, 2);
	const EWoWGlueAlphaMode AlphaMode = ParseAlphaMode(L, 3, EWoWGlueAlphaMode::Add);
	Frame->SetHighlightTexture(Runtime->GlueManager->LoadTexture(TexturePath, AlphaMode), TexturePath, AlphaMode);
	return 0;
}

int32 FWoWGlueLuaRuntime::LuaFrameSetDisabledTexture(lua_State* L)
{
	return LuaFrameSetNormalTexture(L);
}

int32 FWoWGlueLuaRuntime::LuaFrameSetScript(lua_State* L)
{
	FWoWGlueFrame* Frame = CheckFrame(L, 1);
	FWoWGlueLuaRuntime* Runtime = GetRuntime(L);
	if (!Frame || !Runtime)
	{
		return LuaError(L, TEXT("SetScript expected a frame."));
	}

	const FString ScriptName = ToFString(L, 2);
	if (!lua_isfunction(L, 3) && !lua_isnil(L, 3))
	{
		return LuaError(L, TEXT("SetScript expected function or nil."));
	}

	FFrameScriptRef& ScriptRef = Runtime->GetFrameScriptRef(*Frame);
	if (ScriptName.Equals(TEXT("OnLoad"), ESearchCase::IgnoreCase))
	{
		Runtime->ReleaseLuaFunctionRef(ScriptRef.OnLoadRef);
		ScriptRef.OnLoadRef = lua_isnil(L, 3) ? INDEX_NONE : Runtime->StoreLuaFunctionRef(L, 3);
	}
	else if (ScriptName.Equals(TEXT("OnClick"), ESearchCase::IgnoreCase))
	{
		Runtime->ReleaseLuaFunctionRef(ScriptRef.OnClickRef);
		ScriptRef.OnClickRef = lua_isnil(L, 3) ? INDEX_NONE : Runtime->StoreLuaFunctionRef(L, 3);
		TSharedPtr<FWoWGlueFrame> SharedFrame = Runtime->GlueManager.IsValid() ? Runtime->GlueManager->FindFrame(Frame->GetName().ToString()) : nullptr;
		if (SharedFrame.IsValid())
		{
			Frame->SetScript(TEXT("OnClick"), [Runtime, WeakFrame = TWeakPtr<FWoWGlueFrame>(SharedFrame)](TSharedRef<FWoWGlueFrame> ClickedFrame)
			{
				if (Runtime && WeakFrame.IsValid())
				{
					FFrameScriptRef& Ref = Runtime->GetFrameScriptRef(ClickedFrame.Get());
					Runtime->InvokeFrameScript(ClickedFrame, Ref.OnClickRef);
				}
			});
		}
	}
	else if (ScriptName.Equals(TEXT("OnDoubleClick"), ESearchCase::IgnoreCase))
	{
		Runtime->ReleaseLuaFunctionRef(ScriptRef.OnDoubleClickRef);
		ScriptRef.OnDoubleClickRef = lua_isnil(L, 3) ? INDEX_NONE : Runtime->StoreLuaFunctionRef(L, 3);
		TSharedPtr<FWoWGlueFrame> SharedFrame = Runtime->GlueManager.IsValid() ? Runtime->GlueManager->FindFrame(Frame->GetName().ToString()) : nullptr;
		if (SharedFrame.IsValid())
		{
			Frame->SetScript(TEXT("OnDoubleClick"), [Runtime, WeakFrame = TWeakPtr<FWoWGlueFrame>(SharedFrame)](TSharedRef<FWoWGlueFrame> ClickedFrame)
			{
				if (Runtime && WeakFrame.IsValid())
				{
					FFrameScriptRef& Ref = Runtime->GetFrameScriptRef(ClickedFrame.Get());
					Runtime->InvokeFrameScript(ClickedFrame, Ref.OnDoubleClickRef);
				}
			});
		}
	}
	else if (ScriptName.Equals(TEXT("OnTextChanged"), ESearchCase::IgnoreCase))
	{
		Runtime->ReleaseLuaFunctionRef(ScriptRef.OnTextChangedRef);
		ScriptRef.OnTextChangedRef = lua_isnil(L, 3) ? INDEX_NONE : Runtime->StoreLuaFunctionRef(L, 3);
		TSharedPtr<FWoWGlueFrame> SharedFrame = Runtime->GlueManager.IsValid() ? Runtime->GlueManager->FindFrame(Frame->GetName().ToString()) : nullptr;
		if (SharedFrame.IsValid())
		{
			Frame->SetScript(TEXT("OnTextChanged"), [Runtime, WeakFrame = TWeakPtr<FWoWGlueFrame>(SharedFrame)](TSharedRef<FWoWGlueFrame> ChangedFrame)
			{
				if (Runtime && WeakFrame.IsValid())
				{
					FFrameScriptRef& Ref = Runtime->GetFrameScriptRef(ChangedFrame.Get());
					Runtime->InvokeFrameScript(ChangedFrame, Ref.OnTextChangedRef);
				}
			});
		}
	}
	else if (ScriptName.Equals(TEXT("OnEditFocusGained"), ESearchCase::IgnoreCase))
	{
		Runtime->ReleaseLuaFunctionRef(ScriptRef.OnEditFocusGainedRef);
		ScriptRef.OnEditFocusGainedRef = lua_isnil(L, 3) ? INDEX_NONE : Runtime->StoreLuaFunctionRef(L, 3);
		TSharedPtr<FWoWGlueFrame> SharedFrame = Runtime->GlueManager.IsValid() ? Runtime->GlueManager->FindFrame(Frame->GetName().ToString()) : nullptr;
		if (SharedFrame.IsValid())
		{
			Frame->SetScript(TEXT("OnEditFocusGained"), [Runtime, WeakFrame = TWeakPtr<FWoWGlueFrame>(SharedFrame)](TSharedRef<FWoWGlueFrame> FocusedFrame)
			{
				if (Runtime && WeakFrame.IsValid())
				{
					FFrameScriptRef& Ref = Runtime->GetFrameScriptRef(FocusedFrame.Get());
					Runtime->InvokeFrameScript(FocusedFrame, Ref.OnEditFocusGainedRef);
				}
			});
		}
	}
	else if (ScriptName.Equals(TEXT("OnEditFocusLost"), ESearchCase::IgnoreCase))
	{
		Runtime->ReleaseLuaFunctionRef(ScriptRef.OnEditFocusLostRef);
		ScriptRef.OnEditFocusLostRef = lua_isnil(L, 3) ? INDEX_NONE : Runtime->StoreLuaFunctionRef(L, 3);
		TSharedPtr<FWoWGlueFrame> SharedFrame = Runtime->GlueManager.IsValid() ? Runtime->GlueManager->FindFrame(Frame->GetName().ToString()) : nullptr;
		if (SharedFrame.IsValid())
		{
			Frame->SetScript(TEXT("OnEditFocusLost"), [Runtime, WeakFrame = TWeakPtr<FWoWGlueFrame>(SharedFrame)](TSharedRef<FWoWGlueFrame> FocusedFrame)
			{
				if (Runtime && WeakFrame.IsValid())
				{
					FFrameScriptRef& Ref = Runtime->GetFrameScriptRef(FocusedFrame.Get());
					Runtime->InvokeFrameScript(FocusedFrame, Ref.OnEditFocusLostRef);
				}
			});
		}
	}
	else if (ScriptName.Equals(TEXT("OnTabPressed"), ESearchCase::IgnoreCase))
	{
		Runtime->ReleaseLuaFunctionRef(ScriptRef.OnTabPressedRef);
		ScriptRef.OnTabPressedRef = lua_isnil(L, 3) ? INDEX_NONE : Runtime->StoreLuaFunctionRef(L, 3);
		TSharedPtr<FWoWGlueFrame> SharedFrame = Runtime->GlueManager.IsValid() ? Runtime->GlueManager->FindFrame(Frame->GetName().ToString()) : nullptr;
		if (SharedFrame.IsValid())
		{
			Frame->SetScript(TEXT("OnTabPressed"), [Runtime, WeakFrame = TWeakPtr<FWoWGlueFrame>(SharedFrame)](TSharedRef<FWoWGlueFrame> FocusedFrame)
			{
				if (Runtime && WeakFrame.IsValid())
				{
					FFrameScriptRef& Ref = Runtime->GetFrameScriptRef(FocusedFrame.Get());
					Runtime->InvokeFrameScript(FocusedFrame, Ref.OnTabPressedRef);
				}
			});
		}
	}
	else if (ScriptName.Equals(TEXT("OnEnter"), ESearchCase::IgnoreCase))
	{
		Runtime->ReleaseLuaFunctionRef(ScriptRef.OnEnterRef);
		ScriptRef.OnEnterRef = lua_isnil(L, 3) ? INDEX_NONE : Runtime->StoreLuaFunctionRef(L, 3);
		TSharedPtr<FWoWGlueFrame> SharedFrame = Runtime->GlueManager.IsValid() ? Runtime->GlueManager->FindFrame(Frame->GetName().ToString()) : nullptr;
		if (SharedFrame.IsValid())
		{
			Frame->SetScript(TEXT("OnEnter"), [Runtime, WeakFrame = TWeakPtr<FWoWGlueFrame>(SharedFrame)](TSharedRef<FWoWGlueFrame> EnteredFrame)
			{
				if (Runtime && WeakFrame.IsValid())
				{
					FFrameScriptRef& Ref = Runtime->GetFrameScriptRef(EnteredFrame.Get());
					Runtime->InvokeFrameScript(EnteredFrame, Ref.OnEnterRef);
				}
			});
		}
	}
	else if (ScriptName.Equals(TEXT("OnLeave"), ESearchCase::IgnoreCase))
	{
		Runtime->ReleaseLuaFunctionRef(ScriptRef.OnLeaveRef);
		ScriptRef.OnLeaveRef = lua_isnil(L, 3) ? INDEX_NONE : Runtime->StoreLuaFunctionRef(L, 3);
		TSharedPtr<FWoWGlueFrame> SharedFrame = Runtime->GlueManager.IsValid() ? Runtime->GlueManager->FindFrame(Frame->GetName().ToString()) : nullptr;
		if (SharedFrame.IsValid())
		{
			Frame->SetScript(TEXT("OnLeave"), [Runtime, WeakFrame = TWeakPtr<FWoWGlueFrame>(SharedFrame)](TSharedRef<FWoWGlueFrame> LeftFrame)
			{
				if (Runtime && WeakFrame.IsValid())
				{
					FFrameScriptRef& Ref = Runtime->GetFrameScriptRef(LeftFrame.Get());
					Runtime->InvokeFrameScript(LeftFrame, Ref.OnLeaveRef);
				}
			});
		}
	}
	else if (ScriptName.Equals(TEXT("OnUpdate"), ESearchCase::IgnoreCase))
	{
		Runtime->ReleaseLuaFunctionRef(ScriptRef.OnUpdateRef);
		ScriptRef.OnUpdateRef = lua_isnil(L, 3) ? INDEX_NONE : Runtime->StoreLuaFunctionRef(L, 3);
	}
	else if (ScriptName.Equals(TEXT("OnShow"), ESearchCase::IgnoreCase))
	{
		Runtime->ReleaseLuaFunctionRef(ScriptRef.OnShowRef);
		ScriptRef.OnShowRef = lua_isnil(L, 3) ? INDEX_NONE : Runtime->StoreLuaFunctionRef(L, 3);
	}
	else if (ScriptName.Equals(TEXT("OnHide"), ESearchCase::IgnoreCase))
	{
		Runtime->ReleaseLuaFunctionRef(ScriptRef.OnHideRef);
		ScriptRef.OnHideRef = lua_isnil(L, 3) ? INDEX_NONE : Runtime->StoreLuaFunctionRef(L, 3);
	}
	else if (ScriptName.Equals(TEXT("OnEvent"), ESearchCase::IgnoreCase))
	{
		Runtime->ReleaseLuaFunctionRef(ScriptRef.OnEventRef);
		ScriptRef.OnEventRef = lua_isnil(L, 3) ? INDEX_NONE : Runtime->StoreLuaFunctionRef(L, 3);
	}
	else if (ScriptName.Equals(TEXT("OnKeyDown"), ESearchCase::IgnoreCase))
	{
		Runtime->ReleaseLuaFunctionRef(ScriptRef.OnKeyDownRef);
		ScriptRef.OnKeyDownRef = lua_isnil(L, 3) ? INDEX_NONE : Runtime->StoreLuaFunctionRef(L, 3);
	}
	else if (ScriptName.Equals(TEXT("OnEnterPressed"), ESearchCase::IgnoreCase))
	{
		Runtime->ReleaseLuaFunctionRef(ScriptRef.OnEnterPressedRef);
		ScriptRef.OnEnterPressedRef = lua_isnil(L, 3) ? INDEX_NONE : Runtime->StoreLuaFunctionRef(L, 3);
		TSharedPtr<FWoWGlueFrame> SharedFrame = Runtime->GlueManager.IsValid() ? Runtime->GlueManager->FindFrame(Frame->GetName().ToString()) : nullptr;
		if (SharedFrame.IsValid())
		{
			Frame->SetScript(TEXT("OnEnterPressed"), [Runtime, WeakFrame = TWeakPtr<FWoWGlueFrame>(SharedFrame)](TSharedRef<FWoWGlueFrame> FocusedFrame)
			{
				if (Runtime && WeakFrame.IsValid())
				{
					FFrameScriptRef& Ref = Runtime->GetFrameScriptRef(FocusedFrame.Get());
					Runtime->InvokeFrameScript(FocusedFrame, Ref.OnEnterPressedRef);
				}
			});
		}
	}
	else if (ScriptName.Equals(TEXT("OnEscapePressed"), ESearchCase::IgnoreCase))
	{
		Runtime->ReleaseLuaFunctionRef(ScriptRef.OnEscapePressedRef);
		ScriptRef.OnEscapePressedRef = lua_isnil(L, 3) ? INDEX_NONE : Runtime->StoreLuaFunctionRef(L, 3);
		TSharedPtr<FWoWGlueFrame> SharedFrame = Runtime->GlueManager.IsValid() ? Runtime->GlueManager->FindFrame(Frame->GetName().ToString()) : nullptr;
		if (SharedFrame.IsValid())
		{
			Frame->SetScript(TEXT("OnEscapePressed"), [Runtime, WeakFrame = TWeakPtr<FWoWGlueFrame>(SharedFrame)](TSharedRef<FWoWGlueFrame> FocusedFrame)
			{
				if (Runtime && WeakFrame.IsValid())
				{
					FFrameScriptRef& Ref = Runtime->GetFrameScriptRef(FocusedFrame.Get());
					Runtime->InvokeFrameScript(FocusedFrame, Ref.OnEscapePressedRef);
				}
			});
		}
	}
	else if (ScriptName.Equals(TEXT("OnDisable"), ESearchCase::IgnoreCase))
	{
		Runtime->ReleaseLuaFunctionRef(ScriptRef.OnDisableRef);
		ScriptRef.OnDisableRef = lua_isnil(L, 3) ? INDEX_NONE : Runtime->StoreLuaFunctionRef(L, 3);
	}
	else if (ScriptName.Equals(TEXT("OnEnable"), ESearchCase::IgnoreCase))
	{
		Runtime->ReleaseLuaFunctionRef(ScriptRef.OnEnableRef);
		ScriptRef.OnEnableRef = lua_isnil(L, 3) ? INDEX_NONE : Runtime->StoreLuaFunctionRef(L, 3);
	}
	else if (ScriptName.Equals(TEXT("OnHyperlinkClick"), ESearchCase::IgnoreCase))
	{
		Runtime->ReleaseLuaFunctionRef(ScriptRef.OnHyperlinkClickRef);
		ScriptRef.OnHyperlinkClickRef = lua_isnil(L, 3) ? INDEX_NONE : Runtime->StoreLuaFunctionRef(L, 3);
		TSharedPtr<FWoWGlueFrame> SharedFrame = Runtime->GlueManager.IsValid() ? Runtime->GlueManager->FindFrame(Frame->GetName().ToString()) : nullptr;
		if (SharedFrame.IsValid())
		{
			Frame->SetHyperlinkScript([Runtime, WeakFrame = TWeakPtr<FWoWGlueFrame>(SharedFrame)](TSharedRef<FWoWGlueFrame> ClickedFrame, const FString& Link, const FString& Text)
			{
				if (Runtime && WeakFrame.IsValid())
				{
					FFrameScriptRef& Ref = Runtime->GetFrameScriptRef(ClickedFrame.Get());
					Runtime->InvokeFrameScriptWithHyperlink(ClickedFrame, Ref.OnHyperlinkClickRef, Link, Text);
				}
			});
		}
	}
	return 0;
}

int32 FWoWGlueLuaRuntime::LuaFrameRegisterEvent(lua_State* L)
{
	CheckFrame(L, 1);
	return 0;
}

int32 FWoWGlueLuaRuntime::LuaFrameUnregisterEvent(lua_State* L)
{
	CheckFrame(L, 1);
	return 0;
}

int32 FWoWGlueLuaRuntime::LuaFrameRaise(lua_State* L)
{
	if (FWoWGlueFrame* Frame = CheckFrame(L, 1))
	{
		Frame->Raise();
	}
	return 0;
}

int32 FWoWGlueLuaRuntime::LuaFrameClick(lua_State* L)
{
	if (FWoWGlueFrame* Frame = CheckFrame(L, 1))
	{
		Frame->Click();
	}
	return 0;
}

int32 FWoWGlueLuaRuntime::LuaFrameSetFocus(lua_State* L)
{
	if (FWoWGlueFrame* Frame = CheckFrame(L, 1))
	{
		Frame->SetKeyboardFocus();
	}
	return 0;
}

int32 FWoWGlueLuaRuntime::LuaFrameSetKeepKeyboardFocus(lua_State* L)
{
	if (FWoWGlueFrame* Frame = CheckFrame(L, 1))
	{
		Frame->SetKeepKeyboardFocus(lua_toboolean(L, 2) != 0);
	}
	return 0;
}

int32 FWoWGlueLuaRuntime::LuaFrameClearAllPoints(lua_State* L)
{
	if (FWoWGlueFrame* Frame = CheckFrame(L, 1))
	{
		Frame->ClearAllPoints();
	}
	return 0;
}

int32 FWoWGlueLuaRuntime::LuaFrameSetMaxLetters(lua_State* L)
{
	CheckFrame(L, 1);
	return 0;
}

int32 FWoWGlueLuaRuntime::LuaFrameSetMaxBytes(lua_State* L)
{
	CheckFrame(L, 1);
	return 0;
}

int32 FWoWGlueLuaRuntime::LuaFrameShow(lua_State* L)
{
	FWoWGlueFrame* Frame = CheckFrame(L, 1);
	FWoWGlueLuaRuntime* Runtime = GetRuntime(L);
	if (Frame)
	{
		Frame->Show();
		if (Runtime && Runtime->GlueManager.IsValid())
		{
			TSharedPtr<FWoWGlueFrame> SharedFrame = Runtime->GlueManager->FindFrame(Frame->GetName().ToString());
			if (SharedFrame.IsValid())
			{
				FFrameScriptRef& Ref = Runtime->GetFrameScriptRef(*Frame);
				Runtime->InvokeFrameScript(SharedFrame.ToSharedRef(), Ref.OnShowRef);
			}
		}
	}
	return 0;
}

int32 FWoWGlueLuaRuntime::LuaFrameHide(lua_State* L)
{
	FWoWGlueFrame* Frame = CheckFrame(L, 1);
	FWoWGlueLuaRuntime* Runtime = GetRuntime(L);
	if (Frame)
	{
		Frame->Hide();
		if (Runtime && Runtime->GlueManager.IsValid())
		{
			TSharedPtr<FWoWGlueFrame> SharedFrame = Runtime->GlueManager->FindFrame(Frame->GetName().ToString());
			if (SharedFrame.IsValid())
			{
				FFrameScriptRef& Ref = Runtime->GetFrameScriptRef(*Frame);
				Runtime->InvokeFrameScript(SharedFrame.ToSharedRef(), Ref.OnHideRef);
			}
		}
	}
	return 0;
}

int32 FWoWGlueLuaRuntime::LuaFrameIsShown(lua_State* L)
{
	FWoWGlueFrame* Frame = CheckFrame(L, 1);
	if (!Frame)
	{
		return LuaError(L, TEXT("IsShown expected a frame."));
	}
	lua_pushboolean(L, Frame->IsShown());
	return 1;
}

int32 FWoWGlueLuaRuntime::LuaFrameEnable(lua_State* L)
{
	FWoWGlueFrame* Frame = CheckFrame(L, 1);
	FWoWGlueLuaRuntime* Runtime = GetRuntime(L);
	if (Frame)
	{
		Frame->SetEnabled(true);
		if (Runtime && Runtime->GlueManager.IsValid())
		{
			TSharedPtr<FWoWGlueFrame> SharedFrame = Runtime->GlueManager->FindFrame(Frame->GetName().ToString());
			if (SharedFrame.IsValid())
			{
				FFrameScriptRef& Ref = Runtime->GetFrameScriptRef(*Frame);
				Runtime->InvokeFrameScript(SharedFrame.ToSharedRef(), Ref.OnEnableRef);
			}
		}
	}
	return 0;
}

int32 FWoWGlueLuaRuntime::LuaFrameDisable(lua_State* L)
{
	FWoWGlueFrame* Frame = CheckFrame(L, 1);
	FWoWGlueLuaRuntime* Runtime = GetRuntime(L);
	if (Frame)
	{
		Frame->SetEnabled(false);
		if (Runtime && Runtime->GlueManager.IsValid())
		{
			TSharedPtr<FWoWGlueFrame> SharedFrame = Runtime->GlueManager->FindFrame(Frame->GetName().ToString());
			if (SharedFrame.IsValid())
			{
				FFrameScriptRef& Ref = Runtime->GetFrameScriptRef(*Frame);
				Runtime->InvokeFrameScript(SharedFrame.ToSharedRef(), Ref.OnDisableRef);
			}
		}
	}
	return 0;
}

int32 FWoWGlueLuaRuntime::LuaFrameSetEnabled(lua_State* L)
{
	FWoWGlueFrame* Frame = CheckFrame(L, 1);
	FWoWGlueLuaRuntime* Runtime = GetRuntime(L);
	if (Frame)
	{
		const bool bEnabled = lua_toboolean(L, 2) != 0;
		Frame->SetEnabled(bEnabled);
		if (Runtime && Runtime->GlueManager.IsValid())
		{
			TSharedPtr<FWoWGlueFrame> SharedFrame = Runtime->GlueManager->FindFrame(Frame->GetName().ToString());
			if (SharedFrame.IsValid())
			{
				FFrameScriptRef& Ref = Runtime->GetFrameScriptRef(*Frame);
				Runtime->InvokeFrameScript(SharedFrame.ToSharedRef(), bEnabled ? Ref.OnEnableRef : Ref.OnDisableRef);
			}
		}
	}
	return 0;
}

int32 FWoWGlueLuaRuntime::LuaFrameIsEnabled(lua_State* L)
{
	FWoWGlueFrame* Frame = CheckFrame(L, 1);
	if (!Frame)
	{
		return LuaError(L, TEXT("IsEnabled expected a frame."));
	}
	lua_pushboolean(L, Frame->IsEnabled());
	return 1;
}

int32 FWoWGlueLuaRuntime::LuaFrameSetDrawLayer(lua_State* L)
{
	if (FWoWGlueFrame* Frame = CheckFrame(L, 1))
	{
		Frame->SetDrawLayer(static_cast<int32>(luaL_checkinteger(L, 2)));
	}
	return 0;
}

void FWoWGlueLuaRuntime::RegisterGlobals()
{
	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaPrint);
	lua_setglobal(LuaState, "print");

	lua_getglobal(LuaState, "string");
	lua_getfield(LuaState, -1, "format");
	lua_setglobal(LuaState, "format");
	lua_getfield(LuaState, -1, "len");
	lua_setglobal(LuaState, "strlen");
	lua_pop(LuaState, 1);

	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaCreateFrame);
	lua_setglobal(LuaState, "CreateFrame");

	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaGlueLogin);
	lua_setglobal(LuaState, "GlueLogin");

	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaLaunchURL);
	lua_setglobal(LuaState, "LaunchURL");

	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaSetGlueScreen);
	lua_setglobal(LuaState, "SetGlueScreen");

	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaGetGlueScreen);
	lua_setglobal(LuaState, "GetGlueScreen");

	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaGetScreenWidth);
	lua_setglobal(LuaState, "GetScreenWidth");

	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaGetScreenHeight);
	lua_setglobal(LuaState, "GetScreenHeight");

	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaGetAccountError);
	lua_setglobal(LuaState, "GetAccountError");

	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaGetNumCharacters);
	lua_setglobal(LuaState, "GetNumCharacters");

	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaGetCharacterInfo);
	lua_setglobal(LuaState, "GetCharacterInfo");

	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaGetCharacterSummary);
	lua_setglobal(LuaState, "GetCharacterSummary");

	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaGetNumRealms);
	lua_setglobal(LuaState, "GetNumRealms");

	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaGetRealmSummary);
	lua_setglobal(LuaState, "GetRealmSummary");

	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaSelectRealm);
	lua_setglobal(LuaState, "SelectRealm");

	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaChangeRealm);
	lua_setglobal(LuaState, "ChangeRealm");

	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaCancelRealmList);
	lua_setglobal(LuaState, "CancelRealmList");

	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaCancelRealmList);
	lua_setglobal(LuaState, "RealmListDialogCancelled");

	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaSelectCharacter);
	lua_setglobal(LuaState, "SelectCharacter");

	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaEnterWorld);
	lua_setglobal(LuaState, "EnterWorld");

	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaCreateAzerothCharacter);
	lua_setglobal(LuaState, "CreateAzerothCharacter");

	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaGetDefaultCharacterCreateRace);
	lua_setglobal(LuaState, "GetDefaultCharacterCreateRace");

	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaGetDefaultCharacterCreateClass);
	lua_setglobal(LuaState, "GetDefaultCharacterCreateClass");

	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaGetDefaultCharacterCreateGender);
	lua_setglobal(LuaState, "GetDefaultCharacterCreateGender");

	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaSetBackgroundModel);
	lua_setglobal(LuaState, "SetBackgroundModel");

	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaSetCharCustomizeBackground);
	lua_setglobal(LuaState, "SetCharCustomizeBackground");
}

void FWoWGlueLuaRuntime::RegisterFrameMetatable()
{
	luaL_newmetatable(LuaState, FrameMetatableName);
	lua_newtable(LuaState);

	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaFrameSetPoint);
	lua_setfield(LuaState, -2, "SetPoint");
	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaFrameSetSize);
	lua_setfield(LuaState, -2, "SetSize");
	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaFrameGetSize);
	lua_setfield(LuaState, -2, "GetSize");
	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaFrameSetWidth);
	lua_setfield(LuaState, -2, "SetWidth");
	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaFrameSetHeight);
	lua_setfield(LuaState, -2, "SetHeight");
	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaFrameGetWidth);
	lua_setfield(LuaState, -2, "GetWidth");
	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaFrameGetHeight);
	lua_setfield(LuaState, -2, "GetHeight");
	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaFrameSetText);
	lua_setfield(LuaState, -2, "SetText");
	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaFrameGetText);
	lua_setfield(LuaState, -2, "GetText");
	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaFrameGetName);
	lua_setfield(LuaState, -2, "GetName");
	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaFrameGetID);
	lua_setfield(LuaState, -2, "GetID");
	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaFrameSetFormattedText);
	lua_setfield(LuaState, -2, "SetFormattedText");
	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaFrameGetBoundsRect);
	lua_setfield(LuaState, -2, "GetBoundsRect");
	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaFrameSetAlpha);
	lua_setfield(LuaState, -2, "SetAlpha");
	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaFrameGetAlpha);
	lua_setfield(LuaState, -2, "GetAlpha");
	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaFrameSetVertexColor);
	lua_setfield(LuaState, -2, "SetVertexColor");
	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaFrameSetPassword);
	lua_setfield(LuaState, -2, "SetPassword");
	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaFrameSetPlaceholderText);
	lua_setfield(LuaState, -2, "SetPlaceholderText");
	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaFrameSetPlaceholderTextColor);
	lua_setfield(LuaState, -2, "SetPlaceholderTextColor");
	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaFrameSetPlaceholderJustifyH);
	lua_setfield(LuaState, -2, "SetPlaceholderJustifyH");
	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaFrameSetTextInsets);
	lua_setfield(LuaState, -2, "SetTextInsets");
	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaFrameSetTextColor);
	lua_setfield(LuaState, -2, "SetTextColor");
	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaFrameSetFont);
	lua_setfield(LuaState, -2, "SetFont");
	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaFrameSetFontObject);
	lua_setfield(LuaState, -2, "SetFontObject");
	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaFrameSetShadowColor);
	lua_setfield(LuaState, -2, "SetShadowColor");
	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaFrameSetShadowOffset);
	lua_setfield(LuaState, -2, "SetShadowOffset");
	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaFrameSetJustifyH);
	lua_setfield(LuaState, -2, "SetJustifyH");
	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaFrameSetJustifyV);
	lua_setfield(LuaState, -2, "SetJustifyV");
	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaFrameSetTextLayout);
	lua_setfield(LuaState, -2, "SetTextLayout");
	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaFrameSetTextFirstLineIndent);
	lua_setfield(LuaState, -2, "SetTextFirstLineIndent");
	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaFrameSetTextFirstLineIndentChars);
	lua_setfield(LuaState, -2, "SetTextFirstLineIndentChars");
	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaFrameSetTextParagraphSpacing);
	lua_setfield(LuaState, -2, "SetTextParagraphSpacing");
	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaFrameSetTextLineHeight);
	lua_setfield(LuaState, -2, "SetTextLineHeight");
	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaFrameSetTextJustifyLastLine);
	lua_setfield(LuaState, -2, "SetTextJustifyLastLine");
	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaFrameSetScrollChild);
	lua_setfield(LuaState, -2, "SetScrollChild");
	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaFrameSetVerticalScroll);
	lua_setfield(LuaState, -2, "SetVerticalScroll");
	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaFrameGetVerticalScroll);
	lua_setfield(LuaState, -2, "GetVerticalScroll");
	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaFrameGetVerticalScrollRange);
	lua_setfield(LuaState, -2, "GetVerticalScrollRange");
	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaFrameSetScrollBarStyle);
	lua_setfield(LuaState, -2, "SetScrollBarStyle");
	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaFrameSetTextGradient);
	lua_setfield(LuaState, -2, "SetTextGradient");
	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaFrameSetTexture);
	lua_setfield(LuaState, -2, "SetTexture");
	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaFrameSetTexCoord);
	lua_setfield(LuaState, -2, "SetTexCoord");
	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaFrameSetBackdrop);
	lua_setfield(LuaState, -2, "SetBackdrop");
	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaFrameSetNormalTexture);
	lua_setfield(LuaState, -2, "SetNormalTexture");
	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaFrameSetPushedTexture);
	lua_setfield(LuaState, -2, "SetPushedTexture");
	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaFrameSetHighlightTexture);
	lua_setfield(LuaState, -2, "SetHighlightTexture");
	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaFrameSetDisabledTexture);
	lua_setfield(LuaState, -2, "SetDisabledTexture");
	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaFrameSetScript);
	lua_setfield(LuaState, -2, "SetScript");
	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaFrameRegisterEvent);
	lua_setfield(LuaState, -2, "RegisterEvent");
	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaFrameUnregisterEvent);
	lua_setfield(LuaState, -2, "UnregisterEvent");
	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaFrameRaise);
	lua_setfield(LuaState, -2, "Raise");
	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaFrameClick);
	lua_setfield(LuaState, -2, "Click");
	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaFrameSetFocus);
	lua_setfield(LuaState, -2, "SetFocus");
	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaFrameSetKeepKeyboardFocus);
	lua_setfield(LuaState, -2, "SetKeepKeyboardFocus");
	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaFrameClearAllPoints);
	lua_setfield(LuaState, -2, "ClearAllPoints");
	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaFrameSetMaxLetters);
	lua_setfield(LuaState, -2, "SetMaxLetters");
	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaFrameSetMaxBytes);
	lua_setfield(LuaState, -2, "SetMaxBytes");
	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaFrameShow);
	lua_setfield(LuaState, -2, "Show");
	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaFrameHide);
	lua_setfield(LuaState, -2, "Hide");
	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaFrameIsShown);
	lua_setfield(LuaState, -2, "IsShown");
	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaFrameEnable);
	lua_setfield(LuaState, -2, "Enable");
	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaFrameDisable);
	lua_setfield(LuaState, -2, "Disable");
	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaFrameSetEnabled);
	lua_setfield(LuaState, -2, "SetEnabled");
	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaFrameIsEnabled);
	lua_setfield(LuaState, -2, "IsEnabled");
	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaFrameSetDrawLayer);
	lua_setfield(LuaState, -2, "SetDrawLayer");

	lua_setfield(LuaState, -2, FrameMethodsMetatableKey);
	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaFrameIndex);
	lua_setfield(LuaState, -2, "__index");
	lua_pushcfunction(LuaState, &FWoWGlueLuaRuntime::LuaFrameNewIndex);
	lua_setfield(LuaState, -2, "__newindex");
	lua_pop(LuaState, 1);
}

void FWoWGlueLuaRuntime::RegisterFrameGlobal(TSharedRef<FWoWGlueFrame> Frame)
{
	const FString Name = Frame->GetName().ToString();
	if (!Name.IsEmpty() && !Name.StartsWith(TEXT("$unnamed_")))
	{
		PushFrame(LuaState, Frame);
		lua_setglobal(LuaState, TCHAR_TO_UTF8(*Name));
	}
}

TSharedPtr<FWoWGlueFrame> FWoWGlueLuaRuntime::ResolveFrameFromLua(lua_State* L, int32 StackIndex) const
{
	if (!GlueManager.IsValid() || lua_isnoneornil(L, StackIndex))
	{
		return GlueManager.IsValid()
			? TSharedPtr<FWoWGlueFrame>(GlueManager->GetUIParent())
			: nullptr;
	}

	if (lua_isuserdata(L, StackIndex))
	{
		if (FWoWGlueFrame* Frame = CheckFrame(L, StackIndex))
		{
			return GlueManager->FindFrame(Frame->GetName().ToString());
		}
	}

	if (lua_isstring(L, StackIndex))
	{
		return GlueManager->FindFrame(ToFString(L, StackIndex));
	}

	return TSharedPtr<FWoWGlueFrame>(GlueManager->GetUIParent());
}

void FWoWGlueLuaRuntime::InvokeFrameScript(TSharedRef<FWoWGlueFrame> Frame, int32 FunctionRef)
{
	if (!LuaState || FunctionRef == INDEX_NONE)
	{
		return;
	}

	lua_rawgeti(LuaState, LUA_REGISTRYINDEX, FunctionRef);
	PushFrame(LuaState, Frame);
	const int32 Result = lua_pcall(LuaState, 1, 0, 0);
	if (Result != 0)
	{
		const FString Error = ToFString(LuaState, -1);
		UE_LOG(LogTemp, Error, TEXT("[GlueLua] script error: %s"), *Error);
		if (ErrorHandler)
		{
			ErrorHandler(Error);
		}
		lua_pop(LuaState, 1);
	}
}

void FWoWGlueLuaRuntime::InvokeFrameScriptWithElapsed(TSharedRef<FWoWGlueFrame> Frame, int32 FunctionRef, float ElapsedSeconds)
{
	if (!LuaState || FunctionRef == INDEX_NONE)
	{
		return;
	}

	lua_rawgeti(LuaState, LUA_REGISTRYINDEX, FunctionRef);
	PushFrame(LuaState, Frame);
	lua_pushnumber(LuaState, ElapsedSeconds);
	const int32 Result = lua_pcall(LuaState, 2, 0, 0);
	if (Result != 0)
	{
		const FString Error = ToFString(LuaState, -1);
		UE_LOG(LogTemp, Error, TEXT("[GlueLua] OnUpdate error: %s"), *Error);
		if (ErrorHandler)
		{
			ErrorHandler(Error);
		}
		lua_pop(LuaState, 1);
	}
}

void FWoWGlueLuaRuntime::InvokeFrameScriptWithString(TSharedRef<FWoWGlueFrame> Frame, int32 FunctionRef, const FString& Value)
{
	if (!LuaState || FunctionRef == INDEX_NONE)
	{
		return;
	}

	lua_rawgeti(LuaState, LUA_REGISTRYINDEX, FunctionRef);
	PushFrame(LuaState, Frame);
	PushFString(LuaState, Value);
	const int32 Result = lua_pcall(LuaState, 2, 0, 0);
	if (Result != 0)
	{
		const FString Error = ToFString(LuaState, -1);
		UE_LOG(LogTemp, Error, TEXT("[GlueLua] script string event error: %s"), *Error);
		if (ErrorHandler)
		{
			ErrorHandler(Error);
		}
		lua_pop(LuaState, 1);
	}
}

void FWoWGlueLuaRuntime::InvokeFrameScriptWithEvent(TSharedRef<FWoWGlueFrame> Frame, int32 FunctionRef, const FString& EventName)
{
	InvokeFrameScriptWithString(Frame, FunctionRef, EventName);
}

void FWoWGlueLuaRuntime::InvokeFrameScriptWithHyperlink(TSharedRef<FWoWGlueFrame> Frame, int32 FunctionRef, const FString& Link, const FString& Text)
{
	if (!LuaState || FunctionRef == INDEX_NONE)
	{
		return;
	}

	lua_rawgeti(LuaState, LUA_REGISTRYINDEX, FunctionRef);
	PushFrame(LuaState, Frame);
	PushFString(LuaState, Link);
	PushFString(LuaState, Text);
	lua_pushstring(LuaState, "LeftButton");
	const int32 Result = lua_pcall(LuaState, 4, 0, 0);
	if (Result != 0)
	{
		const FString Error = ToFString(LuaState, -1);
		UE_LOG(LogTemp, Error, TEXT("[GlueLua] OnHyperlinkClick error: %s"), *Error);
		if (ErrorHandler)
		{
			ErrorHandler(Error);
		}
		lua_pop(LuaState, 1);
	}
}

void FWoWGlueLuaRuntime::Tick(float DeltaSeconds)
{
	if (!LuaState || !GlueManager.IsValid())
	{
		return;
	}

	TArray<TPair<TSharedRef<FWoWGlueFrame>, int32>> Updates;
	for (const TPair<FWoWGlueFrame*, FFrameScriptRef>& Pair : FrameScriptRefs)
	{
		if (Pair.Value.OnUpdateRef == INDEX_NONE || !Pair.Key)
		{
			continue;
		}
		TSharedPtr<FWoWGlueFrame> Frame = GlueManager->FindFrame(Pair.Key->GetName().ToString());
		if (Frame.IsValid())
		{
			Updates.Emplace(Frame.ToSharedRef(), Pair.Value.OnUpdateRef);
		}
	}

	for (const TPair<TSharedRef<FWoWGlueFrame>, int32>& Update : Updates)
	{
		InvokeFrameScriptWithElapsed(Update.Key, Update.Value, DeltaSeconds);
	}
}

int32 FWoWGlueLuaRuntime::StoreLuaFunctionRef(lua_State* L, int32 StackIndex)
{
	lua_pushvalue(L, StackIndex);
	return luaL_ref(L, LUA_REGISTRYINDEX);
}

void FWoWGlueLuaRuntime::ReleaseLuaFunctionRef(int32& FunctionRef)
{
	if (LuaState && FunctionRef != INDEX_NONE)
	{
		luaL_unref(LuaState, LUA_REGISTRYINDEX, FunctionRef);
		FunctionRef = INDEX_NONE;
	}
}

FWoWGlueLuaRuntime::FFrameScriptRef& FWoWGlueLuaRuntime::GetFrameScriptRef(FWoWGlueFrame& Frame)
{
	return FrameScriptRefs.FindOrAdd(&Frame);
}
