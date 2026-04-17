@echo off
set PATH=C:\msys64\ucrt64\bin;%PATH%
echo === Running edge.exe with debug ===
echo.
echo Usage: edge -c community -l supernode:port -a ip/mask [-d device] [-w] [-vv]
echo.
echo Example: %~nx0 -c test -l 127.0.0.1:1234 -a 10.0.0.1/24 -d tunedge -w -vv
echo.
C:\Users\Administrator\CodeBuddy\n2n\build\edge.exe %*
