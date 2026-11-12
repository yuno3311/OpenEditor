@echo off
rem Run build.sh from PowerShell or cmd:  build [options] [target]   (build --help for the list)
rem Uses Git Bash or MSYS2. Plain "bash" in PowerShell is often WSL's, which also works but is slower.
setlocal
set "SH="
for %%G in (git.exe) do if not "%%~$PATH:G"=="" (
    if exist "%%~dp$PATH:G..\bin\bash.exe" set "SH=%%~dp$PATH:G..\bin\bash.exe"
)
if not defined SH if exist "%ProgramFiles%\Git\bin\bash.exe" set "SH=%ProgramFiles%\Git\bin\bash.exe"
if not defined SH if exist "%LocalAppData%\Programs\Git\bin\bash.exe" set "SH=%LocalAppData%\Programs\Git\bin\bash.exe"
if not defined SH if exist "C:\msys64\usr\bin\bash.exe" set "SH=C:\msys64\usr\bin\bash.exe"
if not defined SH (
    where wsl.exe >nul 2>nul && (
        wsl.exe sh "$(wslpath '%~dp0build.sh')" %*
        exit /b %ERRORLEVEL%
    )
    echo build: error: no Git Bash, MSYS2 or WSL found. Install Git for Windows from https://git-scm.com 1>&2
    exit /b 1
)
"%SH%" "%~dp0build.sh" %*
exit /b %ERRORLEVEL%
