
#include <Shlobj.h>
#include <Shlwapi.h>
#include <commctrl.h>

#include "IPlugParameter.h"
#include "IGraphicsWin.h"
#include "IControl.h"

#include <wininet.h>

#pragma warning(disable:4244) // Pointer size cast mismatch.
#pragma warning(disable:4312) // Pointer size cast mismatch.
#pragma warning(disable:4311) // Pointer size cast mismatch.

static int nWndClassReg = 0;
static const char* wndClassName = "IPlugWndClass";
static double sFPS = 0.0;

#define PARAM_EDIT_ID 99
#define IPLUG_TIMER_ID 2
#define IPLUG_WIN_MAX_WIDE_PATH 4096

// Unicode helpers


void UTF8ToUTF16(wchar_t* utf16Str, const char* utf8Str, int maxLen)
{
  int requiredSize = MultiByteToWideChar(CP_UTF8, 0, utf8Str, -1, NULL, 0);

  if (requiredSize > 0 && requiredSize <= maxLen)
  {
    MultiByteToWideChar(CP_UTF8, 0, utf8Str, -1, utf16Str, requiredSize);
    return;
  }

  utf16Str[0] = 0;
}

void UTF16ToUTF8(WDL_String& utf8Str, const wchar_t* utf16Str)
{
  int requiredSize = WideCharToMultiByte(CP_UTF8, 0, utf16Str, -1, NULL, 0, NULL, NULL);

  if (requiredSize > 0 && utf8Str.SetLen(requiredSize))
  {
    WideCharToMultiByte(CP_UTF8, 0, utf16Str, -1, utf8Str.Get(), requiredSize, NULL, NULL);
    return;
  }

  utf8Str.Set("");
}

// Helper for getting a known folder in UTF8

void GetKnownFolder(WDL_String &path, int identifier, int flags = 0)
{
  wchar_t wideBuffer[1024];

  SHGetFolderPathW(NULL, identifier, NULL, flags, wideBuffer);
  UTF16ToUTF8(path, wideBuffer);
}

inline IMouseInfo IGraphicsWin::GetMouseInfo(LPARAM lParam, WPARAM wParam)
{
  IMouseInfo info;
  info.x = mCursorX = GET_X_LPARAM(lParam) / GetScale();
  info.y = mCursorY = GET_Y_LPARAM(lParam) / GetScale();
  info.ms = IMouseMod((wParam & MK_LBUTTON), (wParam & MK_RBUTTON), (wParam & MK_SHIFT), (wParam & MK_CONTROL),
#ifdef AAX_API
    GetAsyncKeyState(VK_MENU) < 0
#else
    GetKeyState(VK_MENU) < 0
#endif
  );
  return info;
}

inline IMouseInfo IGraphicsWin::GetMouseInfoDeltas(float& dX, float& dY, LPARAM lParam, WPARAM wParam)
{
  float oldX = mCursorX;
  float oldY = mCursorY;
  
  IMouseInfo info = GetMouseInfo(lParam, wParam);

  dX = info.x - oldX;
  dY = info.y - oldY;
  
  return info;
}

// static
LRESULT CALLBACK IGraphicsWin::WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
  if (msg == WM_CREATE)
  {
    LPCREATESTRUCT lpcs = (LPCREATESTRUCT) lParam;
    SetWindowLongPtr(hWnd, GWLP_USERDATA, (LPARAM) (lpcs->lpCreateParams));
    int mSec = int(1000.0 / sFPS);
    SetTimer(hWnd, IPLUG_TIMER_ID, mSec, NULL);
    SetFocus(hWnd); // gets scroll wheel working straight away
    DragAcceptFiles(hWnd, true);
    return 0;
  }

  IGraphicsWin* pGraphics = (IGraphicsWin*) GetWindowLongPtr(hWnd, GWLP_USERDATA);
  char txt[MAX_WIN32_PARAM_LEN];
  double v;

  if (!pGraphics || hWnd != pGraphics->mDelegateWnd)
  {
    return DefWindowProc(hWnd, msg, wParam, lParam);
  }
  if (pGraphics->mParamEditWnd && pGraphics->mParamEditMsg == kEditing)
  {
    if (msg == WM_RBUTTONDOWN || (msg == WM_LBUTTONDOWN))
    {
      pGraphics->mParamEditMsg = kCancel;
      return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
  }

  switch (msg)
  {

    case WM_TIMER:
    {
      if (wParam == IPLUG_TIMER_ID)
      {
        if (pGraphics->mParamEditWnd && pGraphics->mParamEditMsg != kNone)
        {
          switch (pGraphics->mParamEditMsg)
          {
            case kCommit:
            {
              SendMessage(pGraphics->mParamEditWnd, WM_GETTEXT, MAX_WIN32_PARAM_LEN, (LPARAM) txt);

              const IParam* pParam = pGraphics->mEdControl->GetParam();
              
              if(pParam)
              {
                if (pParam->Type() == IParam::kTypeEnum || pParam->Type() == IParam::kTypeBool)
                {
                  double vi = 0.;
                  pParam->MapDisplayText(txt, &vi);
                  v = (double) vi;
                }
                else
                {
                  v = atof(txt);
                  if (pParam->GetNegateDisplay())
                  {
                    v = -v;
                  }
                }
                pGraphics->mEdControl->SetValueFromUserInput(pParam->ToNormalized(v));
              }
              else
              {
                pGraphics->mEdControl->OnTextEntryCompletion(txt);
              }
              // Fall through.
            }
            case kCancel:
            {
              SetWindowLongPtr(pGraphics->mParamEditWnd, GWLP_WNDPROC, (LPARAM) pGraphics->mDefEditProc);
              DestroyWindow(pGraphics->mParamEditWnd);
              pGraphics->mParamEditWnd = nullptr;
              pGraphics->mEdControl = nullptr;
              pGraphics->mDefEditProc = nullptr;
            }
            break;
          }
          pGraphics->mParamEditMsg = kNone;
          return 0; // TODO: check this!
        }

        IRECT dirtyR;
        if (pGraphics->IsDirty(dirtyR))
        {
          dirtyR.ScaleBounds(pGraphics->GetScale());
          RECT r = { (LONG) dirtyR.L, (LONG) dirtyR.T, (LONG) dirtyR.R, (LONG) dirtyR.B };

          InvalidateRect(hWnd, &r, FALSE);

          if (pGraphics->mParamEditWnd)
          {
            IRECT notDirtyR = pGraphics->mEdControl->GetRECT();
            notDirtyR.ScaleBounds(pGraphics->GetScale());
            RECT r2 = { (LONG) notDirtyR.L, (LONG) notDirtyR.T, (LONG) notDirtyR.R, (LONG) notDirtyR.B };
            ValidateRect(hWnd, &r2); // make sure we dont redraw the edit box area
            UpdateWindow(hWnd);
            pGraphics->mParamEditMsg = kUpdate;
          }
          else
          {
            UpdateWindow(hWnd);
          }
        }
      }
      return 0;
    }

    case WM_RBUTTONDOWN:
    case WM_LBUTTONDOWN:
    case WM_MBUTTONDOWN:
  {
      pGraphics->HideTooltip();
      if (pGraphics->mParamEditWnd)
      {
        pGraphics->mParamEditMsg = kCommit;
        return 0;
      }
      SetFocus(hWnd); // Added to get keyboard focus again when user clicks in window
      SetCapture(hWnd);
      IMouseInfo info = pGraphics->GetMouseInfo(lParam, wParam);
      pGraphics->OnMouseDown(info.x, info.y, info.ms);
      return 0;
    }

    case WM_MOUSEMOVE:
    {
      if (!(wParam & (MK_LBUTTON | MK_RBUTTON)))
      {
        IMouseInfo info = pGraphics->GetMouseInfo(lParam, wParam);
    if (pGraphics->OnMouseOver(info.x, info.y, info.ms))
        {
          TRACKMOUSEEVENT eventTrack = { sizeof(TRACKMOUSEEVENT), TME_LEAVE, hWnd, HOVER_DEFAULT };
          if (pGraphics->TooltipsEnabled()) 
          {
            int c = pGraphics->GetMouseOver();
            if (c != pGraphics->mTooltipIdx) 
            {
              if (c >= 0) eventTrack.dwFlags |= TME_HOVER;
              pGraphics->mTooltipIdx = c;
              pGraphics->HideTooltip();
            }
          }

          TrackMouseEvent(&eventTrack);
        }
      }
      else if (GetCapture() == hWnd && !pGraphics->mParamEditWnd)
      {
        float dX, dY;
        IMouseInfo info = pGraphics->GetMouseInfoDeltas(dX, dY, lParam, wParam);
        pGraphics->OnMouseDrag(info.x, info.y, dX, dY, info.ms);
      }

      return 0;
    }
    case WM_MOUSEHOVER: 
    {
      pGraphics->ShowTooltip();
      return 0;
    }
    case WM_MOUSELEAVE:
    {
      pGraphics->HideTooltip();
      pGraphics->OnMouseOut();
      return 0;
    }
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    {
      ReleaseCapture();
      IMouseInfo info = pGraphics->GetMouseInfo(lParam, wParam);
      pGraphics->OnMouseUp(info.x, info.y, info.ms);
      return 0;
    }
    case WM_LBUTTONDBLCLK:
    {
      IMouseInfo info = pGraphics->GetMouseInfo(lParam, wParam);
      if (pGraphics->OnMouseDblClick(info.x, info.y, info.ms))
      {
        SetCapture(hWnd);
      }
      return 0;
    }
    case WM_MOUSEWHEEL:
    {
      if (pGraphics->mParamEditWnd)
      {
        pGraphics->mParamEditMsg = kCancel;
        return 0;
      }
      else
      {
        IMouseInfo info = pGraphics->GetMouseInfo(lParam, wParam);
        float d = GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA;
        RECT r;
        GetWindowRect(hWnd, &r);
        pGraphics->OnMouseWheel(info.x - r.left, info.y - r.top, info.ms, d);
        return 0;
      }
    }

    case WM_KEYDOWN:
    {
      bool handle = true;
      int key;

      if (wParam == VK_SPACE) key = KEY_SPACE;
      else if (wParam == VK_UP) key = KEY_UPARROW;
      else if (wParam == VK_DOWN) key = KEY_DOWNARROW;
      else if (wParam == VK_LEFT) key = KEY_LEFTARROW;
      else if (wParam == VK_RIGHT) key = KEY_RIGHTARROW;
      else if (wParam >= '0' && wParam <= '9') key = KEY_DIGIT_0+wParam-'0';
      else if (wParam >= 'A' && wParam <= 'Z') key = KEY_ALPHA_A+wParam-'A';
      else if (wParam >= 'a' && wParam <= 'z') key = KEY_ALPHA_A+wParam-'a';
      else handle = false;

      if (handle)
      {
        POINT p;
        GetCursorPos(&p);
        ScreenToClient(hWnd, &p);
        handle = pGraphics->OnKeyDown(p.x, p.y, key);
      }

      if (!handle)
      {
        HWND rootHWnd = GetAncestor( hWnd, GA_ROOT);
        SendMessage(rootHWnd, WM_KEYDOWN, wParam, lParam);
        return DefWindowProc(hWnd, msg, wParam, lParam);
      }
      else
        return 0;
    }
    case WM_KEYUP:
    {
      HWND rootHWnd = GetAncestor(hWnd, GA_ROOT);
      SendMessage(rootHWnd, msg, wParam, lParam);
      return DefWindowProc(hWnd, msg, wParam, lParam);
    }
    case WM_PAINT:
    {
      RECT r;
      if (GetUpdateRect(hWnd, &r, FALSE))
      { 
        IRECT ir(r.left, r.top, r.right, r.bottom);
        ir.ScaleBounds(1. / pGraphics->GetScale());
        pGraphics->Draw(ir);
      }
      return 0;
    }

    case WM_CTLCOLOREDIT:
    {
      if(!pGraphics->mEdControl)
        return 0;

      const IText& text = pGraphics->mEdControl->GetText();
      HDC dc = (HDC) wParam;
      SetBkColor(dc, RGB(text.mTextEntryBGColor.R, text.mTextEntryBGColor.G, text.mTextEntryBGColor.B));
      SetTextColor(dc, RGB(text.mTextEntryFGColor.R, text.mTextEntryFGColor.G, text.mTextEntryFGColor.B));
      SetBkMode(dc, OPAQUE);
      return (BOOL) GetStockObject(DC_BRUSH);
    }
    case WM_DROPFILES:
    {
      HDROP hdrop = (HDROP)wParam;
      
      char pathToFile[1025];
      DragQueryFile(hdrop, 0, pathToFile, 1024);
      
      POINT point;
      DragQueryPoint(hdrop, &point);
      
      pGraphics->OnDrop(pathToFile, point.x, point.y);
      
      return 0;
    }
    case WM_CLOSE:
    {
      pGraphics->CloseWindow();
      return 0;
    }
    case WM_SETFOCUS:
    {
      return 0;
    }
    case WM_KILLFOCUS:
    {
      return 0;
    }
  }
  return DefWindowProc(hWnd, msg, wParam, lParam);
}

// static
LRESULT CALLBACK IGraphicsWin::ParamEditProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
  IGraphicsWin* pGraphics = (IGraphicsWin*) GetWindowLongPtr(GetParent(hWnd), GWLP_USERDATA);

  if (pGraphics && pGraphics->mParamEditWnd && pGraphics->mParamEditWnd == hWnd)
  {
    pGraphics->HideTooltip();

    switch (msg)
    {
      case WM_CHAR:
      {
        const IParam* pParam = pGraphics->mEdControl->GetParam();
        // limit to numbers for text entry on appropriate parameters
        if(pParam)
        {
          char c = wParam;

          if(c == 0x08) break; // backspace

          switch ( pParam->Type() )
          {
            case IParam::kTypeEnum:
            case IParam::kTypeInt:
            case IParam::kTypeBool:
              if (c >= '0' && c <= '9') break;
              else if (c == '-') break;
              else if (c == '+') break;
              else return 0;
            case IParam::kTypeDouble:
              if (c >= '0' && c <= '9') break;
              else if (c == '-') break;
              else if (c == '+') break;
              else if (c == '.') break;
              else return 0;
            default:
              break;
          }
        }
        break;
      }
      case WM_KEYDOWN:
      {
        if (wParam == VK_RETURN)
        {
          pGraphics->mParamEditMsg = kCommit;
          return 0;
        }
        else if (wParam == VK_ESCAPE)
        {
          pGraphics->mParamEditMsg = kCancel;
          return 0;
        }
        break;
      }
      case WM_SETFOCUS:
      {
        pGraphics->mParamEditMsg = kEditing;
        break;
      }
      case WM_KILLFOCUS:
      {
        pGraphics->mParamEditMsg = kCommit;
        break;
      }
      // handle WM_GETDLGCODE so that we can say that we want the return key message
      //  (normally single line edit boxes don't get sent return key messages)
      case WM_GETDLGCODE:
      {
        if (pGraphics->mEdControl->GetParam()) break;
        LPARAM lres;
        // find out if the original control wants it
        lres = CallWindowProc(pGraphics->mDefEditProc, hWnd, WM_GETDLGCODE, wParam, lParam);
        // add in that we want it if it is a return keydown
        if (lParam && ((MSG*)lParam)->message == WM_KEYDOWN  &&  wParam == VK_RETURN)
        {
          lres |= DLGC_WANTMESSAGE;
        }
        return lres;
      }
      case WM_COMMAND:
      {
        switch HIWORD(wParam)
        {
          case CBN_SELCHANGE:
          {
            if (pGraphics->mParamEditWnd)
            {
              pGraphics->mParamEditMsg = kCommit;
              return 0;
            }
          }

        }
        break;  // Else let the default proc handle it.
      }
    }
    return CallWindowProc(pGraphics->mDefEditProc, hWnd, msg, wParam, lParam);
  }
  return DefWindowProc(hWnd, msg, wParam, lParam);
}

IGraphicsWin::IGraphicsWin(IDelegate& dlg, int w, int h, int fps)
  : IGRAPHICS_DRAW_CLASS(dlg, w, h, fps)
{}

IGraphicsWin::~IGraphicsWin()
{
  CloseWindow();
  FREE_NULL(mCustomColorStorage);
}

void GetWindowSize(HWND pWnd, int* pW, int* pH)
{
  if (pWnd)
  {
    RECT r;
    GetWindowRect(pWnd, &r);
    *pW = r.right - r.left;
    *pH = r.bottom - r.top;
  }
  else
  {
    *pW = *pH = 0;
  }
}

bool IsChildWindow(HWND pWnd)
{
  if (pWnd)
  {
    int style = GetWindowLong(pWnd, GWL_STYLE);
    int exStyle = GetWindowLong(pWnd, GWL_EXSTYLE);
    return ((style & WS_CHILD) && !(exStyle & WS_EX_MDICHILD));
  }
  return false;
}

void IGraphicsWin::ForceEndUserEdit()
{
  mParamEditMsg = kCancel;
}

#define SETPOS_FLAGS SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE

void IGraphicsWin::Resize(int w, int h, float scale)
{
  if (w == Width() && h == Height() && scale == GetScale()) return;

  int oldWindowWidth = WindowWidth(), oldWindowHeight = WindowHeight();
  IGraphics::Resize(w, h, scale);

  int dw = WindowWidth() - oldWindowWidth, dh = WindowHeight() - oldWindowHeight;

  if (WindowIsOpen())
  {
    HWND pParent = 0, pGrandparent = 0;
    int dlgW = 0, dlgH = 0, parentW = 0, parentH = 0, grandparentW = 0, grandparentH = 0;
    GetWindowSize(mDelegateWnd, &dlgW, &dlgH);

    if (IsChildWindow(mDelegateWnd))
    {
      pParent = GetParent(mDelegateWnd);
      GetWindowSize(pParent, &parentW, &parentH);

      if (IsChildWindow(pParent))
      {
        pGrandparent = GetParent(pParent);
        GetWindowSize(pGrandparent, &grandparentW, &grandparentH);
      }
    }

    SetWindowPos(mDelegateWnd, 0, 0, 0, dlgW + dw, dlgH + dh, SETPOS_FLAGS);

    // don't want to touch the host window in VST3
#ifndef VST3_API
      if(pParent)
      {
        SetWindowPos(pParent, 0, 0, 0, parentW + dw, parentH + dh, SETPOS_FLAGS);
      }

      if(pGrandparent)
      {
        SetWindowPos(pGrandparent, 0, 0, 0, grandparentW + dw, grandparentH + dh, SETPOS_FLAGS);
      }
#endif

    RECT r = { 0, 0, WindowWidth(), WindowHeight() };
    InvalidateRect(mDelegateWnd, &r, FALSE);
  }
}

//void IGraphicsWin::HideMouseCursor(bool hide)
//{
  //if(hide)
  //{
  //  if (mCursorHidden)
  //  {
  //    SetCursorPos(mHiddenMousePointX, mHiddenMousePointY);
  //    ShowCursor(true);
  //    mCursorHidden = false;
  //  }
  //}
  //else
  //{
  //  if (!mCursorHidden)
  //  {
  //    POINT p;
  //    GetCursorPos(&p);
  //    
  //    mHiddenMousePointX = p.x;
  //    mHiddenMousePointY = p.y;
  //    
  //    ShowCursor(false);
  //    mCursorHidden = true;
  //  }
  //}
//}

int IGraphicsWin::ShowMessageBox(const char* text, const char* caption, int type)
{
  return MessageBox(GetMainWnd(), text, caption, type);
}

void* IGraphicsWin::OpenWindow(void* pParentWnd)
{
  int x = 0, y = 0, w = WindowWidth(), h = WindowHeight();
  mParentWnd = (HWND) pParentWnd;

  if (mDelegateWnd)
  {
    RECT pR, cR;
    GetWindowRect((HWND) pParentWnd, &pR);
    GetWindowRect(mDelegateWnd, &cR);
    CloseWindow();
    x = cR.left - pR.left;
    y = cR.top - pR.top;
    w = cR.right - cR.left;
    h = cR.bottom - cR.top;
  }

  if (nWndClassReg++ == 0)
  {
    WNDCLASS wndClass = { CS_DBLCLKS, WndProc, 0, 0, mHInstance, 0, LoadCursor(NULL, IDC_ARROW), 0, 0, wndClassName };
    RegisterClass(&wndClass);
  }

  sFPS = FPS();
  mDelegateWnd = CreateWindow(wndClassName, "IPlug", WS_CHILD | WS_VISIBLE, // | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                          x, y, w, h, (HWND) pParentWnd, 0, mHInstance, this);

  HDC dc = GetDC(mDelegateWnd);
  SetPlatformContext(dc);
  ReleaseDC(mDelegateWnd, dc);

  SetDisplayScale(1);

  if (!mDelegateWnd && --nWndClassReg == 0)
  {
    UnregisterClass(wndClassName, mHInstance);
  }
  else
  {
    SetAllControlsDirty();
  }

  if (mDelegateWnd && TooltipsEnabled())
  {
    bool ok = false;
    static const INITCOMMONCONTROLSEX iccex = { sizeof(INITCOMMONCONTROLSEX), ICC_TAB_CLASSES };

    if (InitCommonControlsEx(&iccex))
    {
      mTooltipWnd = CreateWindowEx(0, TOOLTIPS_CLASS, NULL, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                                   CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, mDelegateWnd, NULL, mHInstance, NULL);
      if (mTooltipWnd)
      {
        SetWindowPos(mTooltipWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        TOOLINFO ti = { TTTOOLINFOA_V2_SIZE, TTF_IDISHWND | TTF_SUBCLASS, mDelegateWnd, (UINT_PTR)mDelegateWnd };
        ti.lpszText = (LPTSTR)NULL;
        SendMessage(mTooltipWnd, TTM_ADDTOOL, 0, (LPARAM)&ti);
        ok = true;
      }
    }

    if (!ok) EnableTooltips(ok);
  }

  return mDelegateWnd;
}

void GetWndClassName(HWND hWnd, WDL_String* pStr)
{
  char cStr[MAX_CLASSNAME_LEN];
  cStr[0] = '\0';
  GetClassName(hWnd, cStr, MAX_CLASSNAME_LEN);
  pStr->Set(cStr);
}

BOOL CALLBACK IGraphicsWin::FindMainWindow(HWND hWnd, LPARAM lParam)
{
  IGraphicsWin* pGraphics = (IGraphicsWin*) lParam;
  if (pGraphics)
  {
    DWORD wPID;
    GetWindowThreadProcessId(hWnd, &wPID);
    WDL_String str;
    GetWndClassName(hWnd, &str);
    if (wPID == pGraphics->mPID && !strcmp(str.Get(), pGraphics->mMainWndClassName.Get()))
    {
      pGraphics->mMainWnd = hWnd;
      return FALSE;   // Stop enumerating.
    }
  }
  return TRUE;
}

HWND IGraphicsWin::GetMainWnd()
{
  if (!mMainWnd)
  {
    if (mParentWnd)
    {
      HWND parentWnd = mParentWnd;
      while (parentWnd)
      {
        mMainWnd = parentWnd;
        parentWnd = GetParent(mMainWnd);
      }
      GetWndClassName(mMainWnd, &mMainWndClassName);
    }
    else if (CStringHasContents(mMainWndClassName.Get()))
    {
      mPID = GetCurrentProcessId();
      EnumWindows(FindMainWindow, (LPARAM) this);
    }
  }
  return mMainWnd;
}

IRECT IGraphicsWin::GetWindowRECT()
{
  if (mDelegateWnd)
  {
    RECT r;
    GetWindowRect(mDelegateWnd, &r);
    r.right -= TOOLWIN_BORDER_W;
    r.bottom -= TOOLWIN_BORDER_H;
    return IRECT(r.left, r.top, r.right, r.bottom);
  }
  return IRECT();
}

void IGraphicsWin::SetWindowTitle(const char* str)
{
  SetWindowText(mDelegateWnd, str);
}

void IGraphicsWin::CloseWindow()
{
  if (mDelegateWnd)
  {
    SetPlatformContext(nullptr);

    if (mTooltipWnd)
    {
      DestroyWindow(mTooltipWnd);
      mTooltipWnd = 0;
      mShowingTooltip = false;
      mTooltipIdx = -1;
    }

    DestroyWindow(mDelegateWnd);
    mDelegateWnd = 0;

    if (--nWndClassReg == 0)
    {
      UnregisterClass(wndClassName, mHInstance);
    }
  }
}

IPopupMenu* IGraphicsWin::GetItemMenu(long idx, long& idxInMenu, long& offsetIdx, const IPopupMenu& baseMenu)
{
  long oldIDx = offsetIdx;
  offsetIdx += baseMenu.NItems();

  if (idx < offsetIdx)
  {
    idxInMenu = idx - oldIDx;
    return &const_cast<IPopupMenu&>(baseMenu);
  }

  IPopupMenu* pMenu = nullptr;

  for(int i = 0; i< baseMenu.NItems(); i++)
  {
    IPopupMenu::Item* pMenuItem = const_cast<IPopupMenu&>(baseMenu).GetItem(i);
    if(pMenuItem->GetSubmenu())
    {
      pMenu = GetItemMenu(idx, idxInMenu, offsetIdx, *pMenuItem->GetSubmenu());

      if(pMenu)
        break;
    }
  }

  return pMenu;
}

HMENU IGraphicsWin::CreateMenu(IPopupMenu& menu, long* pOffsetIdx)
{
  HMENU hMenu = ::CreatePopupMenu();

  WDL_String escapedText;

  int flags = 0;
  long offset = *pOffsetIdx;
  long nItems = menu.NItems();
  *pOffsetIdx += nItems;
  long inc = 0;

  for(int i = 0; i< nItems; i++)
  {
    IPopupMenu::Item* pMenuItem = const_cast<IPopupMenu&>(menu).GetItem(i);

    if (pMenuItem->GetIsSeparator())
    {
      AppendMenu(hMenu, MF_SEPARATOR, 0, 0);
    }
    else
    {
      const char* str = pMenuItem->GetText();
      char* titleWithPrefixNumbers = 0;

      if (menu.GetPrefix())
      {
        titleWithPrefixNumbers = (char*)malloc(strlen(str) + 50);

        switch (menu.GetPrefix())
        {
          case 1:
          {
            sprintf(titleWithPrefixNumbers, "%1d: %s", i+1, str); break;
          }
          case 2:
          {
            sprintf(titleWithPrefixNumbers, "%02d: %s", i+1, str); break;
          }
          case 3:
          {
            sprintf(titleWithPrefixNumbers, "%03d: %s", i+1, str); break;
          }
        }
      }

      const char* entryText(titleWithPrefixNumbers ? titleWithPrefixNumbers : str);

      // Escape ampersands if present

      if (strchr(entryText, '&'))
      {
        escapedText = WDL_String(entryText);

        for (int c = 0; c < escapedText.GetLength(); c++)
          if (escapedText.Get()[c] == '&')
            escapedText.Insert("&", c++);

         entryText = escapedText.Get();
      }

      flags = MF_STRING;
      //if (nItems < 160 && pMenu->getNbItemsPerColumn () > 0 && inc && !(inc % _menu->getNbItemsPerColumn ()))
      //  flags |= MF_MENUBARBREAK;

      if (pMenuItem->GetSubmenu())
      {
        HMENU submenu = CreateMenu(*pMenuItem->GetSubmenu(), pOffsetIdx);
        if (submenu)
        {
          AppendMenu(hMenu, flags|MF_POPUP|MF_ENABLED, (UINT_PTR)submenu, (const TCHAR*)entryText);
        }
      }
      else
      {
        if (pMenuItem->GetEnabled())
          flags |= MF_ENABLED;
        else
          flags |= MF_GRAYED;
        if (pMenuItem->GetIsTitle())
          flags |= MF_DISABLED;
        if (pMenuItem->GetChecked())
          flags |= MF_CHECKED;
        else
          flags |= MF_UNCHECKED;

        AppendMenu(hMenu, flags, offset + inc, entryText);
      }

      if(titleWithPrefixNumbers)
        FREE_NULL(titleWithPrefixNumbers);
    }
    inc++;
  }

  return hMenu;
}

IPopupMenu* IGraphicsWin::CreatePopupMenu(IPopupMenu& menu, const IRECT& bounds, IControl* pCaller)
{
  ReleaseMouseCapture();

  long offsetIdx = 0;
  HMENU hMenu = CreateMenu(menu, &offsetIdx);
  IPopupMenu* result = nullptr;

  if(hMenu)
  {
    POINT cPos;

    cPos.x = bounds.L;
    cPos.y = bounds.B;

    ClientToScreen(mDelegateWnd, &cPos);

    if (TrackPopupMenu(hMenu, TPM_LEFTALIGN, cPos.x, cPos.y, 0, mDelegateWnd, 0))
    {
      MSG msg;
      if (PeekMessage(&msg, mDelegateWnd, WM_COMMAND, WM_COMMAND, PM_REMOVE))
      {
        if (HIWORD(msg.wParam) == 0)
        {
          long res = LOWORD(msg.wParam);
          if (res != -1)
          {
            long idx = 0;
            offsetIdx = 0;
            IPopupMenu* resultMenu = GetItemMenu(res, idx, offsetIdx, menu);
            if(resultMenu)
            {
              result = resultMenu;
              result->SetChosenItemIdx(idx);
            }
          }
        }
      }
    }
    DestroyMenu(hMenu);

    RECT r = { 0, 0, WindowWidth(), WindowHeight() };
    InvalidateRect(mDelegateWnd, &r, FALSE);
  }
  
  if (pCaller)
    pCaller->OnPopupMenuSelection(result);
  
  return result;
}

void IGraphicsWin::CreateTextEntry(IControl& control, const IText& text, const IRECT& bounds, const char* str)
{
  if (mParamEditWnd)
    return;

  DWORD editStyle;

  switch ( text.mAlign )
  {
    case IText::kAlignNear:   editStyle = ES_LEFT;   break;
    case IText::kAlignFar:    editStyle = ES_RIGHT;  break;
    case IText::kAlignCenter:
    default:                  editStyle = ES_CENTER; break;
  }

  mParamEditWnd = CreateWindow("EDIT", str, ES_AUTOHSCROLL /*only works for left aligned text*/ | WS_CHILD | WS_VISIBLE | ES_MULTILINE | editStyle,
    bounds.L, bounds.T, bounds.W()+1, bounds.H()+1,
    mDelegateWnd, (HMENU) PARAM_EDIT_ID, mHInstance, 0);

  HFONT font = CreateFont(text.mSize, 0, 0, 0, text.mStyle == IText::kStyleBold ? FW_BOLD : 0, text.mStyle == IText::kStyleItalic ? TRUE : 0, 0, 0, 0, 0, 0, 0, 0, text.mFont);

  SendMessage(mParamEditWnd, EM_LIMITTEXT, (WPARAM) control.GetTextEntryLength(), 0);
  SendMessage(mParamEditWnd, WM_SETFONT, (WPARAM) font, 0);
  SendMessage(mParamEditWnd, EM_SETSEL, 0, -1);

  SetFocus(mParamEditWnd);

  mDefEditProc = (WNDPROC) SetWindowLongPtr(mParamEditWnd, GWLP_WNDPROC, (LONG_PTR) ParamEditProc);
  SetWindowLongPtr(mParamEditWnd, GWLP_USERDATA, 0xdeadf00b);

  //DeleteObject(font);

  mEdControl = &control;
}

bool IGraphicsWin::RevealPathInExplorerOrFinder(WDL_String& path, bool select)
{
  bool success = false;
  
  if (path.GetLength())
  {
    WCHAR winDir[IPLUG_WIN_MAX_WIDE_PATH];
  WCHAR explorerWide[IPLUG_WIN_MAX_WIDE_PATH];
    UINT len = GetSystemDirectoryW(winDir, IPLUG_WIN_MAX_WIDE_PATH);
    
    if (len || !(len > MAX_PATH - 2))
    {
      winDir[len]   = L'\\';
      winDir[++len] = L'\0';
      
      WDL_String explorerParams;
      
      if(select)
        explorerParams.Append("/select,");
      
      explorerParams.Append("\"");
      explorerParams.Append(path.Get());
      explorerParams.Append("\\\"");
      
    UTF8ToUTF16(explorerWide, explorerParams.Get(), IPLUG_WIN_MAX_WIDE_PATH);
      HINSTANCE result;
      
      if ((result=::ShellExecuteW(NULL, L"open", L"explorer.exe", explorerWide, winDir, SW_SHOWNORMAL)) <= (HINSTANCE) 32)
        success = true;
    }
  }
  
  return success;
}

//TODO: this method needs rewriting
void IGraphicsWin::PromptForFile(WDL_String& filename, WDL_String& path, EFileAction action, const char* extensions)
{
  if (!WindowIsOpen())
  {
    filename.Set("");
    return;
  }
    
  wchar_t fnCStr[_MAX_PATH];
  wchar_t dirCStr[_MAX_PATH];
    
  if (filename.GetLength())
    UTF8ToUTF16(fnCStr, filename.Get(), _MAX_PATH);
  else
    fnCStr[0] = '\0';
    
  dirCStr[0] = '\0';
    
  //if (!path.GetLength())
  //  DesktopPath(path);
    
  UTF8ToUTF16(dirCStr, path.Get(), _MAX_PATH);
    
  OPENFILENAMEW ofn;
  memset(&ofn, 0, sizeof(OPENFILENAMEW));
    
  ofn.lStructSize = sizeof(OPENFILENAMEW);
  ofn.hwndOwner = (HWND) GetWindow();
  ofn.lpstrFile = fnCStr;
  ofn.nMaxFile = _MAX_PATH - 1;
  ofn.lpstrInitialDir = dirCStr;
  ofn.Flags = OFN_PATHMUSTEXIST;
    
  if (CStringHasContents(extensions))
  {
    wchar_t extStr[256];
    wchar_t defExtStr[16];
    int i, p, n = strlen(extensions);
    bool seperator = true;
        
    for (i = 0, p = 0; i < n; ++i)
    {
      if (seperator)
      {
        if (p)
          extStr[p++] = ';';
                
        seperator = false;
        extStr[p++] = '*';
        extStr[p++] = '.';
      }

      if (extensions[i] == ' ')
        seperator = true;
      else
        extStr[p++] = extensions[i];
    }
    extStr[p++] = '\0';
        
    wcscpy(&extStr[p], extStr);
    extStr[p + p] = '\0';
    ofn.lpstrFilter = extStr;
        
    for (i = 0, p = 0; i < n && extensions[i] != ' '; ++i)
      defExtStr[p++] = extensions[i];
    
    defExtStr[p++] = '\0';
    ofn.lpstrDefExt = defExtStr;
  }
    
  bool rc = false;
    
  switch (action)
  {
    case kFileSave:
      ofn.Flags |= OFN_OVERWRITEPROMPT;
      rc = GetSaveFileNameW(&ofn);
      break;
            
    case kFileOpen:
      default:
      ofn.Flags |= OFN_FILEMUSTEXIST;
      rc = GetOpenFileNameW(&ofn);
      break;
  }
    
  if (rc)
  {
    char drive[_MAX_DRIVE];
    char directoryOutCStr[_MAX_PATH];
    
    WDL_String tempUTF8;
    UTF16ToUTF8(tempUTF8, ofn.lpstrFile);
    
    if (_splitpath_s(tempUTF8.Get(), drive, sizeof(drive), directoryOutCStr, sizeof(directoryOutCStr), NULL, 0, NULL, 0) == 0)
    {
      path.Set(drive);
      path.Append(directoryOutCStr);
    }
      
    filename.Set(tempUTF8.Get());
  }
  else
  {
    filename.Set("");
  }
}

void IGraphicsWin::PromptForDirectory(WDL_String& dir)
{
  BROWSEINFO bi;
  memset(&bi, 0, sizeof(bi));
  
  bi.ulFlags   = BIF_USENEWUI;
  bi.hwndOwner = mDelegateWnd;
  bi.lpszTitle = "Choose a Directory";
  
  // must call this if using BIF_USENEWUI
  ::OleInitialize(NULL);
  LPITEMIDLIST pIDL = ::SHBrowseForFolder(&bi);
  
  if(pIDL != NULL)
  {
    char buffer[_MAX_PATH] = {'\0'};
    
    if(::SHGetPathFromIDList(pIDL, buffer) != 0)
    {
      dir.Set(buffer);
      dir.Append("\\");
    }
    
    // free the item id list
    CoTaskMemFree(pIDL);
  }
  else
  {
    dir.Set("");
  }
  
  ::OleUninitialize();
}

UINT_PTR CALLBACK CCHookProc(HWND hdlg, UINT uiMsg, WPARAM wParam, LPARAM lParam)
{
  if (uiMsg == WM_INITDIALOG && lParam)
  {
    CHOOSECOLOR* cc = (CHOOSECOLOR*) lParam;
    if (cc && cc->lCustData)
    {
      char* str = (char*) cc->lCustData;
      SetWindowText(hdlg, str);
    }
  }
  return 0;
}

bool IGraphicsWin::PromptForColor(IColor& color, const char* prompt)
{
  if (!mDelegateWnd)
  {
    return false;
  }
  if (!mCustomColorStorage)
  {
    mCustomColorStorage = (COLORREF*) calloc(16, sizeof(COLORREF));
  }
  CHOOSECOLOR cc;
  memset(&cc, 0, sizeof(CHOOSECOLOR));
  cc.lStructSize = sizeof(CHOOSECOLOR);
  cc.hwndOwner = mDelegateWnd;
  cc.rgbResult = RGB(color.R, color.G, color.B);
  cc.lpCustColors = mCustomColorStorage;
  cc.lCustData = (LPARAM) prompt;
  cc.lpfnHook = CCHookProc;
  cc.Flags = CC_RGBINIT | CC_ANYCOLOR | CC_FULLOPEN | CC_SOLIDCOLOR | CC_ENABLEHOOK;

  if (ChooseColor(&cc))
  {
    color.R = GetRValue(cc.rgbResult);
    color.G = GetGValue(cc.rgbResult);
    color.B = GetBValue(cc.rgbResult);
    return true;
  }
  return false;
}

bool IGraphicsWin::OpenURL(const char* url, const char* msgWindowTitle, const char* confirmMsg, const char* errMsgOnFailure)
{
  if (confirmMsg && MessageBox(mDelegateWnd, confirmMsg, msgWindowTitle, MB_YESNO) != IDYES)
  {
    return false;
  }
  DWORD inetStatus = 0;
  if (InternetGetConnectedState(&inetStatus, 0))
  {
    WCHAR urlWide[IPLUG_WIN_MAX_WIDE_PATH];
    UTF8ToUTF16(urlWide, url, IPLUG_WIN_MAX_WIDE_PATH);
    if ((int) ShellExecuteW(mDelegateWnd, L"open", urlWide, 0, 0, SW_SHOWNORMAL) > MAX_INET_ERR_CODE)
    {
      return true;
    }
  }
  if (errMsgOnFailure)
  {
    MessageBox(mDelegateWnd, errMsgOnFailure, msgWindowTitle, MB_OK);
  }
  return false;
}

void IGraphicsWin::SetTooltip(const char* tooltip)
{
  TOOLINFO ti = { TTTOOLINFOA_V2_SIZE, 0, mDelegateWnd, (UINT_PTR)mDelegateWnd };
  ti.lpszText = (LPTSTR)tooltip;
  SendMessage(mTooltipWnd, TTM_UPDATETIPTEXT, 0, (LPARAM)&ti);
}

void IGraphicsWin::ShowTooltip()
{
  const char* tooltip = GetControl(mTooltipIdx)->GetTooltip();
  if (tooltip)
  {
    assert(strlen(tooltip) < 80);
    SetTooltip(tooltip);
    mShowingTooltip = true;
  }
}

void IGraphicsWin::HideTooltip()
{
  if (mShowingTooltip)
  {
    SetTooltip(NULL);
    mShowingTooltip = false;
  }
}

bool IGraphicsWin::GetTextFromClipboard(WDL_String& str)
{
  bool success = false;
  HGLOBAL hglb;
  
  if (IsClipboardFormatAvailable(CF_UNICODETEXT))
  {
    if(OpenClipboard(0))
    {
      hglb = GetClipboardData(CF_UNICODETEXT);
      
      if(hglb != NULL)
      {
        WCHAR *orig_str = (WCHAR*)GlobalLock(hglb);
        
        if (orig_str != NULL)
        {
          int orig_len = (int) wcslen(orig_str);
          
          orig_len += 1;
          
          // find out how much space is needed
          int new_len = WideCharToMultiByte(CP_UTF8,
                                            0,
                                            orig_str,
                                            orig_len,
                                            0,
                                            0,
                                            NULL,
                                            NULL);
          
          if (new_len > 0)
          {
            char *new_str = new char[new_len + 1];
            
            int num_chars = WideCharToMultiByte(CP_UTF8,
                                                0,
                                                orig_str,
                                                orig_len,
                                                new_str,
                                                new_len,
                                                NULL,
                                                NULL);
            
            if (num_chars > 0)
            {
              success = true;
              str.Set(new_str);
            }
            
            delete [] new_str;
          }
          
          GlobalUnlock(hglb);
        }
      }
    }
    
    CloseClipboard();
  }
  
  if(!success)
    str.Set("");
  
  return success;
}

BOOL IGraphicsWin::EnumResNameProc(HANDLE module, LPCTSTR type, LPTSTR name, LONG param)
{
  if (IS_INTRESOURCE(name)) return true; // integer resources not wanted
  else {
    WDL_String* search = (WDL_String*) param;
    if (search != 0 && name != 0)
    {
      //strip off extra quotes
      WDL_String strippedName(strlwr(name+1)); 
      strippedName.SetLen(strippedName.GetLength() - 1);

      if (strcmp(strlwr(search->Get()), strippedName.Get()) == 0) // if we are looking for a resource with this name
      {
        search->SetFormatted(strippedName.GetLength() + 7, "found: %s", strippedName.Get());
        return false;
      }
    }
  }

  return true; // keep enumerating
}

bool IGraphicsWin::OSFindResource(const char* name, const char* type, WDL_String& result)
{
  if (CStringHasContents(name))
  {
    WDL_String search(name);
    WDL_String typeUpper(type);

    EnumResourceNames(mHInstance, _strupr(typeUpper.Get()), (ENUMRESNAMEPROC)EnumResNameProc, (LONG_PTR)&search);

    if (strstr(search.Get(), "found: ") != 0)
    {
      result.SetFormatted(MAX_PATH, "\"%s\"", search.Get() + 7, search.GetLength() - 7); // 7 = strlen("found: ")
      return true;
    }
    else
    {
      if (PathFileExists(name))
      {
        result.Set(name);
        return true;
      }
    }
  }
  return false;
}

//TODO: THIS IS TEMPORARY, TO EASE DEVELOPMENT
#ifndef NO_IGRAPHICS
#if defined IGRAPHICS_AGG
  #include "IGraphicsAGG.cpp"
  #include "agg_win_pmap.cpp"
  #include "agg_win_font.cpp"
#elif defined IGRAPHICS_CAIRO
  #include "IGraphicsCairo.cpp"
#elif defined IGRAPHICS_LICE
  #include "IGraphicsLice.cpp"
#elif defined IGRAPHICS_NANOVG
  #include "IGraphicsNanoVG.cpp"
  #include "nanovg.c"
//#include "nanovg_mtl.m"
#else
  #include "IGraphicsCairo.cpp"
#endif
#endif