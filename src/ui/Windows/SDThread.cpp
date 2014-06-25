/*
* Copyright (c) 2014 David Kelvin <c-273@users.sourceforge.net>.
* All rights reserved. Use of the code is allowed under the
* Artistic License 2.0 terms, as specified in the LICENSE file
* distributed with this code, or available from
* http://www.opensource.org/licenses/artistic-license-2.0.php
*/

// ThreadedDlg.cpp : implementation file
//

#include "stdafx.h"

#include "SDThread.h"
#include "VirtualKeyboard/VKeyBoardDlg.h"

#include "ThisMfcApp.h"
#include "resource3.h"

#include "core/PWSprefs.h"
#include "core/PWPolicy.h"
#include "core/PWCharPool.h" // for CheckPassword()
#include "os/debug.h"

#include "resource.h"

#include <algorithm>
#include <Imm.h>

#pragma comment(lib, "Imm32.lib")

using namespace std;

int iStartTime;  // Start time for SD timer - does get reset by edit changes or mousclicks (VK)

extern ThisMfcApp app;

extern LRESULT CALLBACK MsgFilter(int code, WPARAM wParam, LPARAM lParam);

CSDThread::CSDThread(GetMasterPhrase *pGMP, CBitmap *pbmpDimmedScreen, const int iDialogID, HMONITOR hCurrentMonitor)
  : m_pGMP(pGMP), m_pbmpDimmedScreen(pbmpDimmedScreen), m_wDialogID((WORD)iDialogID), m_hCurrentMonitor(hCurrentMonitor),
  m_hNewDesktop(NULL), m_hwndBkGnd(NULL), m_hwndMasterPhraseDlg(NULL), m_pVKeyBoardDlg(NULL),
  m_bVKCreated(false), m_bDoTimerProcAction(false), m_bMPWindowBeingShown(false), m_bVKWindowBeingShown(false),
  m_iMinutes(-1), m_iSeconds(-1)
{
  InitInstance();
}

CSDThread::~CSDThread()
{
}

BOOL CSDThread::InitInstance()
{
  // Get us
  m_hInstance = GetModuleHandle(NULL);

  // Only called once the Thread is "Resumed"
  _AFX_THREAD_STATE *pState = AfxGetThreadState();

  if (pState->m_hHookOldMsgFilter)
  {
    if (!UnhookWindowsHookEx(pState->m_hHookOldMsgFilter)) {
      pws_os::IssueError(_T("UnhookWindowsHookEx"), false);
      ASSERT(0);
    }
    pState->m_hHookOldMsgFilter = NULL;
  }

  m_pGMP->clear();

  m_bUseSecureDesktop = PWSprefs::GetInstance()->GetPref(PWSprefs::UseSecureDesktop);
  m_iUserTimeLimit = PWSprefs::GetInstance()->GetPref(PWSprefs::SecureDesktopTimeout);
  m_hTimer = NULL;

  // Clear progress flags
  xFlags = 0;

  return TRUE;
}

DWORD WINAPI CSDThread::ThreadProc(LPVOID lpParameter)
{
  CSDThread *selfThreadProc = (CSDThread *)lpParameter;

  WNDCLASS wc = { 0 };

  StringX sxTemp, sxPrefix;
  DWORD dwError;
  selfThreadProc->m_dwRC = (DWORD)-1;

  selfThreadProc->m_pGMP->clear();
  selfThreadProc->m_hwndVKeyBoard = NULL;

  PWPolicy policy;

  // Get uppercase prefix - 1st character for Desktop name, 2nd for Window Class name
  policy.flags = PWPolicy::UseUppercase;
  policy.length = 2;
  policy.upperminlength = 2;

  sxPrefix = policy.MakeRandomPassword();

  // Future use of this is for the next 15 characters of Dekstop & Window Class names
  policy.flags = PWPolicy::UseLowercase | PWPolicy::UseUppercase | PWPolicy::UseDigits;
  policy.length = 15;
  policy.lowerminlength = policy.upperminlength = policy.digitminlength = 1;

#ifndef NO_NEW_DESKTOP
  selfThreadProc->m_hOriginalDesk = GetThreadDesktop(GetCurrentThreadId());

  // Ensure we don't use an existing Desktop (very unlikely but....)
  do {
    //Create random Desktop name
    sxTemp = sxPrefix.substr(0, 1) + policy.MakeRandomPassword();

    selfThreadProc->m_sDesktopName = sxTemp.c_str();

    // Check not already there
    selfThreadProc->CheckDesktop();
  } while (selfThreadProc->m_bDesktopPresent);

  DWORD dwDesiredAccess = DESKTOP_CREATEWINDOW | DESKTOP_ENUMERATE |
    DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS | DESKTOP_SWITCHDESKTOP | STANDARD_RIGHTS_REQUIRED;

  selfThreadProc->m_hNewDesktop = CreateDesktop(selfThreadProc->m_sDesktopName.c_str(), NULL, NULL, 0, dwDesiredAccess, NULL);
  if (selfThreadProc->m_hNewDesktop == NULL) {
    dwError = pws_os::IssueError(_T("CreateDesktop (new)"), false);
    ASSERT(selfThreadProc->m_hNewDesktop);
    goto BadExit;
  }

  // Update Progress
  selfThreadProc->xFlags |= NEWDESKTOCREATED;

  // The following 3 calls must be in this order to ensure correct operation
  // Need to disable creation of ctfmon.exe in order to close desktop
  // On systems running NVIDIA Display Driver Service (nvsvc), CloseDesktop will also
  // NOT close the new Desktop until the service is stop or stop/restarted
  // THERE MAY BE OTHER PROGRAMS OR SERVICES THAT WILL STOP NEW DESKTOPS CLOSING
  // UNTIL THEY END (PROGRAMS) OR ARE STOPPED (SERVICES).
  if (!ImmDisableIME(0)) {
    dwError = pws_os::IssueError(_T("ImmDisableIME"), false);
    // No need to ASSERT here
  }

  if (!SetThreadDesktop(selfThreadProc->m_hNewDesktop)) {
    dwError = pws_os::IssueError(_T("SetThreadDesktop to new"), false);
    ASSERT(0);
    goto BadExit;
  }

  // Update Progress
  selfThreadProc->xFlags |= SETTHREADDESKTOP;

  if (!SwitchDesktop(selfThreadProc->m_hNewDesktop)) {
    dwError = pws_os::IssueError(_T("SwitchDesktop to new"), false);
    ASSERT(0);
    goto BadExit;
  }

  // Update Progress
  selfThreadProc->xFlags |= SWITCHEDDESKTOP;
#endif

  // Ensure we don't use an existing Window Class Name (very unlikely but....)
  do {
    //Create random Modeless Overlayed Background Window Class Name
    sxTemp = sxPrefix.substr(1, 1) + policy.MakeRandomPassword();

    selfThreadProc->m_sBkGrndClassName = sxTemp.c_str();

    // Check not already there
    selfThreadProc->CheckWindow();
  } while (selfThreadProc->m_bWindowPresent);

  // Register the Window Class Name
  wc.lpfnWndProc = ::DefWindowProc;
  wc.hInstance = selfThreadProc->m_hInstance;
  wc.lpszClassName = selfThreadProc->m_sBkGrndClassName.c_str();
  if (!RegisterClass(&wc)) {
    dwError = pws_os::IssueError(_T("RegisterClass - Background Window"), false);
    ASSERT(0);
    goto BadExit;
  }

  // Update Progress
  selfThreadProc->xFlags |= REGISTEREDWINDOWCLASS;

  selfThreadProc->m_hwndBkGnd = CreateWindowEx(WS_EX_LAYERED | WS_EX_TOOLWINDOW,
    selfThreadProc->m_sBkGrndClassName.c_str(), NULL, WS_POPUP | WS_VISIBLE,
    0, 0, ::GetSystemMetrics(SM_CXVIRTUALSCREEN), ::GetSystemMetrics(SM_CYVIRTUALSCREEN),
    NULL, NULL, selfThreadProc->m_hInstance, NULL);

  if (!selfThreadProc->m_hwndBkGnd) {
    dwError = pws_os::IssueError(_T("CreateWindowEx - Background"), false);
    ASSERT(selfThreadProc->m_hwndBkGnd);
    goto BadExit;
  }

  // Update Progress
  selfThreadProc->xFlags |= BACKGROUNDWINDOWCREATED;

  selfThreadProc->SetBkGndImage(selfThreadProc->m_hwndBkGnd);

  // Don't allow any action if this is clicked!
  EnableWindow(selfThreadProc->m_hwndBkGnd, FALSE);// iDialogID - IDD_SDGETPHRASE

  selfThreadProc->m_hwndMasterPhraseDlg = CreateDialogParam(selfThreadProc->m_hInstance,
    MAKEINTRESOURCE(selfThreadProc->m_wDialogID),
    HWND_DESKTOP, (DLGPROC)selfThreadProc->MPDialogProc, (LPARAM)selfThreadProc);

  if (!selfThreadProc->m_hwndMasterPhraseDlg) {
    dwError = pws_os::IssueError(_T("CreateDialogParam - IDD_SDGETPHRASE"), false);
    ASSERT(0);
    goto BadExit;
  }

  // Update Progress
  selfThreadProc->xFlags |= MASTERPHRASEDIALOGCREATED;

  selfThreadProc->m_pVKeyBoardDlg = new CVKeyBoardDlg(selfThreadProc->m_hwndBkGnd, selfThreadProc->m_hwndMasterPhraseDlg);

  // Update Progress
  selfThreadProc->xFlags |= VIRTUALKEYBOARDCREATED;

  ShowWindow(selfThreadProc->m_hwndMasterPhraseDlg, SW_SHOW);

  MSG msg;
  BOOL brc;

  // Message loop - break out on WM_QUIT or error
  while ((brc = GetMessage(&msg, 0, 0, 0)) != 0) {
    if (brc == -1)
      break;

    if (!IsDialogMessage(selfThreadProc->m_hwndMasterPhraseDlg, &msg)) {
      TranslateMessage(&msg);
      DispatchMessage(&msg);
    }
  }

  // Call DialogProc directly to clear "selfThreadProc".
  // NOTE: - it would NEVER get the WM_QUIT message EVER as this
  // is used to get out of the message loop above(see GetMessage)
  selfThreadProc->MPDialogProc(NULL, WM_QUIT, NULL, NULL);

  // Update Progress
  selfThreadProc->xFlags |= MASTERPHRASEDIALOGENDED;

  // Destroy Masterphrase window
  if (!DestroyWindow(selfThreadProc->m_hwndMasterPhraseDlg)) {
    dwError = pws_os::IssueError(_T("DestroyWindow - IDD_SDGETPHRASE"), false);
    ASSERT(0);
    goto BadExit;
  }

  // Update Progress
  selfThreadProc->xFlags &= ~MASTERPHRASEDIALOGCREATED;

  // Delete Virtual Keyboard instance
  delete selfThreadProc->m_pVKeyBoardDlg;

  // Update Progress
  selfThreadProc->xFlags &= ~VIRTUALKEYBOARDCREATED;

  // Destroy background layered window
  if (!DestroyWindow(selfThreadProc->m_hwndBkGnd)) {
    dwError = pws_os::IssueError(_T("DestroyWindow - Background"), false);
    ASSERT(0);
    goto BadExit;
  }

  // Update Progress
  selfThreadProc->xFlags &= ~BACKGROUNDWINDOWCREATED;

  // Unregister it
  if (!UnregisterClass(selfThreadProc->m_sBkGrndClassName.c_str(), selfThreadProc->m_hInstance)) {
    dwError = pws_os::IssueError(_T("UnregisterClass - Background"), false);
    ASSERT(0);
    goto BadExit;
  }

  // Update Progress
  selfThreadProc->xFlags &= ~REGISTEREDWINDOWCLASS;

  selfThreadProc->m_pbmpDimmedScreen->DeleteObject();

  // Clear variables
  selfThreadProc->m_pVKeyBoardDlg = NULL;
  selfThreadProc->m_hwndMasterPhraseDlg = NULL;
  selfThreadProc->m_sBkGrndClassName.clear();
  selfThreadProc->m_sDesktopName.clear();
  selfThreadProc->m_hwndBkGnd = NULL;

#ifndef NO_NEW_DESKTOP
  // The following 2 calls must be in this order to ensure the new desktop is
  // correctly deleted when finished with - EXCEPT in Winodws 7 (MS bug?)

  // Switch back to the initial desktop
  if (!SwitchDesktop(selfThreadProc->m_hOriginalDesk)) {
    dwError = pws_os::IssueError(_T("SwitchDesktop - back to original"), false);
    ASSERT(0);
    goto BadExit;
  }

  // Update Progress
  selfThreadProc->xFlags &= ~SWITCHEDDESKTOP;

  if (!SetThreadDesktop(selfThreadProc->m_hOriginalDesk)) {
    dwError = pws_os::IssueError(_T("SetThreadDesktop - back to original"), false);
    ASSERT(0);
    goto BadExit;
  }
  // Update Progress
  selfThreadProc->xFlags &= ~SETTHREADDESKTOP;

  // Now that thread is ending - close new desktop
  if (selfThreadProc->xFlags & NEWDESKTOCREATED) {
    if (!CloseDesktop(selfThreadProc->m_hNewDesktop)) {
      dwError = pws_os::IssueError(_T("CloseDesktop (new)"), false);
      ASSERT(0);
    }
  }
  // Update Progress
  selfThreadProc->xFlags &= ~NEWDESKTOCREATED;
#endif
  return selfThreadProc->m_dwRC;

BadExit:
  // Need to tidy up what was done in reverse order - ignoring what wasn't and ignore errors
  if (selfThreadProc->xFlags & VIRTUALKEYBOARDCREATED) {
    // Delete Virtual Keyboard instance
    delete selfThreadProc->m_pVKeyBoardDlg;
  }
  if (selfThreadProc->xFlags & MASTERPHRASEDIALOGCREATED) {
    DestroyWindow(selfThreadProc->m_hwndMasterPhraseDlg);
  }
  if (selfThreadProc->xFlags & BACKGROUNDWINDOWCREATED) {
    DestroyWindow(selfThreadProc->m_hwndBkGnd);
  }
  if (selfThreadProc->xFlags & REGISTEREDWINDOWCLASS) {
    UnregisterClass(selfThreadProc->m_sBkGrndClassName.c_str(), selfThreadProc->m_hInstance);
  }
  if (selfThreadProc->xFlags & SWITCHEDDESKTOP) {
    SwitchDesktop(selfThreadProc->m_hOriginalDesk);
  }
  if (selfThreadProc->xFlags & SETTHREADDESKTOP) {
    SetThreadDesktop(selfThreadProc->m_hOriginalDesk);
  }
  if (selfThreadProc->xFlags & NEWDESKTOCREATED) {
    CloseDesktop(selfThreadProc->m_hNewDesktop);
  }
  return (DWORD)-1;
}

// Is Desktop there?
BOOL CALLBACK CSDThread::DesktopEnumProc(LPTSTR name, LPARAM lParam)
{
  CSDThread *self = (CSDThread *)lParam;

  // If already there, set flag and no need to be called again
  if (_tcscmp(name, self->m_sDesktopName.c_str()) == 0) {
    self->m_bDesktopPresent = true;
    return FALSE;
  }

  return TRUE;
}

void CSDThread::CheckDesktop()
{
  m_bDesktopPresent = false;

  // Check if Desktop already created and still there
  HWINSTA station = GetProcessWindowStation();
  EnumDesktops(station, (DESKTOPENUMPROC)DesktopEnumProc, (LPARAM)this);
  CloseWindowStation(station);
}

// Is Window there?
BOOL CALLBACK CSDThread::WindowEnumProc(HWND hwnd, LPARAM lParam)
{
  CSDThread *self = (CSDThread *)lParam;

  // Get Window Class Name
  const int nMaxCOunt = 256;
  TCHAR szClassName[nMaxCOunt] = { 0 };
  int irc = GetClassName(hwnd, szClassName, nMaxCOunt);
  if (irc == 0) {
    pws_os::IssueError(_T("WindowEnumProc - Error return from GetClassName"), false);
    ASSERT(0);
    self->m_bWindowPresent = true;
    return FALSE;
  }

  // If already there, set flag and no need to be called again
  if (_tcscmp(szClassName, self->m_sBkGrndClassName.c_str()) == 0) {
    self->m_bWindowPresent = true;
    return FALSE;
  }

  return TRUE;
}

void CSDThread::CheckWindow()
{
  m_bWindowPresent = false;

  // Populate vector with desktop names.
  HWINSTA station = GetProcessWindowStation();
  EnumWindows((WNDENUMPROC)WindowEnumProc, (LPARAM)this);
  CloseWindowStation(station);
}

StringX GetControlText(const HWND hwnd)
{
  int n = GetWindowTextLength(hwnd) + 1;
  if (n > 1)
  {
    StringX s(n, 0);
    GetWindowText(hwnd, &s[0], n);
    s.pop_back();  // Remove trailing NULL [C++11 feature]
    return s;
  }
  return L"";
}

INT_PTR CSDThread::MPDialogProc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
  /**
      NOTE: Normally return code TRUE meaning it has processed this message and FALSE meaning it did not.

      However - MS Dpcumentation is conflicting!

      The following messages have different rules:
      WM_CHARTOITEM
      WM_COMPAREITEM
      WM_CTLCOLORBTN
      WM_CTLCOLORDLG
      WM_CTLCOLOREDIT
      WM_CTLCOLORLISTBOX
      WM_CTLCOLORSCROLLBAR
      WM_CTLCOLORSTATIC
      WM_INITDIALOG
      WM_QUERYDRAGICON
      WM_VKEYTOITEM
  **/

  static CSDThread *selfMPProc(NULL);

  BOOL brc;
  DWORD dwError;

  if (uMsg != WM_INITDIALOG && selfMPProc == NULL)
    return (INT_PTR)FALSE;

  switch (uMsg) {
  case WM_INITDIALOG:
  {
    selfMPProc = (CSDThread *)lParam;

    selfMPProc->m_hwndDlg = hwndDlg;

    selfMPProc->m_hwndStaticTimer = GetDlgItem(hwndDlg, IDC_STATIC_TIMER);
    selfMPProc->m_hwndStaticTimerText = GetDlgItem(hwndDlg, IDC_STATIC_TIMERTEXT);
    selfMPProc->m_hwndStaticSeconds = GetDlgItem(hwndDlg, IDC_STATIC_SECONDS);

    int iMinutes = selfMPProc->m_iUserTimeLimit / 60;
    int iSeconds = selfMPProc->m_iUserTimeLimit - (60 * iMinutes);
    stringT sTime;
    Format(sTime, _T("%02d:%02d"), iMinutes, iSeconds);
    SetWindowText(selfMPProc->m_hwndStaticTimer, sTime.c_str());

    // Secure Desktop toggle button image transparent mask
    selfMPProc->m_cfMask = RGB(255, 255, 255);

    if (selfMPProc->m_bUseSecureDesktop)
    {
      // Seure Desktop toggle bitmap
      selfMPProc->m_IDB = IDB_USING_SD;

      // Set up timer - fires every 100 milliseconds
      brc = CreateTimerQueueTimer(&(selfMPProc->m_hTimer), NULL, (WAITORTIMERCALLBACK)TimerProc,
        selfMPProc, 0, 100, 0);
      if (brc == NULL) {
        dwError = pws_os::IssueError(_T("CreateTimerQueueTimer"), false);
        ASSERT(brc);
      }

      // Get start time in milliseconds
      iStartTime = GetTickCount();
    }
    else
    {
      // Seure Desktop toggle bitmap
      selfMPProc->m_IDB = IDB_NOT_USING_SD;

      // Not using Secure Desktop - hide timer
      ShowWindow(selfMPProc->m_hwndStaticTimer, SW_HIDE);

      HWND hwndStaticTimerText = GetDlgItem(hwndDlg, IDC_STATIC_TIMERTEXT);
      ShowWindow(hwndStaticTimerText, SW_HIDE);
    }

    // Create the tooltip
    selfMPProc->m_hwndTooltip = CreateWindowEx(NULL, TOOLTIPS_CLASS, NULL,
      WS_POPUP | WS_EX_TOOLWINDOW | TTS_ALWAYSTIP | TTS_BALLOON | TTS_NOPREFIX,
      CW_USEDEFAULT, CW_USEDEFAULT,
      CW_USEDEFAULT, CW_USEDEFAULT,
      hwndDlg, NULL,
      GetModuleHandle(NULL), NULL);

    if (!selfMPProc->m_hwndTooltip)
      ASSERT(0);

    SendMessage(selfMPProc->m_hwndTooltip, TTM_SETMAXTIPWIDTH, 0, (LPARAM)300);

    //int iTime = SendMessage(selfMPProc->m_hwndTooltip, TTM_GETDELAYTIME, TTDT_AUTOPOP, NULL);
    SendMessage(selfMPProc->m_hwndTooltip, TTM_SETDELAYTIME, TTDT_INITIAL, 1000);       // Default  500 ms
    SendMessage(selfMPProc->m_hwndTooltip, TTM_SETDELAYTIME, TTDT_AUTOPOP, 5000);       // Default 5000 ms
    SendMessage(selfMPProc->m_hwndTooltip, TTM_SETDELAYTIME, TTDT_RESHOW,  1000);       // Default  100 ms

    selfMPProc->AddTooltip(IDC_SD_TOGGLE, IDS_TOGGLE_SECURE_DESKTOP_ON);

    // Activate tooltips
    SendMessage(selfMPProc->m_hwndTooltip, TTM_ACTIVATE, TRUE, NULL);

    // Centre in monitor having previous dialog
    // Get current Monitor information
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    GetMonitorInfo(selfMPProc->m_hCurrentMonitor, &mi);

    // Get Window rectangle
    CRect wRect;
    GetWindowRect(hwndDlg, &wRect);

    // Get windows width/height
    int wWidth = wRect.right - wRect.left;
    int wHeight = wRect.bottom - wRect.top;

    // Centre it
    int wLeft = mi.rcMonitor.left + (mi.rcMonitor.right - mi.rcMonitor.left - wWidth) / 2;
    int wTop = mi.rcMonitor.top + (mi.rcMonitor.bottom - mi.rcMonitor.top - wHeight) / 2;

    SetWindowPos(hwndDlg, HWND_TOP, wLeft, wTop, 0, 0, SWP_NOSIZE);

    // Tell TimerProc to do its thing
    selfMPProc->m_bDoTimerProcAction = true;

    return (INT_PTR)TRUE; // Processed - special case
  }  // WM_INITDIALOG

  case WM_SHOWWINDOW:
  {
    selfMPProc->m_bMPWindowBeingShown = (BOOL)wParam == TRUE;

    return (INT_PTR)FALSE;  // Processed!
  }

  case WM_COMMAND:
  {
    const int iControlID = LOWORD(wParam);
    const int iNotificationCode = HIWORD(wParam);

    // lParam == handle to the control window
    switch (iControlID) {
    case IDC_VKB:
    {
      // Shouldn't be here if couldn't load DLL. Static control disabled/hidden
      if (!CVKeyBoardDlg::IsOSKAvailable())
        return (INT_PTR)TRUE; // Processed

      if (selfMPProc->m_hwndVKeyBoard != NULL && IsWindowVisible(selfMPProc->m_hwndVKeyBoard)) {
        // Already there - move to top and enable it
        SetWindowPos(selfMPProc->m_hwndVKeyBoard, HWND_TOP, 0, 0, 0, 0, SWP_SHOWWINDOW | SWP_NOMOVE | SWP_NOSIZE);
        EnableWindow(selfMPProc->m_hwndVKeyBoard, TRUE);
        return (INT_PTR)TRUE; // Processed
      }

      // If not already created - do it, otherwise just reset it
      if (selfMPProc->m_hwndVKeyBoard == NULL) {
        StringX cs_LUKBD = PWSprefs::GetInstance()->GetPref(PWSprefs::LastUsedKeyboard);
        selfMPProc->m_hwndVKeyBoard = CreateDialogParam(selfMPProc->m_hInstance, MAKEINTRESOURCE(IDD_SDVKEYBOARD), selfMPProc->m_hwndMasterPhraseDlg,
          (DLGPROC)(selfMPProc->m_pVKeyBoardDlg->VKDialogProc), (LPARAM)(selfMPProc->m_pVKeyBoardDlg));

        if (selfMPProc->m_hwndVKeyBoard == NULL) {
          dwError = pws_os::IssueError(_T("CreateDialogParam - IDD_SDVKEYBOARD"), false);
          ASSERT(selfMPProc->m_hwndVKeyBoard);
        }
      }
      else {
        selfMPProc->m_pVKeyBoardDlg->ResetKeyboard();
      }

      // Now show it and make it top & enable it
      SetWindowPos(selfMPProc->m_hwndVKeyBoard, HWND_TOP, 0, 0, 0, 0, SWP_SHOWWINDOW | SWP_NOMOVE | SWP_NOSIZE);
      EnableWindow(selfMPProc->m_hwndVKeyBoard, TRUE);

      return (INT_PTR)TRUE; // Processed
    }  // IDC_VKB


    case IDC_PASSKEY:
    case IDC_NEWPASSKEY:
    case IDC_VERIFY:
    case IDC_CONFIRMNEW:
      if (iNotificationCode == EN_SETFOCUS)
      {
        selfMPProc->m_iLastFocus = iControlID;
        // Don't say we have processed to let default action occur
        return (INT_PTR)FALSE;
      }
      if (iNotificationCode == EN_CHANGE)
      {
        // Reset timer start time
        iStartTime = GetTickCount();
        return (INT_PTR)FALSE;
      }
      break;

    case IDOK:
    {
      StringX sErrorMsg;

      /*
        selfThreadProc->m_wDialogID

          IDD_SDGETPHRASE      IDC_PASSKEY, IDC_VKB, IDOK, IDCANCEL
          IDD_SDKEYCHANGE      IDC_PASSKEY, IDC_NEWPASSKEY, IDC_CONFIRMNEW, IDC_VKB, IDOK, IDCANCEL
          IDD_SDPASSKEYSETUP   IDC_PASSKEY, IDC_VERIFY, IDC_VKB, IDOK, IDCANCEL
      */

      StringX sxPassKey, sxNewPassKey1, sxNewPassKey2, sxVerifyPassKey;

      HWND hwndPassKey = GetDlgItem(hwndDlg, IDC_PASSKEY);
      sxPassKey = GetControlText(hwndPassKey);

      if (!sxPassKey.empty()) {
        selfMPProc->m_pGMP->sPhrase = sxPassKey;
        selfMPProc->m_pGMP->bPhraseEntered = true;
      }
      else
      {
        LoadAString(sErrorMsg, selfMPProc->m_wDialogID == IDD_SDPASSKEYSETUP ? IDS_ENTERKEYANDVERIFY : IDS_CANNOTBEBLANK);
        MessageBox(hwndDlg, sErrorMsg.c_str(), NULL, MB_OK);
        SetFocus(hwndPassKey);
        return (INT_PTR)FALSE;
      }

      switch (selfMPProc->m_wDialogID) {
      case IDD_SDGETPHRASE:
      {
        // Just verify IDC_PASSKEY - done by caller
        // Tidy everything
        break;
      }
      case IDD_SDKEYCHANGE:
      {
        // Verify DC_PASSKEY, IDC_NEWPASSKEY, IDC_CONFIRMNEW
        UINT iMsgID(0);
        int rc = app.GetCore()->CheckPasskey(app.GetCore()->GetCurFile(), sxPassKey);

        HWND hwndFocus = hwndPassKey;

        if (rc == PWScore::WRONG_PASSWORD)
          iMsgID = IDS_WRONGOLDPHRASE;
        else if (rc == PWScore::CANT_OPEN_FILE)
          iMsgID = IDS_CANTVERIFY;
        else
        {
          HWND hwndNewPassKey1 = GetDlgItem(hwndDlg, IDC_NEWPASSKEY);
          sxNewPassKey1 = GetControlText(hwndNewPassKey1);

          HWND hwndNewPassKey2 = GetDlgItem(hwndDlg, IDC_CONFIRMNEW);
          sxNewPassKey2 = GetControlText(hwndNewPassKey2);

          if (sxNewPassKey1.empty()) {
            iMsgID = IDS_CANNOTBEBLANK;
            hwndFocus = hwndNewPassKey1;
          }
          else if (sxNewPassKey1 != sxNewPassKey2) {
            iMsgID = IDS_NEWOLDDONOTMATCH;
            hwndFocus = hwndNewPassKey2;
          }
        }

        if (iMsgID != 0) {
          LoadAString(sErrorMsg, iMsgID);
          MessageBox(hwndDlg, sErrorMsg.c_str(), NULL, MB_OK | MB_ICONSTOP);
          SetFocus(hwndFocus);
          return (INT_PTR)FALSE;
        }

        if (!CPasswordCharPool::CheckPassword(sxNewPassKey1, sErrorMsg)) {
          StringX sxMsg, sxText;
          Format(sxMsg, IDS_WEAKPASSPHRASE, sErrorMsg.c_str());

#ifndef PWS_FORCE_STRONG_PASSPHRASE
          LoadAString(sxText, IDS_USEITANYWAY);
          sxMsg += sxText;
          INT_PTR rc = MessageBox(hwndDlg, sxMsg.c_str(), NULL, MB_YESNO | MB_ICONSTOP);
          if (rc == IDNO)
            return (INT_PTR)FALSE;
#else
          LoadAString(sxText, IDS_TRYANOTHER);
          sxMsg += sxText;
          MessageBox(hwndDlg, sxMsg.c_str(), NULL, MB_OK | MB_ICONSTOP);
          return (INT_PTR)FALSE;
#endif  // PWS_FORCE_STRONG_PASSPHRASE
        }
        selfMPProc->m_pGMP->sNewPhrase = sxNewPassKey1;
        selfMPProc->m_pGMP->bNewPhraseEntered = true;
        break;
      }
      case IDD_SDPASSKEYSETUP:
      {
        // Verify DC_PASSKEY, IDC_VERIFY
        UINT iMsgID(0);
        HWND hwndFocus = hwndPassKey;
        {
          HWND hwndNewPassKey1 = GetDlgItem(hwndDlg, IDC_VERIFY);
          sxNewPassKey1 = GetControlText(hwndNewPassKey1);

          if (sxPassKey != sxNewPassKey1) {
            iMsgID = IDS_ENTRIESDONOTMATCH;
            hwndFocus = hwndNewPassKey1;
          }
        }

        if (iMsgID != 0) {
          LoadAString(sErrorMsg, iMsgID);
          MessageBox(hwndDlg, sErrorMsg.c_str(), NULL, MB_OK | MB_ICONSTOP);
          SetFocus(hwndFocus);
          return (INT_PTR)FALSE;
        }

        if (!CPasswordCharPool::CheckPassword(sxNewPassKey1, sErrorMsg)) {
          StringX sxMsg, sxText;
          Format(sxMsg, IDS_WEAKPASSPHRASE, sErrorMsg.c_str());

#ifndef PWS_FORCE_STRONG_PASSPHRASE
          LoadAString(sxText, IDS_USEITANYWAY);
          sxMsg += sxText;
          INT_PTR rc = MessageBox(hwndDlg, sxMsg.c_str(), NULL, MB_YESNO | MB_ICONSTOP);
          if (rc == IDNO)
            return (INT_PTR)FALSE;
#else
          LoadAString(sxText, IDS_TRYANOTHER);
          sxMsg += sxText;
          MessageBox(hwndDlg, sxMsg.c_str(), NULL, MB_OK | MB_ICONSTOP);
          return (INT_PTR)FALSE;
#endif  // PWS_FORCE_STRONG_PASSPHRASE
        }
        selfMPProc->m_pGMP->sNewPhrase = sxNewPassKey1;
        selfMPProc->m_pGMP->bNewPhraseEntered = true;
        break;
      }
      default:
        ASSERT(0);
      }

      // Tell TimerProc to do nothing
      selfMPProc->m_bDoTimerProcAction = false;

      if (selfMPProc->m_bVKCreated) {
        ASSERT(selfMPProc->m_hwndVKeyBoard);

        brc = DestroyWindow(selfMPProc->m_hwndVKeyBoard);
        if (brc == NULL) {
          dwError = pws_os::IssueError(_T("DestroyWindow - IDD_SDVKEYBOARD - IDOK"), false);
          ASSERT(brc);
        }

        selfMPProc->m_hwndVKeyBoard = NULL;
        selfMPProc->m_bVKCreated = false;
     }

      PostQuitMessage(IDOK);
      selfMPProc->m_dwRC = IDOK;
      return (INT_PTR)TRUE; // Processed
    }  // IDOK

    case IDCANCEL:
    {
      // Tell TimerProc to do nothing
      selfMPProc->m_bDoTimerProcAction = false;

      selfMPProc->m_pGMP->clear();

      if (selfMPProc->m_bVKCreated) {
        ASSERT(selfMPProc->m_hwndVKeyBoard);

        brc = DestroyWindow(selfMPProc->m_hwndVKeyBoard);
        if (brc == NULL) {
          dwError = pws_os::IssueError(_T("DestroyWindow - IDD_SDVKEYBOARD - IDCANCEL"), false);
          ASSERT(brc);
        }

        selfMPProc->m_hwndVKeyBoard = NULL;
        selfMPProc->m_bVKCreated = false;
      }

      PostQuitMessage(IDCANCEL);
      selfMPProc->m_dwRC = IDCANCEL;
      return (INT_PTR)TRUE; // Processed
    }

    case IDC_SD_TOGGLE:
    {
      PostQuitMessage(INT_MAX);
      selfMPProc->m_dwRC = INT_MAX;
      return (INT_PTR)TRUE; // Processed
    }
    }  // switch (iControlID)
    break;
  }  // WM_COMMAND

    case WM_DRAWITEM:
    {
      if (wParam == IDC_SD_TOGGLE) {
        LPDRAWITEMSTRUCT lpDrawItemStruct = (LPDRAWITEMSTRUCT)lParam;
        CDC dc;
        dc.Attach(lpDrawItemStruct->hDC);

        CBitmap bmp;
        bmp.LoadBitmap(selfMPProc->m_IDB);

        BITMAP bitMapInfo;
        bmp.GetBitmap(&bitMapInfo);

        CDC memDC;
        memDC.CreateCompatibleDC(&dc);

        memDC.SelectObject(&bmp);
        int bmw = bitMapInfo.bmWidth;
        int bmh = bitMapInfo.bmHeight;

        // Draw button image transparently
        ::TransparentBlt(dc.GetSafeHdc(), 0, 0, bmw, bmh, memDC.GetSafeHdc(), 0, 0, bmw, bmh, selfMPProc->m_cfMask);
        return TRUE;
      }
    }
  case PWS_MSG_INSERTBUFFER:
  {
    // Get the buffer
    StringX vkbuffer = selfMPProc->m_pVKeyBoardDlg->GetPassphrase();

    // Find the selected characters - if any
    DWORD nStartChar, nEndChar;
    HWND hedtPhrase = GetDlgItem(hwndDlg, selfMPProc->m_iLastFocus);

    SendMessage(hedtPhrase, EM_GETSEL, (WPARAM)&nStartChar, (LPARAM)&nEndChar);

    // Replace them or, if none selected, put at current cursor position
    SendMessage(hedtPhrase, EM_REPLACESEL, (WPARAM)FALSE, (LPARAM)(LPCWSTR)vkbuffer.c_str());

    // Put cursor at end of inserted text
    SendMessage(hedtPhrase, EM_SETSEL, nStartChar + vkbuffer.length(), nStartChar + vkbuffer.length());

    return (INT_PTR)TRUE; // Processed
  }  // PWS_MSG_INSERTBUFFER

  case WM_QUIT:
  {
    // Special handling for WM_QUIT, which it would NEVER EVER get normally
    ASSERT(selfMPProc);

    if (selfMPProc->m_bVKCreated && selfMPProc->m_hwndVKeyBoard != NULL) {
      brc = DestroyWindow(selfMPProc->m_hwndVKeyBoard);
      if (brc == NULL) {
        dwError = pws_os::IssueError(_T("DestroyWindow - IDD_SDVKEYBOARD - WM_QUIT"), false);
        ASSERT(brc);
      }

      selfMPProc->m_hwndVKeyBoard = NULL;
      selfMPProc->m_bVKCreated = false;
    }

    // Delete timer (only if set)
    if (selfMPProc->m_hTimer != NULL) {
      // Tell TimerProc to do nothing
      selfMPProc->m_bDoTimerProcAction = false;

      // Create an event for timer deletion
      HANDLE hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
      if (hEvent == NULL) {
        dwError = pws_os::IssueError(_T("CreateEvent in MPDialogProc"), false);
        ASSERT(hEvent);
      }

      // Delete all timers in the timer queue
      brc = DeleteTimerQueueTimer(NULL, selfMPProc->m_hTimer, hEvent);
      if (brc == NULL) {
        dwError = pws_os::IssueError(_T("DeleteTimerQueueTimer"), false);
        ASSERT(brc);
      }

      // Wait for timer queue to go
      WaitForSingleObject(hEvent, INFINITE);

      // Close the handle - NOT the timer handles
      CloseHandle(hEvent);
    }

    // Don't need it any more
    selfMPProc = NULL;

    return (INT_PTR)TRUE;
  }  // WM_QUIT

  }  // switch (uMsg)

  return (INT_PTR)FALSE; // Not processed
}

void CALLBACK CSDThread::TimerProc(LPVOID lpParameter, BOOLEAN )
{
  CSDThread *selfTimerProc = (CSDThread *)lpParameter;

  // Don't do anything if closing down
  if (!selfTimerProc->m_bDoTimerProcAction)
    return;

  // Don't do anything if windows aren't visible
  if (!selfTimerProc->m_bMPWindowBeingShown && !selfTimerProc->m_bVKWindowBeingShown)
    return;

  // Get time left in seconds
  int iTimeLeft = selfTimerProc->m_iUserTimeLimit - (GetTickCount() - iStartTime) / 1000;

  int iShow = (iTimeLeft <= selfTimerProc->m_iUserTimeLimit / 4) ? SW_SHOW : SW_HIDE;

  if (selfTimerProc->m_bMPWindowBeingShown || IsWindowVisible(selfTimerProc->m_hwndMasterPhraseDlg))
  {
    ShowWindow(selfTimerProc->m_hwndStaticTimer, iShow);
    ShowWindow(selfTimerProc->m_hwndStaticTimerText, iShow);
    ShowWindow(selfTimerProc->m_hwndStaticSeconds, iShow);
  }

  if (selfTimerProc->m_bVKWindowBeingShown || IsWindowVisible(selfTimerProc->m_hwndVKeyBoard))
  {
    ShowWindow(selfTimerProc->m_pVKeyBoardDlg->m_hwndVKStaticTimer, iShow);
    ShowWindow(selfTimerProc->m_pVKeyBoardDlg->m_hwndVKStaticTimerText, iShow);
    ShowWindow(selfTimerProc->m_pVKeyBoardDlg->m_hwndVKStaticSeconds, iShow);
  }

  if (iShow == SW_HIDE)
    return;

  int iMinutes = iTimeLeft / 60;
  int iSeconds = iTimeLeft - (60 * iMinutes);
  if (selfTimerProc->m_iMinutes != iMinutes || selfTimerProc->m_iSeconds != iSeconds) {
    stringT sTime;
    Format(sTime, _T("%02d:%02d"), iMinutes, iSeconds);

    if (selfTimerProc->m_bMPWindowBeingShown || IsWindowVisible(selfTimerProc->m_hwndMasterPhraseDlg))
    {
      SetWindowText(selfTimerProc->m_hwndStaticTimer, sTime.c_str());
    }

    if (selfTimerProc->m_bVKWindowBeingShown || IsWindowVisible(selfTimerProc->m_hwndVKeyBoard))
    {
      SetWindowText(selfTimerProc->m_pVKeyBoardDlg->m_hwndVKStaticTimer, sTime.c_str());
    }

    selfTimerProc->m_iMinutes = iMinutes;
    selfTimerProc->m_iSeconds = iSeconds;
  }
}

void CSDThread::SetBkGndImage(HWND hwndBkGnd)
{
  HBITMAP hbmpBkGnd = (HBITMAP)*m_pbmpDimmedScreen;

  // Get the size of the bitmap
  BITMAP bm;
  GetObject(*m_pbmpDimmedScreen, sizeof(bm), &bm);
  SIZE sizeBkGnd = { bm.bmWidth, bm.bmHeight };

  // Create a memory DC holding the BkGnd bitmap
  HDC hDCScreen = GetDC(NULL);
  HDC hDCMem = CreateCompatibleDC(hDCScreen);
  HBITMAP hbmpOld = (HBITMAP)SelectObject(hDCMem, hbmpBkGnd);

  // Use the source image's alpha channel for blending
  BLENDFUNCTION bf = { 0 };
  bf.BlendOp = AC_SRC_OVER;
  bf.SourceConstantAlpha = 255;
  bf.AlphaFormat = AC_SRC_ALPHA;

  POINT ptZero = { 0 };

  // Paint the window (in the right location) with the alpha-blended bitmap
  UpdateLayeredWindow(hwndBkGnd, hDCScreen, &ptZero, &sizeBkGnd,
    hDCMem, &ptZero, RGB(0, 0, 0), &bf, ULW_OPAQUE);

  // Delete temporary objects
  SelectObject(hDCMem, hbmpOld);
  DeleteDC(hDCMem);
  ReleaseDC(NULL, hDCScreen);
}

// Modified from MSDN: http://msdn.microsoft.com/en-us/library/bb760252(v=vs.85).aspx

BOOL CSDThread::AddTooltip(UINT uiControlID, stringT sText)
{
  if (!uiControlID || sText.empty())
    return FALSE;

  // Get the window of the tool.
  HWND hwndTool = GetDlgItem(m_hwndDlg, uiControlID);

  // Associate the tooltip with the tool.
  TOOLINFO ti = { 0 };
  ti.cbSize = sizeof(ti);
  ti.hwnd = m_hwndDlg;
  ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS | TTF_CENTERTIP | TTF_TRANSPARENT;
  ti.uId = (UINT_PTR)hwndTool;
  ti.lpszText = (LPWSTR)sText.c_str();

  BOOL brc = SendMessage(m_hwndTooltip, TTM_ADDTOOL, 0, (LPARAM)&ti);

  return brc;
}

BOOL CSDThread::AddTooltip(UINT uiControlID, UINT uiToolString, UINT uiFormat)
{
  if (!uiControlID || !uiToolString)
    return FALSE;

  stringT sText;
  LoadAString(sText, uiToolString);
  if (sText.empty())
    return FALSE;

  if (uiFormat != NULL)
  {
    Format(sText, uiFormat, sText.c_str());
  }

  return AddTooltip(uiControlID, sText);
}