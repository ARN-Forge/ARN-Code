; Keep Tauri's maintained install/upgrade/uninstall logic. Customize its UI only.
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
