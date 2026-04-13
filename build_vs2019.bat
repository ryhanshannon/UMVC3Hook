@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d "C:\Users\rs\Documents\GitHub\umvc3-research\external\umvc3-specific\UMVC3Hook"
msbuild UMVC3Hook.sln /p:Configuration=Release /p:Platform=x64 /v:minimal 2>&1
echo BUILD_EXIT_CODE=%ERRORLEVEL%
