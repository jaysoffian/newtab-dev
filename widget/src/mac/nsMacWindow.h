/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 *
 * The contents of this file are subject to the Netscape Public
 * License Version 1.1 (the "License"); you may not use this file
 * except in compliance with the License. You may obtain a copy of
 * the License at http://www.mozilla.org/NPL/
 *
 * Software distributed under the License is distributed on an "AS
 * IS" basis, WITHOUT WARRANTY OF ANY KIND, either express or
 * implied. See the License for the specific language governing
 * rights and limitations under the License.
 *
 * The Original Code is mozilla.org code.
 *
 * The Initial Developer of the Original Code is Netscape
 * Communications Corporation.  Portions created by Netscape are
 * Copyright (C) 1998 Netscape Communications Corporation. All
 * Rights Reserved.
 *
 * Contributor(s): 
 */
#ifndef MacWindow_h__
#define MacWindow_h__

#include <memory>	// for auto_ptr
#include <Controls.h>

using std::auto_ptr;

#include "nsWindow.h"
#include "nsMacEventHandler.h"

class nsMacEventHandler;
struct PhantomScrollbarData;

//-------------------------------------------------------------------------
//
// nsMacWindow
//
//-------------------------------------------------------------------------
//	MacOS native window

class nsMacWindow : public nsChildWindow
{
private:
	typedef nsChildWindow Inherited;

public:
    nsMacWindow();
    virtual ~nsMacWindow();

/*
    // nsIWidget interface
    NS_IMETHOD            Create(nsIWidget *aParent,
                                     const nsRect &aRect,
                                     EVENT_CALLBACK aHandleEventFunction,
                                     nsIDeviceContext *aContext,
                                     nsIAppShell *aAppShell = nsnull,
                                     nsIToolkit *aToolkit = nsnull,
                                     nsWidgetInitData *aInitData = nsnull);
*/
    NS_IMETHOD              Create(nsNativeWidget aParent,
                                     const nsRect &aRect,
                                     EVENT_CALLBACK aHandleEventFunction,
                                     nsIDeviceContext *aContext,
                                     nsIAppShell *aAppShell = nsnull,
                                     nsIToolkit *aToolkit = nsnull,
                                     nsWidgetInitData *aInitData = nsnull);

     // Utility method for implementing both Create(nsIWidget ...) and
     // Create(nsNativeWidget...)

    virtual nsresult        StandardCreate(nsIWidget *aParent,
				                            const nsRect &aRect,
				                            EVENT_CALLBACK aHandleEventFunction,
				                            nsIDeviceContext *aContext,
				                            nsIAppShell *aAppShell,
				                            nsIToolkit *aToolkit,
				                            nsWidgetInitData *aInitData,
				                            nsNativeWidget aNativeParent = nsnull);

    NS_IMETHOD              Show(PRBool aState);
    NS_IMETHOD              ConstrainPosition(PRInt32 *aX, PRInt32 *aY);
    NS_IMETHOD              Move(PRInt32 aX, PRInt32 aY);
    NS_IMETHOD              PlaceBehind(nsIWidget *aWidget, PRBool aActivate);
    NS_IMETHOD              SetSizeMode(PRInt32 aMode);

    NS_IMETHOD              Resize(PRInt32 aWidth,PRInt32 aHeight, PRBool aRepaint);
    NS_IMETHOD            	GetScreenBounds(nsRect &aRect);
    virtual PRBool          OnPaint(nsPaintEvent &event);

		NS_IMETHOD              SetTitle(const nsString& aTitle);

		virtual PRBool					HandleOSEvent(
																		EventRecord&		aOSEvent);

		virtual PRBool					HandleMenuCommand(
																		EventRecord&		aOSEvent,
																		long						aMenuResult);

		// be notified that a some form of drag event needs to go into Gecko
	virtual PRBool 			DragEvent ( unsigned int aMessage, Point aMouseGlobal, UInt16 aKeyModifiers ) ;

    void                    ComeToFront();

  	// nsIKBStateControl interface
  	NS_IMETHOD ResetInputState();

    void              		MoveToGlobalPoint(PRInt32 aX, PRInt32 aY);

    void IsActive(PRBool* aActive);
    void SetIsActive(PRBool aActive);
protected:

  void InstallBorderlessDefProc ( WindowPtr inWindow ) ;
  void RemoveBorderlessDefProc ( WindowPtr inWindow ) ;

	pascal static OSErr DragTrackingHandler ( DragTrackingMessage theMessage, WindowPtr theWindow, 
										void *handlerRefCon, DragReference theDrag );
	pascal static OSErr DragReceiveHandler (WindowPtr theWindow,
												void *handlerRefCon, DragReference theDragRef) ;
	DragTrackingHandlerUPP mDragTrackingHandlerUPP;
	DragReceiveHandlerUPP mDragReceiveHandlerUPP;

#if TARGET_CARBON
  pascal static OSStatus WindowEventHandler ( EventHandlerCallRef inHandlerChain, 
                                               EventRef inEvent, void* userData ) ;
#endif

	PRBool							mWindowMadeHere; // true if we created the window
	PRBool							mIsDialog;       // true if the window is a dialog
	auto_ptr<nsMacEventHandler>		mMacEventHandler;
	nsIWidget                      *mOffsetParent;
	PRBool                          mAcceptsActivation;
	PRBool                          mIsActive;
	PRBool                          mZoomOnShow;
	PRBool                          mResizeIsFromUs;    // we originated the resize, prevent infinite recursion
	
	ControlHandle      mPhantomScrollbar;  // a native scrollbar for the scrollwheel
	PhantomScrollbarData* mPhantomScrollbarData;
};

#endif // MacWindow_h__
