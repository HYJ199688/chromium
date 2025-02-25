// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.browserservices;

import android.app.PendingIntent;
import android.content.Intent;
import android.graphics.Bitmap;
import android.support.annotation.Nullable;
import android.support.customtabs.CustomTabsSessionToken;
import android.widget.RemoteViews;

import org.chromium.content_public.browser.LoadUrlParams;

/**
 * Interface to handle browser services calls whenever the session id matched.
 * TODO(yusufo): Add a way to handle mayLaunchUrl as well.
 */
public interface BrowserSessionContentHandler {
    /**
     * Loads a new url inside the {@link BrowserSessionContentHandler}, and tracks
     * its load time.
     *
     * @param params The params to use while loading the url.
     * @param timestamp The intent arrival timestamp, as returned by
     *                  {@link SystemClock#elapsedRealtime()}.
     */
    void loadUrlAndTrackFromTimestamp(LoadUrlParams params, long timestamp);

    /**
     * @return The session this {@link BrowserSessionContentHandler} is associated with.
     */
    CustomTabsSessionToken getSession();

    /**
     * Check whether an intent is valid or should be ignored within this content handler.
     * @param intent The intent to check.
     * @return Whether the intent should be ignored.
     */
    boolean shouldIgnoreIntent(Intent intent);

    /**
     * Finds the action button with the given id, and updates it with the new content.
     * @return Whether the action button has been updated.
     */
    boolean updateCustomButton(int id, Bitmap bitmap, String description);

    /**
     * Updates the {@link RemoteViews} shown on the secondary toolbar.
     * @return Whether this update is successful.
     */
    boolean updateRemoteViews(
            RemoteViews remoteViews, int[] clickableIDs, PendingIntent pendingIntent);

    /**
     * @return The current url being displayed to the user.
     */
    @Nullable String getCurrentUrl();

    /**
     * @return The url of a pending navigation, if any.
     */
    @Nullable String getPendingUrl();

    /**
     * @return the task id the content handler is running in.
     */
    int getTaskId();
}
