// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.android_webview.test;

import android.support.test.InstrumentationRegistry;
import android.support.test.filters.SmallTest;
import android.webkit.JavascriptInterface;

import org.junit.After;
import org.junit.Assert;
import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.runner.RunWith;

import org.chromium.android_webview.AwContents;
import org.chromium.android_webview.test.AwActivityTestRule.PopupInfo;
import org.chromium.android_webview.test.util.CommonResources;
import org.chromium.base.test.util.Feature;
import org.chromium.base.test.util.RetryOnFailure;
import org.chromium.content_public.browser.SelectionPopupController;
import org.chromium.content_public.browser.test.util.Criteria;
import org.chromium.content_public.browser.test.util.CriteriaHelper;
import org.chromium.content_public.browser.test.util.DOMUtils;
import org.chromium.content_public.browser.test.util.TestCallbackHelperContainer;
import org.chromium.content_public.browser.test.util.TestThreadUtils;
import org.chromium.net.test.util.TestWebServer;

import java.util.Locale;
import java.util.concurrent.TimeUnit;

/**
 * Tests for pop up window flow.
 */
@RunWith(AwJUnit4ClassRunner.class)
public class PopupWindowTest {
    @Rule
    public AwActivityTestRule mActivityTestRule = new AwActivityTestRule();

    private TestAwContentsClient mParentContentsClient;
    private AwTestContainerView mParentContainerView;
    private AwContents mParentContents;
    private TestWebServer mWebServer;

    private static final String POPUP_TITLE = "Popup Window";

    @Before
    public void setUp() throws Exception {
        mParentContentsClient = new TestAwContentsClient();
        mParentContainerView =
                mActivityTestRule.createAwTestContainerViewOnMainSync(mParentContentsClient);
        mParentContents = mParentContainerView.getAwContents();
        mWebServer = TestWebServer.start();
    }

    @After
    public void tearDown() throws Exception {
        if (mWebServer != null) {
            mWebServer.shutdown();
        }
    }

    @Test
    @SmallTest
    @Feature({"AndroidWebView"})
    public void testPopupWindow() throws Throwable {
        final String popupPath = "/popup.html";
        final String parentPageHtml = CommonResources.makeHtmlPageFrom("", "<script>"
                        + "function tryOpenWindow() {"
                        + "  var newWindow = window.open('" + popupPath + "');"
                        + "}</script>");

        final String popupPageHtml = CommonResources.makeHtmlPageFrom(
                "<title>" + POPUP_TITLE + "</title>",
                "This is a popup window");

        mActivityTestRule.triggerPopup(mParentContents, mParentContentsClient, mWebServer,
                parentPageHtml, popupPageHtml, popupPath, "tryOpenWindow()");
        AwContents popupContents =
                mActivityTestRule.connectPendingPopup(mParentContents).popupContents;
        Assert.assertEquals(POPUP_TITLE, mActivityTestRule.getTitleOnUiThread(popupContents));
    }

    @Test
    @SmallTest
    @Feature({"AndroidWebView"})
    public void testJavascriptInterfaceForPopupWindow() throws Throwable {
        // android.webkit.cts.WebViewTest#testJavascriptInterfaceForClientPopup
        final String popupPath = "/popup.html";
        final String parentPageHtml = CommonResources.makeHtmlPageFrom("",
                "<script>"
                        + "function tryOpenWindow() {"
                        + "  var newWindow = window.open('" + popupPath + "');"
                        + "}</script>");

        final String popupPageHtml = CommonResources.makeHtmlPageFrom(
                "<title>" + POPUP_TITLE + "</title>", "This is a popup window");

        mActivityTestRule.triggerPopup(mParentContents, mParentContentsClient, mWebServer,
                parentPageHtml, popupPageHtml, popupPath, "tryOpenWindow()");
        PopupInfo popupInfo = mActivityTestRule.createPopupContents(mParentContents);
        TestAwContentsClient popupContentsClient = popupInfo.popupContentsClient;
        final AwContents popupContents = popupInfo.popupContents;

        class DummyJavaScriptInterface {
            @JavascriptInterface
            public int test() {
                return 42;
            }
        }
        final DummyJavaScriptInterface obj = new DummyJavaScriptInterface();

        InstrumentationRegistry.getInstrumentation().runOnMainSync(
                () -> popupContents.addJavascriptInterface(obj, "dummy"));

        mActivityTestRule.loadPopupContents(mParentContents, popupInfo, null);

        AwActivityTestRule.pollInstrumentationThread(() -> {
            String ans = mActivityTestRule.executeJavaScriptAndWaitForResult(
                    popupContents, popupContentsClient, "dummy.test()");

            return ans.equals("42");
        });
    }

    @Test
    @SmallTest
    @Feature({"AndroidWebView"})
    public void testDefaultUserAgentsInParentAndChildWindows() throws Throwable {
        mActivityTestRule.getAwSettingsOnUiThread(mParentContents).setJavaScriptEnabled(true);
        mActivityTestRule.loadUrlSync(
                mParentContents, mParentContentsClient.getOnPageFinishedHelper(), "about:blank");
        String parentUserAgent = mActivityTestRule.executeJavaScriptAndWaitForResult(
                mParentContents, mParentContentsClient, "navigator.userAgent");

        final String popupPath = "/popup.html";
        final String myUserAgentString = "myUserAgent";
        final String parentPageHtml = CommonResources.makeHtmlPageFrom("",
                "<script>"
                        + "function tryOpenWindow() {"
                        + "  var newWindow = window.open('" + popupPath + "');"
                        + "}</script>");

        final String popupPageHtml = "<html><head><script>"
                + "document.title = navigator.userAgent;"
                + "</script><body><div id='a'></div></body></html>";

        mActivityTestRule.triggerPopup(mParentContents, mParentContentsClient, mWebServer,
                parentPageHtml, popupPageHtml, popupPath, "tryOpenWindow()");

        PopupInfo popupInfo = mActivityTestRule.createPopupContents(mParentContents);
        TestAwContentsClient popupContentsClient = popupInfo.popupContentsClient;
        final AwContents popupContents = popupInfo.popupContents;

        mActivityTestRule.loadPopupContents(mParentContents, popupInfo, null);

        final String childUserAgent = mActivityTestRule.executeJavaScriptAndWaitForResult(
                popupContents, popupContentsClient, "navigator.userAgent");

        Assert.assertEquals(parentUserAgent, childUserAgent);
    }

    @Test
    @SmallTest
    @Feature({"AndroidWebView"})
    public void testOverrideUserAgentInOnCreateWindow() throws Throwable {
        final String popupPath = "/popup.html";
        final String myUserAgentString = "myUserAgent";
        final String parentPageHtml = CommonResources.makeHtmlPageFrom("",
                "<script>"
                        + "function tryOpenWindow() {"
                        + "  var newWindow = window.open('" + popupPath + "');"
                        + "}</script>");

        final String popupPageHtml = "<html><head><script>"
                + "document.title = navigator.userAgent;"
                + "</script><body><div id='a'></div></body></html>";

        mActivityTestRule.triggerPopup(mParentContents, mParentContentsClient, mWebServer,
                parentPageHtml, popupPageHtml, popupPath, "tryOpenWindow()");

        PopupInfo popupInfo = mActivityTestRule.createPopupContents(mParentContents);
        TestAwContentsClient popupContentsClient = popupInfo.popupContentsClient;
        final AwContents popupContents = popupInfo.popupContents;

        // Override the user agent string for the popup window.
        mActivityTestRule.loadPopupContents(
                mParentContents, popupInfo, new AwActivityTestRule.OnCreateWindowHandler() {
                    @Override
                    public boolean onCreateWindow(AwContents awContents) {
                        awContents.getSettings().setUserAgentString(myUserAgentString);
                        return true;
                    }
                });

        CriteriaHelper.pollUiThread(Criteria.equals(
                myUserAgentString, () -> mActivityTestRule.getTitleOnUiThread(popupContents)));
    }

    @Test
    @SmallTest
    @Feature({"AndroidWebView"})
    public void testOnPageFinishedCalledOnDomModificationAfterNavigation() throws Throwable {
        final String popupPath = "/popup.html";
        final String parentPageHtml = CommonResources.makeHtmlPageFrom("",
                "<script>"
                        + "function tryOpenWindow() {"
                        + "  window.popupWindow = window.open('" + popupPath + "');"
                        + "  window.popupWindow.console = {};"
                        + "}"
                        + "function modifyDomOfPopup() {"
                        + "  window.popupWindow.document.body.innerHTML = 'Hello from the parent!';"
                        + "}</script>");

        final String popupPageHtml = CommonResources.makeHtmlPageFrom(
                "<title>" + POPUP_TITLE + "</title>",
                "This is a popup window");

        mActivityTestRule.triggerPopup(mParentContents, mParentContentsClient, mWebServer,
                parentPageHtml, popupPageHtml, popupPath, "tryOpenWindow()");
        PopupInfo popupInfo = mActivityTestRule.connectPendingPopup(mParentContents);
        Assert.assertEquals(
                POPUP_TITLE, mActivityTestRule.getTitleOnUiThread(popupInfo.popupContents));

        TestCallbackHelperContainer.OnPageFinishedHelper onPageFinishedHelper =
                popupInfo.popupContentsClient.getOnPageFinishedHelper();
        final int onPageFinishedCallCount = onPageFinishedHelper.getCallCount();

        mActivityTestRule.executeJavaScriptAndWaitForResult(
                mParentContents, mParentContentsClient, "modifyDomOfPopup()");
        // Test that |waitForCallback| does not time out.
        onPageFinishedHelper.waitForCallback(onPageFinishedCallCount);
    }

    @Test
    @SmallTest
    @Feature({"AndroidWebView"})
    @RetryOnFailure
    public void testPopupWindowTextHandle() throws Throwable {
        final String popupPath = "/popup.html";
        final String parentPageHtml = CommonResources.makeHtmlPageFrom("", "<script>"
                        + "function tryOpenWindow() {"
                        + "  var newWindow = window.open('" + popupPath + "');"
                        + "}</script>");

        final String popupPageHtml = CommonResources.makeHtmlPageFrom(
                "<title>" + POPUP_TITLE + "</title>",
                "<span id=\"plain_text\" class=\"full_view\">This is a popup window.</span>");

        mActivityTestRule.triggerPopup(mParentContents, mParentContentsClient, mWebServer,
                parentPageHtml, popupPageHtml, popupPath, "tryOpenWindow()");
        PopupInfo popupInfo = mActivityTestRule.connectPendingPopup(mParentContents);
        final AwContents popupContents = popupInfo.popupContents;
        TestAwContentsClient popupContentsClient = popupInfo.popupContentsClient;
        Assert.assertEquals(POPUP_TITLE, mActivityTestRule.getTitleOnUiThread(popupContents));

        AwActivityTestRule.enableJavaScriptOnUiThread(popupContents);

        // Now long press on some texts and see if the text handles show up.
        DOMUtils.longPressNode(popupContents.getWebContents(), "plain_text");
        SelectionPopupController controller = TestThreadUtils.runOnUiThreadBlocking(
                () -> SelectionPopupController.fromWebContents(popupContents.getWebContents()));
        assertWaitForSelectActionBarStatus(true, controller);
        Assert.assertTrue(TestThreadUtils.runOnUiThreadBlocking(() -> controller.hasSelection()));

        // Now hide the select action bar. This should hide the text handles and
        // clear the selection.
        hideSelectActionMode(controller);

        assertWaitForSelectActionBarStatus(false, controller);
        String jsGetSelection = "window.getSelection().toString()";
        // Test window.getSelection() returns empty string "" literally.
        Assert.assertEquals("\"\"",
                mActivityTestRule.executeJavaScriptAndWaitForResult(
                        popupContents, popupContentsClient, jsGetSelection));
    }

    @Test
    @SmallTest
    @Feature({"AndroidWebView"})
    public void testPopupWindowHasUserGestureForUserInitiated() throws Throwable {
        runPopupUserGestureTest(true);
    }

    @Test
    @SmallTest
    @Feature({"AndroidWebView"})
    public void testPopupWindowHasUserGestureForUserInitiatedNoOpener() throws Throwable {
        runPopupUserGestureTest(false);
    }

    private void runPopupUserGestureTest(boolean hasOpener) throws Throwable {
        TestThreadUtils.runOnUiThreadBlocking(() -> {
            mParentContents.getSettings().setJavaScriptEnabled(true);
            mParentContents.getSettings().setSupportMultipleWindows(true);
            mParentContents.getSettings().setJavaScriptCanOpenWindowsAutomatically(true);
        });

        final String body = String.format(Locale.US,
                "<a href=\"popup.html\" id=\"link\" %s target=\"_blank\">example.com</a>",
                hasOpener ? "" : "rel=\"noopener noreferrer\"");
        final String mainHtml = CommonResources.makeHtmlPageFrom("", body);
        final String openerUrl = mWebServer.setResponse("/popupOpener.html", mainHtml, null);
        final String popupUrl = mWebServer.setResponse("/popup.html",
                CommonResources.makeHtmlPageFrom(
                        "<title>" + POPUP_TITLE + "</title>", "This is a popup window"),
                null);

        mParentContentsClient.getOnCreateWindowHelper().setReturnValue(true);
        mActivityTestRule.loadUrlSync(
                mParentContents, mParentContentsClient.getOnPageFinishedHelper(), openerUrl);

        TestAwContentsClient.OnCreateWindowHelper onCreateWindowHelper =
                mParentContentsClient.getOnCreateWindowHelper();
        int currentCallCount = onCreateWindowHelper.getCallCount();
        DOMUtils.clickNode(mParentContents.getWebContents(), "link");
        onCreateWindowHelper.waitForCallback(
                currentCallCount, 1, AwActivityTestRule.WAIT_TIMEOUT_MS, TimeUnit.MILLISECONDS);

        Assert.assertTrue(onCreateWindowHelper.getIsUserGesture());
    }

    @Test
    @SmallTest
    @Feature({"AndroidWebView"})
    public void testPopupWindowNoUserGestureForJsInitiated() throws Throwable {
        final String popupPath = "/popup.html";
        final String openerPageHtml = CommonResources.makeHtmlPageFrom("",
                "<script>"
                        + "function tryOpenWindow() {"
                        + "  var newWindow = window.open('" + popupPath + "');"
                        + "}</script>");

        final String popupPageHtml = CommonResources.makeHtmlPageFrom(
                "<title>" + POPUP_TITLE + "</title>", "This is a popup window");

        mActivityTestRule.triggerPopup(mParentContents, mParentContentsClient, mWebServer,
                openerPageHtml, popupPageHtml, popupPath, "tryOpenWindow()");
        TestAwContentsClient.OnCreateWindowHelper onCreateWindowHelper =
                mParentContentsClient.getOnCreateWindowHelper();
        Assert.assertFalse(onCreateWindowHelper.getIsUserGesture());
    }

    // Copied from imeTest.java.
    private void assertWaitForSelectActionBarStatus(
            boolean show, final SelectionPopupController controller) {
        CriteriaHelper.pollUiThread(
                Criteria.equals(show, () -> controller.isSelectActionBarShowing()));
    }

    private void hideSelectActionMode(final SelectionPopupController controller) {
        InstrumentationRegistry.getInstrumentation().runOnMainSync(
                () -> controller.destroySelectActionMode());
    }
}
