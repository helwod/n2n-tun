@echo off
setlocal

REM === n2n Windows Build ===
set PATH=C:\msys64\ucrt64\bin;%PATH%
set CC=gcc.exe
set SRC=C:\Users\Administrator\CodeBuddy\n2n
set BUILD=%SRC%\build

echo =============================================
echo   n2n Windows Build (MinGW)
echo =============================================
echo.

REM Clean
del /q "%BUILD%\*.o" 2>nul
del /q "%BUILD%\*.a" 2>nul
del /q "%BUILD%\*.exe" 2>nul
del /q "%BUILD%\*.err" 2>nul

set CFLAGS=-DGNUC=1 -DN2N_HAVE_OPENSSL=1 -DN2N_HAVE_WINTUN=1 -DN2N_HAVE_ZSTD=1 -DWINVER=0x0600 -D_CONSOLE -D_CRT_SECURE_NO_WARNINGS -D_WIN32_WINNT=0x0600
set INCLUDES=-I"%SRC%\include" -I"%SRC%\src" -I"%SRC%\thirdparty" -I"%SRC%\build"
set FLAGS=-Wall -O3 -std=gnu99

set OK=0
set FAIL=0

echo === Compiling core sources ===
echo.

for %%F in (
    aes.c auth.c cc20.c curve25519.c
    edge_management.c edge_utils.c header_encryption.c
    hexdump.c json.c management.c minilzo.c
    n2n.c n2n_port_mapping.c n2n_regex.c
    network_traffic_filter.c pearson.c random_numbers.c
    sn_management.c sn_selection.c sn_utils.c speck.c
    tf.c transform_aes.c transform_cc20.c transform_lzo.c
    transform_null.c transform_speck.c transform_tf.c transform_zstd.c
    wire.c
) do (
    <nul set /p="  %%F ... "
    "%CC%" %CFLAGS% %INCLUDES% %FLAGS% -c "%SRC%\src\%%F" -o "%BUILD%\%%~nF.o" 2>"%BUILD%\%%~nF.err"
    if errorlevel 1 (
        echo FAILED
        set /a FAIL+=1
    ) else (
        echo OK
        set /a OK+=1
        del /q "%BUILD%\%%~nF.err" 2>nul
    )
)

echo.
echo === Compiling Windows sources ===
echo.

for %%F in (
    wintap.c edge_utils_win32.c getopt.c getopt1.c wintun_device.c
) do (
    <nul set /p="  win32\%%F ... "
    "%CC%" %CFLAGS% %INCLUDES% %FLAGS% -c "%SRC%\src\win32\%%F" -o "%BUILD%\%%~nF.o" 2>"%BUILD%\%%~nF.err"
    if errorlevel 1 (
        echo FAILED
        set /a FAIL+=1
    ) else (
        echo OK
        set /a OK+=1
        del /q "%BUILD%\%%~nF.err" 2>nul
    )
)

echo.
echo === Compiling edge.c ===
echo.
<nul set /p="  edge.c ... "
"%CC%" %CFLAGS% %INCLUDES% %FLAGS% -c "%SRC%\src\edge.c" -o "%BUILD%\edge.o" 2>"%BUILD%\edge.err"
if errorlevel 1 (
    echo FAILED
    set /a FAIL+=1
) else (
    echo OK
    set /a OK+=1
    del /q "%BUILD%\edge.err" 2>nul
)

echo.
echo Result: %OK% OK, %FAIL% FAILED
echo.

if %FAIL% gtr 0 (
    echo === Error details ===
    for %%F in ("%BUILD%\*.err") do (
        if %%~zF gtr 0 (
            echo --- %%~nF ---
            type "%%F"
            echo.
        )
    )
    exit /b 1
)

echo === Linking edge.exe ===
echo.

"%CC%" -o "%BUILD%\edge.exe" "%BUILD%\*.o" -lws2_32 -lssl -lcrypto -lzstd -liphlpapi -lsetupapi -lnewdev -lcfgmgr32 -lole32 -lm -lpthread 2>"%BUILD%\link.err"
if errorlevel 1 (
    echo Linking edge FAILED!
    type "%BUILD%\link.err"
    exit /b 1
)

echo   edge.exe linked OK

echo.
echo === Compiling supernode.c ===
echo.
<nul set /p="  supernode.c ... "
"%CC%" %CFLAGS% %INCLUDES% %FLAGS% -c "%SRC%\src\supernode.c" -o "%BUILD%\supernode_main.o" 2>"%BUILD%\supernode_main.err"
if errorlevel 1 (
    echo FAILED
    type "%BUILD%\supernode_main.err"
    exit /b 1
)
echo OK

echo.
echo === Linking supernode.exe ===
echo.
REM Exclude edge.o and supernode_main.o conflict - use all library .o except edge.o
"%CC%" -o "%BUILD%\supernode.exe" "%BUILD%\supernode_main.o" "%BUILD%\aes.o" "%BUILD%\auth.o" "%BUILD%\cc20.o" "%BUILD%\curve25519.o" "%BUILD%\edge_management.o" "%BUILD%\edge_utils.o" "%BUILD%\header_encryption.o" "%BUILD%\hexdump.o" "%BUILD%\json.o" "%BUILD%\management.o" "%BUILD%\minilzo.o" "%BUILD%\n2n.o" "%BUILD%\n2n_port_mapping.o" "%BUILD%\n2n_regex.o" "%BUILD%\network_traffic_filter.o" "%BUILD%\pearson.o" "%BUILD%\random_numbers.o" "%BUILD%\sn_management.o" "%BUILD%\sn_selection.o" "%BUILD%\sn_utils.o" "%BUILD%\speck.o" "%BUILD%\tf.o" "%BUILD%\transform_aes.o" "%BUILD%\transform_cc20.o" "%BUILD%\transform_lzo.o" "%BUILD%\transform_null.o" "%BUILD%\transform_speck.o" "%BUILD%\transform_tf.o" "%BUILD%\transform_zstd.o" "%BUILD%\wire.o" "%BUILD%\wintap.o" "%BUILD%\edge_utils_win32.o" "%BUILD%\getopt.o" "%BUILD%\getopt1.o" "%BUILD%\wintun_device.o" -lws2_32 -lssl -lcrypto -lzstd -liphlpapi -lsetupapi -lnewdev -lcfgmgr32 -lole32 -lm -lpthread 2>"%BUILD%\link_sn.err"
if errorlevel 1 (
    echo Linking supernode FAILED!
    type "%BUILD%\link_sn.err"
    exit /b 1
)

echo   supernode.exe linked OK

echo.
echo =============================================
echo   BUILD SUCCESS!
echo =============================================
echo.
dir "%BUILD%\edge.exe"
