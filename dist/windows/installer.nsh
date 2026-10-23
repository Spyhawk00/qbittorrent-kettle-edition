Var uninstallerPath

Section "-hidden"

    ;Search if qBittorrent is already installed.
    FindFirst $0 $1 "$uninstallerPath\uninst.exe"
    FindClose $0
    StrCmp $1 "" done

    ;Run the uninstaller of the previous install.
    DetailPrint $(inst_unist)
    ExecWait '"$uninstallerPath\uninst.exe" /S _?=$uninstallerPath'
    Delete "$uninstallerPath\uninst.exe"
    RMDir "$uninstallerPath"

    done:

SectionEnd


Section $(inst_qbt_req) ;"qBittorrent (required)"

  SectionIn RO

  ; Set output path to the installation directory.
  SetOutPath $INSTDIR
  ; Put files there
  File "${QBT_DIST_DIR}\qbittorrent-ke.exe"
  File "qt.conf"
  File /nonfatal /r /x "${QBT_DIST_DIR}\qbittorrent-ke.exe" "${QBT_DIST_DIR}\*.*"

  ; Write the installation path into the registry
  WriteRegStr HKLM "Software\qBittorrent" "InstallLocation" "$INSTDIR"
  ; Kettle Edition's own key, so a reinstall never lands in the folder of an official install
  WriteRegStr HKLM "Software\qBittorrent-ke" "InstallLocation" "$INSTDIR"

  ; Register qBittorrent as possible default program for .torrent files and magnet links
  WriteRegStr HKLM "Software\qBittorrent\Capabilities" "ApplicationDescription" "A BitTorrent client in Qt"
  WriteRegStr HKLM "Software\qBittorrent\Capabilities" "ApplicationName" "qBittorrent Kettle Edition"
  WriteRegStr HKLM "Software\qBittorrent\Capabilities\FileAssociations" ".torrent" "qBittorrent.File.Torrent"
  WriteRegStr HKLM "Software\qBittorrent\Capabilities\UrlAssociations" "magnet" "qBittorrent.Url.Magnet"
  WriteRegStr HKLM "Software\RegisteredApplications" "qBittorrent" "Software\qBittorrent\Capabilities"
  ; Register qBittorrent ProgIDs
  WriteRegStr HKLM "Software\Classes\qBittorrent.File.Torrent" "" "Torrent File"
  WriteRegStr HKLM "Software\Classes\qBittorrent.File.Torrent\DefaultIcon" "" '"$INSTDIR\qbittorrent-ke.exe",1'
  WriteRegStr HKLM "Software\Classes\qBittorrent.File.Torrent\shell\open\command" "" '"$INSTDIR\qbittorrent-ke.exe" "%1"'
  WriteRegStr HKLM "Software\Classes\qBittorrent.Url.Magnet" "" "Magnet URI"
  WriteRegStr HKLM "Software\Classes\qBittorrent.Url.Magnet\DefaultIcon" "" '"$INSTDIR\qbittorrent-ke.exe",1'
  WriteRegStr HKLM "Software\Classes\qBittorrent.Url.Magnet\shell\open\command" "" '"$INSTDIR\qbittorrent-ke.exe" "%1"'

  WriteRegStr HKLM "Software\Classes\.torrent" "Content Type" "application/x-bittorrent"
  WriteRegStr HKLM "Software\Classes\magnet" "" "URL:Magnet URI"
  WriteRegStr HKLM "Software\Classes\magnet" "Content Type" "application/x-magnet"
  WriteRegStr HKLM "Software\Classes\magnet" "URL Protocol" ""

  System::Call 'shell32::SHChangeNotify(i ${SHCNE_ASSOCCHANGED}, i ${SHCNF_IDLIST}, p 0, p 0)'

  ; Write the uninstall keys for Windows
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\qBittorrent" "DisplayName" "qBittorrent Kettle Edition"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\qBittorrent" "UninstallString" '"$INSTDIR\uninst.exe"'
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\qBittorrent" "DisplayIcon" '"$INSTDIR\qbittorrent-ke.exe",0'
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\qBittorrent" "Publisher" "Spyhawk"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\qBittorrent" "URLInfoAbout" "https://github.com/Spyhawk00/qbittorrent-kettle-edition"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\qBittorrent" "DisplayVersion" "${KETTLE_EDITION}"
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\qBittorrent" "NoModify" 1
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\qBittorrent" "NoRepair" 1
  WriteUninstaller "uninst.exe"
  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  IntFmt $0 "0x%08X" $0
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\qBittorrent" "EstimatedSize" "$0"

SectionEnd

; Lets the Windows user who started this installer change the program folder, so that Kettle Edition can update itself
; without an administrator prompt. The installer itself runs as an administrator (possibly a different account than the
; user, when a standard user typed an administrator's password), so the user is looked up in the user's own session.
; Shown on the components page, ticked by default, so nobody gets the folder permission without seeing it.
Section "Allow updates without admin prompts" kettle_update_permission

  !insertmacro UAC_AsUser_Call Function kettle_get_user ${UAC_SYNCREGISTERS}
  ; $0 = user name, $1 = domain (for a local account: the computer name)
  nsExec::ExecToStack 'icacls "$INSTDIR" /grant "$1\$0:(OI)(CI)M"'
  Pop $2
  Pop $3
  ${If} $2 != 0
    DetailPrint "Could not give $1\$0 access to $INSTDIR (icacls: $2): $3"
    MessageBox MB_OK|MB_ICONEXCLAMATION "qBittorrent Kettle Edition was installed, but automatic updates will ask for administrator rights because the program folder could not be opened for your account." /SD IDOK
  ${EndIf}

SectionEnd

Function kettle_get_user

  System::Call 'advapi32::GetUserNameW(w .r0, *i ${NSIS_MAX_STRLEN})'
  ReadEnvStr $1 USERDOMAIN

FunctionEnd

; Optional section (can be disabled by the user)
Section /o $(inst_desktop) ;"Create Desktop Shortcut"

  CreateShortCut "$DESKTOP\qBittorrent Kettle Edition.lnk" "$INSTDIR\qbittorrent-ke.exe"

SectionEnd

Section $(inst_startmenu) ;"Create Start Menu Shortcut"

  CreateDirectory "$SMPROGRAMS\qBittorrent Kettle Edition"
  CreateShortCut "$SMPROGRAMS\qBittorrent Kettle Edition\qBittorrent Kettle Edition.lnk" "$INSTDIR\qbittorrent-ke.exe"
  CreateShortCut "$SMPROGRAMS\qBittorrent Kettle Edition\$(inst_uninstall_link_description).lnk" "$INSTDIR\uninst.exe"

SectionEnd

Section /o $(inst_startup) ;"Start qBittorrent on Windows start up"

  !insertmacro UAC_AsUser_Call Function inst_startup_user ${UAC_SYNCREGISTERS}|${UAC_SYNCOUTDIR}|${UAC_SYNCINSTDIR}

SectionEnd

Function inst_startup_user

  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "qBittorrent" "$INSTDIR\qbittorrent-ke.exe"

FunctionEnd

Section $(inst_firewall)

  DetailPrint $(inst_firewallinfo)
  nsisFirewallW::AddAuthorizedApplication "$INSTDIR\qbittorrent-ke.exe" "qBittorrent"

SectionEnd

Section $(inst_pathlimit) ;"Disable Windows path length limit (260 character MAX_PATH limitation, requires Windows 10 1607 or later)"

  WriteRegDWORD HKLM "SYSTEM\CurrentControlSet\Control\FileSystem" "LongPathsEnabled" 1

SectionEnd

;--------------------------------

Function .onInit

  !insertmacro Init "installer"
  !insertmacro MUI_LANGDLL_DISPLAY

  ${IfNot} ${AtLeastWaaS} 1809 ; Windows 10 (1809) / Windows Server 2019. Min supported version by Qt6
    MessageBox MB_OK|MB_ICONEXCLAMATION $(inst_requires_win10) /SD IDOK
    SetErrorLevel 1654 # WinError.h: `ERROR_INSTALL_REJECTED`
    Abort
  ${EndIf}

  ${IfNot} ${RunningX64}
    MessageBox MB_OK|MB_ICONEXCLAMATION $(inst_requires_64bit) /SD IDOK
    SetErrorLevel 1654 # WinError.h: `ERROR_INSTALL_REJECTED`
    Abort
  ${EndIf}

  ; check installer and current system architecture
  ${If} "${QBT_CPU_ARCH}" == "arm64"
  ${AndIf} ${IsNativeAMD64}  ; installing arm64 binary on x64
    MessageBox MB_OK|MB_ICONEXCLAMATION $(inst_arch_mismatch_arm64_on_x64) /SD IDOK
    SetErrorLevel 1654 # WinError.h: `ERROR_INSTALL_REJECTED`
    Abort
  ${EndIf}

  ;Search if qBittorrent is already installed.
  FindFirst $0 $1 "$INSTDIR\uninst.exe"
  FindClose $0
  StrCmp $1 "" done

  ;Copy old value to var so we can call the correct uninstaller
  StrCpy $uninstallerPath $INSTDIR

  ;Inform the user
  MessageBox MB_OKCANCEL|MB_ICONINFORMATION $(inst_uninstall_question) /SD IDOK IDOK done
  Quit

  done:

FunctionEnd

Function check_instance

  check:
  FindProcDLL::FindProc "qbittorrent-ke.exe"
  StrCmp $R0 "1" running
  ; the official program file, which this installer replaces
  FindProcDLL::FindProc "qbittorrent.exe"
  StrCmp $R0 "1" running notfound

  running:
  MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION $(inst_warning) /SD IDCANCEL IDRETRY check IDCANCEL canceled

  canceled:
  SetErrorLevel 15618 # WinError.h: `ERROR_PACKAGES_IN_USE`
  Abort

  notfound:

FunctionEnd

Function PageFinishRun

  !insertmacro UAC_AsUser_ExecShell "" "$INSTDIR\qbittorrent-ke.exe" "" "" ""

FunctionEnd

Function .onInstSuccess
  SetErrorLevel 0
FunctionEnd
