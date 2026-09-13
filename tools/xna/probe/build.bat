@echo off
setlocal
rem Builds the probe with csc directly. XNA Game Studio is NOT installed here -
rem only the redistributable runtime, which is in the 32-bit GAC - so there are
rem no MSBuild targets and no content pipeline to lean on. Nothing in the probe
rem needs content: BasicEffect ships in the framework and its one texture is
rem built in code.
rem
rem i386, managed only, and referencing the same assembly identity the console
rem assemblies carry, so the one binary runs on the PC runtime and under Nexia.

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

rem /target:exe, not winexe: the report goes to stdout and a windowed subsystem
rem binary has nowhere to put it.
"%CSC%" /nologo /target:exe /platform:x86 /langversion:5 /debug:pdbonly ^
  /out:"%~dp0bin\XnaProbe.exe" ^
  /reference:"%XNA%" /reference:"%XNAGAME%" /reference:"%XNAGFX%" ^
  /resource:"%~dp0sky.bgra",sky.bgra ^
  "%~dp0Program.cs" "%~dp0Report.cs" "%~dp0ProbeGame.cs"

if errorlevel 1 (
  echo BUILD FAILED
  exit /b 1
)
echo Built %~dp0bin\XnaProbe.exe
endlocal
