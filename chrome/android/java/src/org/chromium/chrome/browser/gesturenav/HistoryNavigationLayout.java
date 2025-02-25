// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.gesturenav;

import android.content.Context;
import android.support.annotation.IntDef;
import android.util.AttributeSet;
import android.view.GestureDetector;
import android.view.MotionEvent;
import android.widget.FrameLayout;

import org.chromium.chrome.browser.ActivityTabProvider;
import org.chromium.chrome.browser.ChromeActivity;
import org.chromium.chrome.browser.ChromeFeatureList;
import org.chromium.chrome.browser.tab.Tab;

import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;

/**
 * FrameLayout that supports side-wise slide gesture for history navigation. Inheriting
 * class may need to override {@link #isGestureConsumed()} if {@link #onTouchEvent} cannot
 * be relied upon to know whether the side-wise swipe related event was handled. Namely
 * {@link  android.support.v7.widget.RecyclerView}) always claims to handle touch events.
 */
public class HistoryNavigationLayout extends FrameLayout {
    @IntDef({GestureState.NONE, GestureState.STARTED, GestureState.DRAGGED})
    @Retention(RetentionPolicy.SOURCE)
    private @interface GestureState {
        int NONE = 0;
        int STARTED = 1;
        int DRAGGED = 2;
    }

    private GestureDetector mDetector;

    private SideSlideLayout mSideSlideLayout;

    // Async runnable for ending the refresh animation after the page first
    // loads a frame. This is used to provide a reasonable minimum animation time.
    private Runnable mStopNavigatingRunnable;

    // Handles removing the layout from the view hierarchy.  This is posted to ensure
    // it does not conflict with pending Android draws.
    private Runnable mDetachLayoutRunnable;

    // Provides activity tab where the navigation should happen.
    private ActivityTabProvider mTabProvider;

    public HistoryNavigationLayout(Context context) {
        this(context, null);
    }

    public HistoryNavigationLayout(Context context, AttributeSet attrs) {
        super(context, attrs);
        if (!ChromeFeatureList.isEnabled(ChromeFeatureList.OVERSCROLL_HISTORY_NAVIGATION)) return;
        if (context instanceof ChromeActivity) {
            mTabProvider = ((ChromeActivity) context).getActivityTabProvider();
            mDetector = new GestureDetector(getContext(), new SideNavGestureListener());
        }
    }

    private void createLayout() {
        mSideSlideLayout = new SideSlideLayout(getContext());
        mSideSlideLayout.setLayoutParams(
                new LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT));
        mSideSlideLayout.setEnabled(false);
        mSideSlideLayout.setOnNavigationListener((isForward) -> {
            if (mTabProvider == null) return;
            Tab tab = mTabProvider.getActivityTab();
            if (isForward) {
                tab.goForward();
            } else {
                if (canNavigate(/* forward= */ false)) {
                    tab.goBack();
                } else {
                    tab.getActivity().onBackPressed();
                }
            }
            cancelStopNavigatingRunnable();
            mSideSlideLayout.post(getStopNavigatingRunnable());
        });

        mSideSlideLayout.setOnResetListener(() -> {
            if (mDetachLayoutRunnable != null) return;
            mDetachLayoutRunnable = () -> {
                mDetachLayoutRunnable = null;
                detachSideSlideLayoutIfNecessary();
            };
            mSideSlideLayout.post(mDetachLayoutRunnable);
        });
    }

    @Override
    public boolean dispatchTouchEvent(MotionEvent e) {
        if (mDetector != null) {
            mDetector.onTouchEvent(e);
            if (e.getAction() == MotionEvent.ACTION_UP) {
                if (mSideSlideLayout != null) mSideSlideLayout.release(true);
            }
        }
        return super.dispatchTouchEvent(e);
    }

    private class SideNavGestureListener extends GestureDetector.SimpleOnGestureListener {
        private @GestureState int mState = GestureState.NONE;

        @Override
        public boolean onDown(MotionEvent event) {
            mState = GestureState.STARTED;
            return true;
        }

        @Override
        public boolean onScroll(MotionEvent e1, MotionEvent e2, float distanceX, float distanceY) {
            // onScroll needs handling only after the state moves away from none state.
            if (mState == GestureState.NONE) return true;

            if (wasLastSideSwipeGestureConsumed()) {
                reset();
                mState = GestureState.NONE;
                return true;
            }
            if (mState == GestureState.STARTED) {
                if (Math.abs(distanceX) > Math.abs(distanceY)) {
                    boolean forward = distanceX > 0;
                    boolean navigable = canNavigate(forward);
                    if (navigable || !forward) {
                        start(forward, !navigable);
                        mState = GestureState.DRAGGED;
                    }
                }
                if (mState != GestureState.DRAGGED) mState = GestureState.NONE;
            }
            if (mState == GestureState.DRAGGED) mSideSlideLayout.pull(-distanceX);
            return true;
        }
    }

    private boolean canNavigate(boolean forward) {
        if (mTabProvider == null) return false;
        Tab tab = mTabProvider.getActivityTab();
        return forward ? tab.canGoForward() : tab.canGoBack();
    }

    /**
     * Checks if the gesture event was consumed by one of children views, in which case
     * history navigation should not proceed. Whatever the child view does with the gesture
     * events should take precedence and not be disturbed by the navigation.
     *
     * @return {@code true} if gesture event is consumed by one of the children.
     */
    public boolean wasLastSideSwipeGestureConsumed() {
        return false;
    }

    private void start(boolean isForward, boolean enableCloseIndicator) {
        if (mSideSlideLayout == null) createLayout();
        mSideSlideLayout.setEnabled(true);
        mSideSlideLayout.setDirection(isForward);
        mSideSlideLayout.setEnableCloseIndicator(enableCloseIndicator);
        attachSideSlideLayoutIfNecessary();
        mSideSlideLayout.start();
    }

    /**
     * Reset navigation UI in action.
     */
    private void reset() {
        if (mSideSlideLayout != null) {
            cancelStopNavigatingRunnable();
            mSideSlideLayout.reset();
        }
    }

    /**
     * Cancel navigation UI with animation effect.
     */
    public void release() {
        if (mSideSlideLayout != null) {
            cancelStopNavigatingRunnable();
            mSideSlideLayout.release(false);
        }
    }

    private void cancelStopNavigatingRunnable() {
        if (mStopNavigatingRunnable != null) {
            mSideSlideLayout.removeCallbacks(mStopNavigatingRunnable);
            mStopNavigatingRunnable = null;
        }
    }

    private void cancelDetachLayoutRunnable() {
        if (mDetachLayoutRunnable != null) {
            mSideSlideLayout.removeCallbacks(mDetachLayoutRunnable);
            mDetachLayoutRunnable = null;
        }
    }

    private Runnable getStopNavigatingRunnable() {
        if (mStopNavigatingRunnable == null) {
            mStopNavigatingRunnable = () -> mSideSlideLayout.stopNavigating();
        }
        return mStopNavigatingRunnable;
    }

    private void attachSideSlideLayoutIfNecessary() {
        // The animation view is attached/detached on-demand to minimize overlap
        // with composited SurfaceView content.
        cancelDetachLayoutRunnable();
        if (mSideSlideLayout.getParent() == null) {
            addView(mSideSlideLayout);
        }
    }

    private void detachSideSlideLayoutIfNecessary() {
        cancelDetachLayoutRunnable();
        if (mSideSlideLayout.getParent() != null) {
            removeView(mSideSlideLayout);
        }
    }
}
