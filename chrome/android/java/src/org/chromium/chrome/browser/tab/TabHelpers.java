// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.tab;

import org.chromium.chrome.browser.ChromeActionModeCallback;
import org.chromium.chrome.browser.SwipeRefreshHandler;
import org.chromium.chrome.browser.contextualsearch.ContextualSearchTabHelper;
import org.chromium.chrome.browser.crypto.CipherFactory;
import org.chromium.chrome.browser.infobar.InfoBarContainer;
import org.chromium.chrome.browser.media.ui.MediaSessionTabHelper;
import org.chromium.chrome.browser.tab.TabUma.TabCreationState;
import org.chromium.components.content_capture.ContentCaptureFeatures;
import org.chromium.components.content_capture.ContentCaptureReceiverManager;
import org.chromium.content_public.browser.ImeAdapter;
import org.chromium.content_public.browser.ImeEventObserver;
import org.chromium.content_public.browser.SelectionPopupController;
import org.chromium.content_public.browser.WebContents;
import org.chromium.content_public.browser.WebContentsAccessibility;

/**
 * Helper class that initializes various tab UserData objects.
 */
public final class TabHelpers {
    private TabHelpers() {}

    /**
     * Creates Tab helper objects upon Tab creation.
     * @param tab {@link Tab} to create helpers for.
     * @param creationState State in which the tab is created.
     */
    static void initTabHelpers(Tab tab, @TabCreationState Integer creationState) {
        if (creationState != null) TabUma.create(tab, creationState);
        TabThemeColorHelper.createForTab(tab);
        TabFullscreenHandler.createForTab(tab);
        InterceptNavigationDelegateImpl.createForTab(tab);
        ContextualSearchTabHelper.createForTab(tab);
        MediaSessionTabHelper.createForTab(tab);

        // TODO(jinsukkim): Do this by having something observe new tab creation.
        if (tab.isIncognito()) CipherFactory.getInstance().triggerKeyGeneration();
    }

    /**
     * Initializes {@link TabWebContentsUserData} and WebContents-related objects
     * when a new WebContents is set to the tab.
     * @param tab {@link Tab} to create helpers for.
     */
    static void initWebContentsHelpers(Tab tab) {
        // The InfoBarContainer needs to be created after the ContentView has been natively
        // initialized. In the case where restoring a Tab or showing a prerendered one we already
        // have a valid infobar container, no need to recreate one.
        InfoBarContainer.from(tab);

        TabWebContentsObserver.from(tab);
        TabGestureStateListener.from(tab, tab::getFullscreenManager);
        SwipeRefreshHandler.from(tab);
        TabFavicon.from(tab);
        TrustedCdn.from(tab);

        WebContents webContents = tab.getWebContents();

        // Initializes WebContents objects.

        // TODO(jinsukkim): Split this up into a new userdata and FullscreenHandler.
        ImeAdapter.fromWebContents(webContents).addEventObserver(new ImeEventObserver() {
            @Override
            public void onImeEvent() {
                // Some text was set in the page. Don't reuse it if a tab is open from the same
                // external application, we might lose some user data.
                tab.setAppAssociatedWith(null);
            }

            @Override
            public void onNodeAttributeUpdated(boolean editable, boolean password) {
                if (tab.getFullscreenManager() == null) return;
                tab.updateFullscreenEnabledState();
            }
        });
        SelectionPopupController.fromWebContents(webContents)
                .setActionModeCallback(new ChromeActionModeCallback(tab, webContents));

        // For browser tabs, we want to set accessibility focus to the page when it loads. This
        // is not the default behavior for embedded web views.
        WebContentsAccessibility.fromWebContents(webContents).setShouldFocusOnPageLoad(true);

        // The created object is held by native side.
        if (ContentCaptureFeatures.isEnabled()) ContentCaptureReceiverManager.create(webContents);
    }
}
