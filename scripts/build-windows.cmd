@echo off
setlocal
set "repo_dir=%~dp0.."
set "vs_devcmd=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat"
if not exist "%vs_devcmd%" (
    echo Visual Studio 2022 Build Tools developer environment not found. >&2
    exit /b 1
)
call "%vs_devcmd%" -arch=x64 -host_arch=x64
if errorlevel 1 exit /b %errorlevel%
cmake -S "%repo_dir%" -B "%repo_dir%\build_clang" -G Ninja "-DCMAKE_C_COMPILER=%ProgramFiles%\LLVM\bin\clang-cl.exe" "-DCMAKE_CXX_COMPILER=%ProgramFiles%\LLVM\bin\clang-cl.exe" -DCMAKE_BUILD_TYPE=RelWithDebInfo
if errorlevel 1 exit /b %errorlevel%
cmake --build "%repo_dir%\build_clang" --parallel 8
exit /b %errorlevel%
