@echo off
setlocal

set CSC=%WINDIR%\Microsoft.NET\Framework\v4.0.30319\csc.exe
set GAC=%WINDIR%\Microsoft.NET\assembly\GAC_32
set XNA=%GAC%\Microsoft.Xna.Framework\v4.0_4.0.0.0__842cf8be1de50553\Microsoft.Xna.Framework.dll
set XNAGAME=%GAC%\Microsoft.Xna.Framework.Game\v4.0_4.0.0.0__842cf8be1de50553\Microsoft.Xna.Framework.Game.dll
set XNAGFX=%GAC%\Microsoft.Xna.Framework.Graphics\v4.0_4.0.0.0__842cf8be1de50553\Microsoft.Xna.Framework.Graphics.dll

if not exist "%CSC%" (
  echo ERROR: no csc at %CSC%
  exit /b 1
)
if not exist "%XNA%" (
  echo ERROR: no XNA framework at %XNA%
  exit /b 1
)

if not exist "%~dp0bin" mkdir "%~dp0bin"

"%CSC%" /nologo /target:exe /platform:x86 /langversion:5 /debug:pdbonly ^
  /out:"%~dp0bin\XnaScene.exe" ^
  /reference:"%XNA%" /reference:"%XNAGAME%" /reference:"%XNAGFX%" ^
  "%~dp0Program.cs" "%~dp0SceneGame.cs" "%~dp0..\probe\Report.cs"

if errorlevel 1 (
  echo BUILD FAILED
  exit /b 1
)
echo Built %~dp0bin\XnaScene.exe
endlocal
