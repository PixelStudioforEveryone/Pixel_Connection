Unicode True
!include "MUI2.nsh"
!include "x64.nsh"
!include "WinVer.nsh"
!include "${REMOVAL_MANIFEST}"

Name "PixelConnection"
OutFile "${OUTPUT_FILE}"
InstallDir "$LOCALAPPDATA\Programs\PixelConnection"
InstallDirRegKey HKCU "Software\PixelConnection\Installer" "InstallLocation"
RequestExecutionLevel user
SetCompressor /SOLID lzma
Icon "${APP_ICON}"
UninstallIcon "${APP_ICON}"
VIProductVersion "${VERSION}.0"
VIAddVersionKey /LANG=1033 "ProductName" "PixelConnection"
VIAddVersionKey /LANG=1033 "FileVersion" "${VERSION}"
VIAddVersionKey /LANG=1033 "FileDescription" "PixelConnection Windows x64 installer"
VIAddVersionKey /LANG=1033 "LegalCopyright" "PixelConnection contributors, GPL-3.0-only"

!insertmacro MUI_PAGE_LICENSE "${PAYLOAD_DIR}\LICENSE"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"
!insertmacro MUI_LANGUAGE "SimpChinese"

Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "PixelConnection requires 64-bit Windows."
    Abort
  ${EndIf}
  ${IfNot} ${AtLeastWin10}
    MessageBox MB_ICONSTOP "PixelConnection requires Windows 10 or later."
    Abort
  ${EndIf}
  SetRegView 64
FunctionEnd

Section "PixelConnection"
  SetOutPath "$INSTDIR"
  File /r "${PAYLOAD_DIR}\*"
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  CreateDirectory "$SMPROGRAMS\PixelConnection"
  CreateShortcut "$SMPROGRAMS\PixelConnection\PixelConnection.lnk" "$INSTDIR\pxc-client.exe"
  CreateShortcut "$SMPROGRAMS\PixelConnection\Uninstall.lnk" "$INSTDIR\Uninstall.exe"
  CreateShortcut "$DESKTOP\PixelConnection.lnk" "$INSTDIR\pxc-client.exe"
  WriteRegStr HKCU "Software\PixelConnection\Installer" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\PixelConnection" "DisplayName" "PixelConnection"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\PixelConnection" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\PixelConnection" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\PixelConnection" "DisplayIcon" "$INSTDIR\pxc-client.exe"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\PixelConnection" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\PixelConnection" "NoModify" 1
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\PixelConnection" "NoRepair" 1
SectionEnd

Section "Uninstall"
  SetRegView 64
  !insertmacro RemovePayload
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"
  Delete "$SMPROGRAMS\PixelConnection\PixelConnection.lnk"
  Delete "$SMPROGRAMS\PixelConnection\Uninstall.lnk"
  RMDir "$SMPROGRAMS\PixelConnection"
  Delete "$DESKTOP\PixelConnection.lnk"
  DeleteRegKey HKCU "Software\PixelConnection\Installer"
  DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\PixelConnection"
SectionEnd
