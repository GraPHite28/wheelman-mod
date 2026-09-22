@echo off
rem Builds MapDataBuilder.exe with the C# compiler that ships with Windows (.NET Framework 4, no SDK needed).
setlocal
set CSC=%WINDIR%\Microsoft.NET\Framework\v4.0.30319\csc.exe
if not exist "%CSC%" ( echo csc.exe not found & exit /b 1 )
cd /d "%~dp0"
if not exist bin mkdir bin
copy /y lib\*.dll bin\ >nul
"%CSC%" /nologo /target:exe /main:MapDataBuilder /r:System.Drawing.dll /r:lib\Eliot.UELib.dll /out:bin\MapDataBuilder.exe MapDataBuilder.cs Decomp.cs TexDump.cs SwfIcons.cs RoadMask.cs
if errorlevel 1 exit /b 1
copy /y lib\UELib-LICENSE.txt bin\ >nul
echo built %~dp0bin\MapDataBuilder.exe
