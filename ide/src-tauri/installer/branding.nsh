; Keep Tauri's maintained install/upgrade/uninstall logic. Customize its UI,
; and check for the Microsoft Visual C++ runtime before anything is installed.
SetFont "Segoe UI" 9
!define MUI_HEADERIMAGE_RIGHT
!define MUI_BGCOLOR "FFFFFF"
!define MUI_TEXTCOLOR "172633"
!define MUI_INSTALLCOLORS "172633 FFFFFF"
!define MUI_INSTFILESPAGE_PROGRESSBAR "colored"
!define MUI_WELCOMEPAGE_TITLE "$(ArnWelcomeTitle)"
!define MUI_WELCOMEPAGE_TEXT "$(ArnWelcomeText)"
!define MUI_WELCOMEPAGE_TITLE_3LINES
!define MUI_FINISHPAGE_TITLE "$(ArnFinishTitle)"
!define MUI_FINISHPAGE_TEXT "$(ArnFinishText)"
!define MUI_FINISHPAGE_TITLE_3LINES
!define MUI_FINISHPAGE_RUN_NOTCHECKED
!define MUI_FINISHPAGE_SHOWREADME_NOTCHECKED

LangString ArnWelcomeTitle 1033 "A home for your next idea."
LangString ArnWelcomeText 1033 "Your editor, AI assistant and project in one workspace.$\r$\n$\r$\nARN will be installed for your Windows account. Your projects stay where they are.$\r$\n$\r$\nConnect your AI provider after setup. File changes always need your approval.$\r$\n$\r$\nLet's get your workspace ready."
LangString ArnFinishTitle 1033 "Your workspace is ready."
LangString ArnFinishText 1033 "ARN IDE is installed.$\r$\n$\r$\n1. Open your project folder.$\r$\n2. Connect your AI provider.$\r$\n3. Build something with Arny.$\r$\n$\r$\nYou can find ARN IDE in the Start menu."
LangString ArnWelcomeTitle 1058 "Місце для вашої наступної ідеї."
LangString ArnWelcomeText 1058 "Редактор, ШІ-помічник і ваш проєкт в одному просторі.$\r$\n$\r$\nARN буде встановлено для вашого облікового запису Windows. Проєкти залишаться на своїх місцях.$\r$\n$\r$\nПідключіть ШІ після встановлення. Зміни файлів потребують вашого підтвердження.$\r$\n$\r$\nПідготуймо простір для роботи."
LangString ArnFinishTitle 1058 "Ваш робочий простір готовий."
LangString ArnFinishText 1058 "ARN IDE встановлено.$\r$\n$\r$\n1. Відкрийте папку проєкту.$\r$\n2. Підключіть ШІ-провайдера.$\r$\n3. Створюйте разом з Arny.$\r$\n$\r$\nARN IDE доступна в меню «Пуск»."

; The Microsoft Visual C++ runtime is a prerequisite: ARN does not ship it, and
; this installer never downloads or installs it. installer/runtime/vc-runtime.nsh
; (written by scripts/prepare-installer.mjs) lists the DLLs that the bundled
; arn.exe needs. When one is missing from System32, the installer stops before
; it copies anything (an existing installation stays as it was) and points to
; Microsoft's download; the check needs no administrator rights.
!include /NONFATAL "${__FILEDIR__}\runtime\vc-runtime.nsh"
LangString ArnVcRuntimeMissing 1033 "ARN IDE needs the Microsoft Visual C++ Redistributable (x64), which is not installed on this computer (missing:$R9).$\r$\n$\r$\nInstall it from Microsoft:$\r$\nhttps://aka.ms/vc14/vc_redist.x64.exe$\r$\n(installing it needs administrator rights), then run this installer again.$\r$\n$\r$\nARN IDE was not installed."
LangString ArnVcRuntimeOpen 1033 "Open this link in your browser now?"
LangString ArnVcRuntimeMissing 1058 "ARN IDE потребує Microsoft Visual C++ Redistributable (x64), якого на цьому комп'ютері немає (бракує:$R9).$\r$\n$\r$\nВстановіть його з сайту Microsoft:$\r$\nhttps://aka.ms/vc14/vc_redist.x64.exe$\r$\n(для встановлення потрібні права адміністратора), а потім запустіть цей інсталятор знову.$\r$\n$\r$\nARN IDE не встановлено."
LangString ArnVcRuntimeOpen 1058 "Відкрити це посилання в браузері зараз?"

!macro NSIS_HOOK_PREINSTALL
  !ifmacrodef ARN_FIND_MISSING_VC_RUNTIME
    !insertmacro ARN_FIND_MISSING_VC_RUNTIME
    ${If} $R9 != ""
      ; Tauri's Install section has made $INSTDIR the working directory; leave
      ; it, then remove it only if it is empty (it was created just now).
      SetOutPath "$TEMP"
      RMDir "$INSTDIR"
      ${If} ${Silent}
        System::Call 'kernel32::AttachConsole(i -1)i.r0'
        ${If} $0 <> 0
          System::Call 'kernel32::GetStdHandle(i -11)i.r0'
          FileWrite $0 "$(ArnVcRuntimeMissing)$\r$\n"
        ${EndIf}
      ${Else}
        MessageBox MB_YESNO|MB_ICONEXCLAMATION "$(ArnVcRuntimeMissing)$\r$\n$\r$\n$(ArnVcRuntimeOpen)" IDNO arn_vc_runtime_reported
        ExecShell "open" "${ARN_VC_REDIST_URL}"
        arn_vc_runtime_reported:
      ${EndIf}
      SetErrorLevel 3
      Abort "$(ArnVcRuntimeMissing)"
    ${EndIf}
  !endif
!macroend
