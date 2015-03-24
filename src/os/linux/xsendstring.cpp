/*
* Copyright (c) 2003-2015 Rony Shapiro <ronys@users.sourceforge.net>.
* All rights reserved. Use of the code is allowed under the
* Artistic License 2.0 terms, as specified in the LICENSE file
* distributed with this code, or available from
* http://www.opensource.org/licenses/artistic-license-2.0.php
*/

/*
 * xsendstring - send a bunch of keystrokes to the app having current input focus
 *
 * Calls X library functions defined in Xt and Xtst
 *
 * To-Do list:
 * +. Initialize all the params of XKeyEvent
 * +  __STD_ISO_10646__ check
 * +  Remap an unused keycode to a keysym of XKeysymToKeycode fails
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <ctype.h>
#include <string.h>
#include <time.h>
#include <vector>
#include <errno.h>
#include <limits.h>

#include <memory>
#include <algorithm>
#include <functional>

#include <X11/Intrinsic.h> // in libxt-dev
#include <X11/keysym.h>
#include <X11/extensions/XTest.h> // in libxtst-dev

#include "./xsendstring.h"
#include "../sleep.h"
#include "../../core/PwsPlatform.h" // for NumberOf()
#include "../../core/StringX.h"
#include "./unicode2keysym.h"

namespace { // anonymous namespace for hiding
  //           local variables and functions
typedef struct _KeyPress {
  KeyCode code;
  unsigned int state;
} KeyPressInfo;

struct AutotypeGlobals
{
  Boolean      error_detected;
  char      errorString[1024];
} atGlobals  = { False, {0} };

class autotype_exception: public std::exception
{
  public:
  virtual const char* what() const throw() {
    return atGlobals.errorString;
  }
};

/*
 * ErrorHandler will be called when X detects an error. This function
 * just sets a global flag and saves the error message text
 */
int ErrorHandler(Display *my_dpy, XErrorEvent *event)
{
  char xmsg[512] = {0};

  atGlobals.error_detected = TRUE;
  XGetErrorText(my_dpy, event->error_code, xmsg, NumberOf(xmsg) - 1);
  snprintf(atGlobals.errorString, NumberOf(atGlobals.errorString)-1, "X error (%d): %s", event->request_code, xmsg);
  return 0;
}




void XTest_SendEvent(XKeyEvent *event)
{
  XTestFakeKeyEvent(event->display, event->keycode, event->type == KeyPress, 0);
}

void XSendKeys_SendEvent(XKeyEvent *event)
{
    XSendEvent(event->display, event->window, TRUE, KeyPressMask, reinterpret_cast<XEvent *>(event));
}

void XSendKeys_SendKeyEvent(XKeyEvent* event)
{
  event->type = KeyPress;
  XSendKeys_SendEvent(event);

  event->type = KeyRelease;
  XSendKeys_SendEvent(event);

  XFlush(event->display);
}


void XTest_SendKeyEvent(XKeyEvent* event)
{
  XKeyEvent shiftEvent;

  /* must simulate the shift-press for CAPS and shifted keypresses manually */
  if (event->state & ShiftMask) {
    memcpy(&shiftEvent, event, sizeof(shiftEvent));

    shiftEvent.keycode = XKeysymToKeycode(event->display, XK_Shift_L);
    shiftEvent.type = KeyPress;

    XTest_SendEvent(&shiftEvent);
  }

  event->type = KeyPress;
  XTest_SendEvent(event);

  event->type = KeyRelease;
  XTest_SendEvent(event);

  if (event->state & ShiftMask) {
    shiftEvent.type = KeyRelease;
    XTest_SendEvent(&shiftEvent);
  }

  XFlush(event->display);

}

Bool UseXTest(Display* disp)
{
  int major_opcode, first_event, first_error;
  static Bool useXTest;
  static int checked = 0;

  if (!checked) {
    useXTest = XQueryExtension(disp, "XTEST", &major_opcode, &first_event, &first_error);
    checked = 1;
  }
  return useXTest;
}

class AutotypeEvent: public XKeyEvent {
public:
  AutotypeEvent()
  {
    display = XOpenDisplay(NULL);
    if (display) {
      int    revert_to;
      XGetInputFocus(display, &window, &revert_to);
      subwindow = None;
      x = y = x_root = y_root = 1;
      same_screen = True;
    }
  }

  ~AutotypeEvent() {
    if (display)
      XCloseDisplay(display);
  }

  bool operator !() const { return display == NULL; }
};


int FindModifierMask(Display* disp, KeySym sym)
{
  int modmask = 0;
  XModifierKeymap* modmap = XGetModifierMapping(disp);
  if (modmap) {
    const int last = 8*modmap->max_keypermod;
    //begin at 4th row, where Mod1 starts
    for (int i = Mod1MapIndex*modmap->max_keypermod; i < last && !modmask; i++) {
      //
      const KeyCode kc = modmap->modifiermap[i];
      if (!kc)
        continue;
      int keysyms_per_keycode = 0;
      // For each keycode attached to this modifier, get a list of all keysyms
      // attached with this keycode. If any of those keysyms is what we are looking
      // for, then this is the modifier to use
      KeySym* symlist = XGetKeyboardMapping(disp, kc, 1, &keysyms_per_keycode);
      if ( symlist) {
        for (int j = 0; j < keysyms_per_keycode; j++) {
          if (sym == symlist[j]) {
            modmask = (i / modmap->max_keypermod);
            break;
          }
        }
        XFree(symlist);
      }
    }
    XFreeModifiermap(modmap);
  }
  return modmask;
}

int CalcModifiersForKeysym(KeyCode code, KeySym sym, Display* disp)
{
  int modifiers = 0;
  int keysyms_per_keycode = 0;
  KeySym* symlist = XGetKeyboardMapping(disp, code, 1, &keysyms_per_keycode);
  if (symlist != NULL && keysyms_per_keycode > 0) {
    // Supported everywhere.  Note: order is important
    std::vector<int> masks = {0, ShiftMask};
    // These aren't necessarily supported in all systems.  Once again, order is important
    for ( const auto s: {XK_Mode_switch, XK_ISO_Level3_Shift}) {
      const int modshift = FindModifierMask(disp, s);
      // May repeat.  Only consider it if we haven't added it already
      if (modshift && std::find(masks.begin(), masks.end(), 1 << modshift) == masks.end()) {
        std::vector<int> extra_masks;
        // OR each element of mask with "1 << modshift" & insert the result in mask
        std::transform(masks.begin(), masks.end(), std::back_inserter(extra_masks),
                        std::bind(std::bit_or<int>(), std::placeholders::_1, 1 << modshift));
        masks.insert(masks.end(), extra_masks.begin(), extra_masks.end());
      }
    }
    // Get the index of the symbol we are searching for
    const auto max_keysym_index = std::min(masks.size(), static_cast<size_t>(keysyms_per_keycode));
    // return the modifiers at the same index
    const size_t match_index = std::find(symlist, symlist + max_keysym_index, sym) - symlist;
    if ( match_index != max_keysym_index)
        modifiers = masks[match_index];
    XFree(symlist);
  }
  return modifiers;
}

KeySym wchar2keysym(wchar_t wc)
{
  if (wc < 0x100) {
    if (wc >= 0x20)
      return wc;
    switch(wc) {
      case L'\t': return XK_Tab;
      case L'\r': return XK_Return;
      case L'\n': return XK_Linefeed;
      case '\010': return XK_BackSpace;
      case '\177': return XK_Delete;
      case '\033': return XK_Escape;
      default:
        return NoSymbol;
    }
  }
  if (wc > 0x10ffff || (wc > 0x7e && wc < 0xa0))
    return NoSymbol;
  KeySym sym = unicode2keysym(wc);
  if (sym != NoSymbol)
    return sym;
  //For everything else, there's Mastercard :)
  return wc | 0x01000000;
}

//converts a  single wchar_t to a byte string [i.e. char*]
class wchar2bytes
{
private:
  //MB_CUR_MAX is a function call, not a constant
  char* bytes;
public:
  wchar2bytes(wchar_t wc):  bytes(new char[MB_CUR_MAX*2 + sizeof(wchar_t)*2 + 2 + 1]) {
    mbstate_t ps;
    memset(&ps, 0, sizeof(ps));//initialize mbstate
    size_t n;
    if ((n = wcrtomb(bytes, wc, &ps)) == size_t(-1))
      snprintf(bytes, NumberOf(bytes), "U+%04X", int(wc));
    else
      bytes[n] = 0;
  }
  ~wchar2bytes() { delete [] bytes; }
  const char* str() const {return bytes;}
};

/*
 * DoSendString - actually sends a string to the X Window having input focus
 *
 * The main task of this function is to convert the ascii char values
 * into X KeyCodes.  But they need to be converted to X KeySyms first
 * and then to the keycodes.  The KeyCodes can have any random values
 * and are not contiguous like the ascii values are.
 *
 * Some escape sequences can be converted to the appropriate KeyCodes
 * by this function.  See the code below for details
 */
void DoSendString(const StringX& str, pws_os::AutotypeMethod method, unsigned delayMS)
{
  atGlobals.error_detected = false;
  atGlobals.errorString[0] = 0;

  AutotypeEvent event;
  if (!event) {
    if (!atGlobals.error_detected)
      atGlobals.error_detected = true;
    if (!atGlobals.errorString[0])
      strncpy(atGlobals.errorString, "Could not open X display for autotyping", NumberOf(atGlobals.errorString));
    throw autotype_exception();
  }

  // convert all the chars into keycodes and required shift states first
  // Abort if any of the characters cannot be converted
  typedef std::vector<KeyPressInfo> KeyPressInfoVector;
  KeyPressInfoVector keypresses;

  for (StringX::const_iterator srcIter = str.begin(); srcIter != str.end(); ++srcIter) {

    //throw away 'vertical tab' chars which are only used on Windows to send a shift+tab
    //as a workaround for some issues with IE
    if (*srcIter == _T('\v'))
      continue;

    //Try a regular conversion first
    KeySym sym = wchar2keysym(*srcIter);

    if (NoSymbol != sym) {
      KeyPressInfo keypress = {0, 0};
      if ((keypress.code = XKeysymToKeycode(event.display, sym)) != 0) {
        //non-zero return value implies sym -> code was successful
        keypress.state |= CalcModifiersForKeysym(keypress.code, sym, event.display);
        keypresses.push_back(keypress);
      }
      else {
        const char* symStr = XKeysymToString(sym);
        snprintf(atGlobals.errorString, NumberOf(atGlobals.errorString),
              "Could not get keycode for key char(%s) - sym(%#X) - str(%s). Aborting autotype.\n\nIf \'xmodmap -pk\' does not list this KeySym, you probably need to install an appropriate keyboard layout.",
                          wchar2bytes(*srcIter).str(), static_cast<int>(sym), symStr ? symStr : "NULL");
        atGlobals.error_detected = True;
        return;
      }
    }
    else {
      snprintf(atGlobals.errorString, NumberOf(atGlobals.errorString),
              "Cannot convert '%s' [U+%04X] to keysym. Aborting autotype", wchar2bytes(*srcIter).str(), int(*srcIter));
      atGlobals.error_detected = True;
      return;
    }
  }

  XSetErrorHandler(ErrorHandler);
  atGlobals.error_detected = False;

  bool useXTEST = (UseXTest(event.display) && method != pws_os::ATMETHOD_XSENDKEYS);
  void (*KeySendFunction)(XKeyEvent*);

  if ( useXTEST) {
    KeySendFunction = XTest_SendKeyEvent;
    XTestGrabControl(event.display, True);
  }
  else {
    KeySendFunction = XSendKeys_SendKeyEvent;
  }

  for (KeyPressInfoVector::const_iterator itr = keypresses.begin(); itr != keypresses.end()
                              && !atGlobals.error_detected; ++itr) {
    event.keycode = itr->code;
    event.state = itr->state;
    event.time = CurrentTime;

    KeySendFunction(&event);
    pws_os::sleep_ms(delayMS);
  }

  if (useXTEST) {
    XTestGrabControl(event.display, False);
  }
  else {
    XSync(event.display, False);
  }

  XSetErrorHandler(NULL);
}

} // anonymous namespace

/*
 * SendString - The interface method for CKeySend
 *
 * The actual work is done by DoSendString above. This function just
 * just throws an exception if DoSendString encounters an error.
 *
 */
void pws_os::SendString(const StringX& str, AutotypeMethod method, unsigned delayMS)
{
  atGlobals.error_detected = false;
  atGlobals.errorString[0] = 0;

  DoSendString(str, method, delayMS);

  if (atGlobals.error_detected)
    throw autotype_exception();
}
