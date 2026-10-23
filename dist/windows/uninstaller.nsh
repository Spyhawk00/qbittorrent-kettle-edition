Section "un.$(remove_files)" ;"un.Remove files"
  SectionIn RO

; Remove files and uninstaller
  Delete "$INSTDIR\qbittorrent-ke.exe"
  Delete "$INSTDIR\qbittorrent.exe"
  Delete "$INSTDIR\qbittorrent.pdb"
  Delete "$INSTDIR\qt.conf"
  Delete "$INSTDIR\uninst.exe"

  ; Remove directories used
  RMDir /r "$INSTDIR\translations"

  ; The files of this version (list made by make-installer.ps1), for a folder that is shared with other programs
  !include "${KETTLE_UNINSTALL_LIST}"

  ; Updates add and replace files, so the list can be out of date. Everything in a folder of its own goes.
  ${un.GetFileName} "$INSTDIR" $0
  StrCmp $0 "qBittorrent-ke" +2 0
  StrCmp $0 "qBittorrent" 0 +2
    RMDir /r "$INSTDIR"
  RMDir "$INSTDIR"
SectionEnd

Section "un.$(remove_shortcuts)" ;"un.Remove shortcuts"
  SectionIn RO
; Remove shortcuts, if any (the ones of the official installer too)
  RMDir /r "$SMPROGRAMS\qBittorrent Kettle Edition"
  RMDir /r "$SMPROGRAMS\qBittorrent"
  Delete "$DESKTOP\qBittorrent Kettle Edition.lnk"
  Delete "$DESKTOP\qBittorrent.lnk"
SectionEnd

Section "un.$(remove_registry)" ;"un.Remove registry keys"
  SectionIn RO
  ; Remove registry keys
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\qBittorrent"
  DeleteRegKey HKLM "Software\qBittorrent"
  DeleteRegKey HKLM "Software\qBittorrent-ke"
  ; Remove ProgIDs
  DeleteRegKey HKLM "Software\Classes\qBittorrent.File.Torrent"
  DeleteRegKey HKLM "Software\Classes\qBittorrent.Url.Magnet"
  System::Call 'shell32::SHChangeNotify(i ${SHCNE_ASSOCCHANGED}, i ${SHCNF_IDLIST}, p 0, p 0)'
SectionEnd

Section "un.$(remove_firewall)" ;

  DetailPrint $(remove_firewallinfo)
  nsisFirewallW::RemoveAuthorizedApplication "$INSTDIR\qbittorrent-ke.exe"

SectionEnd

Section /o "un.$(remove_conf)" ;"un.Remove configuration files"

  !insertmacro UAC_AsUser_Call Function un.remove_conf_user ${UAC_SYNCREGISTERS}|${UAC_SYNCOUTDIR}|${UAC_SYNCINSTDIR}

SectionEnd

Function un.remove_conf_user

  System::Call 'shell32::SHGetKnownFolderPath(g "${FOLDERID_RoamingAppData}", i 0, p 0, *w . r1) i . r0'
  RMDir /r "$1\qBittorrent"
  System::Call 'ole32::CoTaskMemFree(p r1)'

FunctionEnd

Section /o "un.$(remove_cache)"

  !insertmacro UAC_AsUser_Call Function un.remove_cache_user ${UAC_SYNCREGISTERS}|${UAC_SYNCOUTDIR}|${UAC_SYNCINSTDIR}

SectionEnd

Function un.remove_cache_user

  System::Call 'shell32::SHGetKnownFolderPath(g "${FOLDERID_LocalAppData}", i 0, p 0, *w . r1) i . r0'
  RMDir /r "$1\qBittorrent"
  System::Call 'ole32::CoTaskMemFree(p r1)'

FunctionEnd

;--------------------------------
;Uninstaller Functions

Function un.onInit

  !insertmacro Init "uninstaller"
  !insertmacro MUI_UNGETLANGUAGE

FunctionEnd

Function un.check_instance

  check:
  FindProcDLL::FindProc "qbittorrent-ke.exe"
  StrCmp $R0 "1" 0 notfound
  MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION $(uninst_warning) /SD IDCANCEL IDRETRY check IDCANCEL canceled

  canceled:
  SetErrorLevel 15618 # WinError.h: `ERROR_PACKAGES_IN_USE`
  Abort

  notfound:

FunctionEnd

Function un.onUninstSuccess
  SetErrorLevel 0
FunctionEnd
