#pragma once

#include "CoreMinimal.h"
#include "WoWGlueUI.h"

struct lua_State;

struct FWoWGlueXmlObjectNode
{
	FString Tag;
	TMap<FString, FString> Attributes;
	TArray<TSharedPtr<FWoWGlueXmlObjectNode>> Children;
	FString Content;
};

struct FWoWGlueXmlTemplateInfo
{
	FWoWGlueXmlObjectNode Node;
};

class WOW_API FWoWGlueLuaRuntime : public TSharedFromThis<FWoWGlueLuaRuntime>
{
public:
	using FGlueLoginHandler = TFunction<void(const FString& AccountName, const FString& Password)>;
	using FGlueScreenHandler = TFunction<void(const FString& ScreenName)>;
	using FGlueCharacterSelectHandler = TFunction<void(int32 CharacterGuid)>;
	using FGlueEnterWorldHandler = TFunction<void()>;
	using FGlueCharacterCreateHandler = TFunction<void(const FString& CharacterName, int32 Race, int32 ClassId, int32 Gender, int32 Skin, int32 Face, int32 HairStyle, int32 HairColor, int32 FacialStyle)>;
	using FGlueRealmSelectHandler = TFunction<void(int32 RealmIndex)>;
	using FGlueRealmCancelHandler = TFunction<void()>;
	using FGlueLuaErrorHandler = TFunction<void(const FString& ErrorText)>;
	using FGlueBackgroundModelHandler = TFunction<void(const FString& ModelPath, int32 Race, int32 Gender)>;
	using FGlueDebugLogHandler = TFunction<void(const FString& Message)>;

	FWoWGlueLuaRuntime();
	~FWoWGlueLuaRuntime();

	void Initialize(TSharedRef<FWoWGlueUIManager> InGlueManager);
	bool LoadFile(const FString& ScriptPath, FString* OutError = nullptr);
	bool LoadXML(const FString& XmlPath, FString* OutError = nullptr);
	bool LoadTOC(const FString& TocPath, FString* OutError = nullptr);
	void Shutdown();
	bool IsInitialized() const { return LuaState != nullptr; }
	void SetGlueLoginHandler(FGlueLoginHandler Handler);
	void SetGlueScreenHandler(FGlueScreenHandler Handler);
	void SetCharacterSelectHandler(FGlueCharacterSelectHandler Handler);
	void SetEnterWorldHandler(FGlueEnterWorldHandler Handler);
	void SetCharacterCreateHandler(FGlueCharacterCreateHandler Handler);
	void SetRealmSelectHandler(FGlueRealmSelectHandler Handler);
	void SetRealmCancelHandler(FGlueRealmCancelHandler Handler);
	void SetErrorHandler(FGlueLuaErrorHandler Handler);
	void SetBackgroundModelHandler(FGlueBackgroundModelHandler Handler);
	void SetDebugLogHandler(FGlueDebugLogHandler Handler);
	void Tick(float DeltaSeconds);
	void SetCurrentGlueScreen(const FString& ScreenName);
	const FString& GetCurrentGlueScreen() const { return CurrentGlueScreen; }
	void SetAccountErrorText(const FString& ErrorText);
	void SetCharacterList(const TArray<FString>& CharacterRowsTsv);
	void SetRealmList(const TArray<FString>& RealmRowsTsv);
	void SetCharacterCreateDefaults(int32 Race, int32 ClassId, int32 Gender);
	bool GetGlobalString(const FString& Name, FString& OutValue) const;
	bool CallGlobalFunction(const FString& FunctionName, const TArray<FString>& StringArgs = TArray<FString>(), FString* OutError = nullptr);
	FString BuildCharacterInfoDebugDump() const;

private:
	struct FFrameScriptRef
	{
		int32 OnLoadRef = INDEX_NONE;
		int32 OnClickRef = INDEX_NONE;
		int32 OnDoubleClickRef = INDEX_NONE;
		int32 OnTextChangedRef = INDEX_NONE;
		int32 OnEditFocusGainedRef = INDEX_NONE;
		int32 OnEditFocusLostRef = INDEX_NONE;
		int32 OnTabPressedRef = INDEX_NONE;
		int32 OnEnterRef = INDEX_NONE;
		int32 OnLeaveRef = INDEX_NONE;
		int32 OnUpdateRef = INDEX_NONE;
		int32 OnShowRef = INDEX_NONE;
		int32 OnHideRef = INDEX_NONE;
		int32 OnEventRef = INDEX_NONE;
		int32 OnKeyDownRef = INDEX_NONE;
		int32 OnEnterPressedRef = INDEX_NONE;
		int32 OnEscapePressedRef = INDEX_NONE;
		int32 OnDisableRef = INDEX_NONE;
		int32 OnEnableRef = INDEX_NONE;
		int32 OnHyperlinkClickRef = INDEX_NONE;
	};

	static FWoWGlueLuaRuntime* GetRuntime(lua_State* L);
	static FWoWGlueFrame* CheckFrame(lua_State* L, int32 StackIndex);
	static void PushFrame(lua_State* L, TSharedRef<FWoWGlueFrame> Frame);
	static FString ToFString(lua_State* L, int32 StackIndex);
	static void PushFString(lua_State* L, const FString& Value);
	static int32 LuaFrameIndex(lua_State* L);
	static int32 LuaFrameNewIndex(lua_State* L);
	static int32 LuaPrint(lua_State* L);
	static int32 LuaCreateFrame(lua_State* L);
	static int32 LuaGlueLogin(lua_State* L);
	static int32 LuaLaunchURL(lua_State* L);
	static int32 LuaSetGlueScreen(lua_State* L);
	static int32 LuaGetGlueScreen(lua_State* L);
	static int32 LuaGetScreenWidth(lua_State* L);
	static int32 LuaGetScreenHeight(lua_State* L);
	static int32 LuaGetAccountError(lua_State* L);
	static int32 LuaGetNumCharacters(lua_State* L);
	static int32 LuaGetCharacterInfo(lua_State* L);
	static int32 LuaGetCharacterSummary(lua_State* L);
	static int32 LuaGetNumRealms(lua_State* L);
	static int32 LuaGetRealmSummary(lua_State* L);
	static int32 LuaSelectRealm(lua_State* L);
	static int32 LuaChangeRealm(lua_State* L);
	static int32 LuaCancelRealmList(lua_State* L);
	static int32 LuaSelectCharacter(lua_State* L);
	static int32 LuaEnterWorld(lua_State* L);
	static int32 LuaCreateAzerothCharacter(lua_State* L);
	static int32 LuaGetDefaultCharacterCreateRace(lua_State* L);
	static int32 LuaGetDefaultCharacterCreateClass(lua_State* L);
	static int32 LuaGetDefaultCharacterCreateGender(lua_State* L);
	static int32 LuaSetBackgroundModel(lua_State* L);
	static int32 LuaSetCharCustomizeBackground(lua_State* L);
	static int32 LuaFrameSetPoint(lua_State* L);
	static int32 LuaFrameSetSize(lua_State* L);
	static int32 LuaFrameGetSize(lua_State* L);
	static int32 LuaFrameSetWidth(lua_State* L);
	static int32 LuaFrameSetHeight(lua_State* L);
	static int32 LuaFrameGetWidth(lua_State* L);
	static int32 LuaFrameGetHeight(lua_State* L);
	static int32 LuaFrameSetText(lua_State* L);
	static int32 LuaFrameGetText(lua_State* L);
	static int32 LuaFrameGetName(lua_State* L);
	static int32 LuaFrameGetID(lua_State* L);
	static int32 LuaFrameSetFormattedText(lua_State* L);
	static int32 LuaFrameGetBoundsRect(lua_State* L);
	static int32 LuaFrameSetAlpha(lua_State* L);
	static int32 LuaFrameGetAlpha(lua_State* L);
	static int32 LuaFrameSetVertexColor(lua_State* L);
	static int32 LuaFrameSetPassword(lua_State* L);
	static int32 LuaFrameSetPlaceholderText(lua_State* L);
	static int32 LuaFrameSetPlaceholderTextColor(lua_State* L);
	static int32 LuaFrameSetPlaceholderJustifyH(lua_State* L);
	static int32 LuaFrameSetTextInsets(lua_State* L);
	static int32 LuaFrameSetTextColor(lua_State* L);
	static int32 LuaFrameSetFont(lua_State* L);
	static int32 LuaFrameSetFontObject(lua_State* L);
	static int32 LuaFrameSetShadowColor(lua_State* L);
	static int32 LuaFrameSetShadowOffset(lua_State* L);
	static int32 LuaFrameSetJustifyH(lua_State* L);
	static int32 LuaFrameSetJustifyV(lua_State* L);
	static int32 LuaFrameSetTextLayout(lua_State* L);
	static int32 LuaFrameSetTextFirstLineIndent(lua_State* L);
	static int32 LuaFrameSetTextFirstLineIndentChars(lua_State* L);
	static int32 LuaFrameSetTextParagraphSpacing(lua_State* L);
	static int32 LuaFrameSetTextLineHeight(lua_State* L);
	static int32 LuaFrameSetTextJustifyLastLine(lua_State* L);
	static int32 LuaFrameSetScrollChild(lua_State* L);
	static int32 LuaFrameSetVerticalScroll(lua_State* L);
	static int32 LuaFrameGetVerticalScroll(lua_State* L);
	static int32 LuaFrameGetVerticalScrollRange(lua_State* L);
	static int32 LuaFrameSetScrollBarStyle(lua_State* L);
	static int32 LuaFrameSetTextGradient(lua_State* L);
	static int32 LuaFrameSetTexture(lua_State* L);
	static int32 LuaFrameSetTexCoord(lua_State* L);
	static int32 LuaFrameSetBackdrop(lua_State* L);
	static int32 LuaFrameSetNormalTexture(lua_State* L);
	static int32 LuaFrameSetPushedTexture(lua_State* L);
	static int32 LuaFrameSetHighlightTexture(lua_State* L);
	static int32 LuaFrameSetDisabledTexture(lua_State* L);
	static int32 LuaFrameSetScript(lua_State* L);
	static int32 LuaFrameRegisterEvent(lua_State* L);
	static int32 LuaFrameUnregisterEvent(lua_State* L);
	static int32 LuaFrameRaise(lua_State* L);
	static int32 LuaFrameClick(lua_State* L);
	static int32 LuaFrameSetFocus(lua_State* L);
	static int32 LuaFrameSetKeepKeyboardFocus(lua_State* L);
	static int32 LuaFrameClearAllPoints(lua_State* L);
	static int32 LuaFrameSetMaxLetters(lua_State* L);
	static int32 LuaFrameSetMaxBytes(lua_State* L);
	static int32 LuaFrameShow(lua_State* L);
	static int32 LuaFrameHide(lua_State* L);
	static int32 LuaFrameIsShown(lua_State* L);
	static int32 LuaFrameEnable(lua_State* L);
	static int32 LuaFrameDisable(lua_State* L);
	static int32 LuaFrameSetEnabled(lua_State* L);
	static int32 LuaFrameIsEnabled(lua_State* L);
	static int32 LuaFrameSetDrawLayer(lua_State* L);

	void RegisterGlobals();
	void RegisterFrameMetatable();
	void LoadGlueDbcStores();
	void RegisterFrameGlobal(TSharedRef<FWoWGlueFrame> Frame);
	bool LoadXmlScriptFile(const FString& XmlPath, const FString& ScriptFile, FString* OutError);
	bool BuildXmlObject(const FWoWGlueXmlObjectNode& Node, TSharedPtr<FWoWGlueFrame> Parent, TSharedPtr<FWoWGlueFrame>& OutFrame, FString* OutError);
	bool ApplyXmlObjectBody(const FWoWGlueXmlObjectNode& Node, TSharedRef<FWoWGlueFrame> Frame, FString* OutError);
	bool ApplyXmlTemplate(const FString& TemplateName, TSharedRef<FWoWGlueFrame> Frame, FString* OutError);
	void RegisterXmlTemplate(const FString& TemplateName, const FWoWGlueXmlObjectNode& Node);
	FString ResolveXmlInheritedTexturePath(const FString& TemplateName) const;
	bool BindXmlFrameScript(TSharedRef<FWoWGlueFrame> Frame, const FString& ScriptName, const FString& Body);
	void InvokeFrameOnLoad(TSharedRef<FWoWGlueFrame> Frame);
	TSharedPtr<FWoWGlueFrame> ResolveFrameFromLua(lua_State* L, int32 StackIndex) const;
	void InvokeFrameScript(TSharedRef<FWoWGlueFrame> Frame, int32 FunctionRef);
	void InvokeFrameScriptWithElapsed(TSharedRef<FWoWGlueFrame> Frame, int32 FunctionRef, float ElapsedSeconds);
	void InvokeFrameScriptWithString(TSharedRef<FWoWGlueFrame> Frame, int32 FunctionRef, const FString& Value);
	void InvokeFrameScriptWithEvent(TSharedRef<FWoWGlueFrame> Frame, int32 FunctionRef, const FString& EventName);
	void InvokeFrameScriptWithHyperlink(TSharedRef<FWoWGlueFrame> Frame, int32 FunctionRef, const FString& Link, const FString& Text);
	int32 StoreLuaFunctionRef(lua_State* L, int32 StackIndex);
	void ReleaseLuaFunctionRef(int32& FunctionRef);
	FFrameScriptRef& GetFrameScriptRef(FWoWGlueFrame& Frame);

	lua_State* LuaState = nullptr;
	TSharedPtr<FWoWGlueUIManager> GlueManager;
	TMap<FString, TSharedPtr<FWoWGlueXmlTemplateInfo>> XmlTemplates;
	TMap<FWoWGlueFrame*, FFrameScriptRef> FrameScriptRefs;
	FGlueLoginHandler GlueLoginHandler;
	FGlueScreenHandler GlueScreenHandler;
	FGlueCharacterSelectHandler CharacterSelectHandler;
	FGlueEnterWorldHandler EnterWorldHandler;
	FGlueCharacterCreateHandler CharacterCreateHandler;
	FGlueRealmSelectHandler RealmSelectHandler;
	FGlueRealmCancelHandler RealmCancelHandler;
	FGlueLuaErrorHandler ErrorHandler;
	FGlueBackgroundModelHandler BackgroundModelHandler;
	FGlueDebugLogHandler DebugLogHandler;
	FString CurrentGlueScreen = TEXT("login");
	FString AccountErrorText;
	TArray<FString> CharacterListRowsTsv;
	TArray<FString> RealmListRowsTsv;
	TMap<int32, FString> DbcRaceNames;
	TMap<int32, FString> DbcClassNames;
	TMap<int32, FString> DbcAreaNames;
	int32 DefaultCharacterCreateRace = 1;
	int32 DefaultCharacterCreateClass = 1;
	int32 DefaultCharacterCreateGender = 0;
};
