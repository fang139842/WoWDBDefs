# Lua 5.1.5

This directory embeds Lua 5.1.5 for the WoW-style GlueXML runtime.

The Win64 static library was built from `lua-5.1.5/src` with MSVC, excluding
`lua.c`, `luac.c`, and `print.c`.

Example rebuild command from the project root:

```powershell
$LuaSrc = Resolve-Path "Source\ThirdParty\Lua\lua-5.1.5\src"
$BuildDir = "Source\ThirdParty\Lua\Build\Win64"
$LibDir = "Source\ThirdParty\Lua\Lib\Win64"
New-Item -ItemType Directory -Force -Path $BuildDir, $LibDir | Out-Null
$Sources = Get-ChildItem $LuaSrc -Filter *.c | Where-Object { $_.Name -notin @('lua.c','luac.c','print.c') } | ForEach-Object { $_.FullName }
Push-Location $BuildDir
Remove-Item -Force *.obj,*.lib -ErrorAction SilentlyContinue
cl /nologo /c /O2 /MD /DLUA_USE_WINDOWS /D_CRT_SECURE_NO_WARNINGS /D_CRT_NONSTDC_NO_DEPRECATE /I "$LuaSrc" $Sources
lib /nologo /OUT:"..\..\Lib\Win64\lua51.lib" *.obj
Pop-Location
```
