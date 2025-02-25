// Copyright 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.android_webview.test;

import android.support.test.InstrumentationRegistry;
import android.support.test.filters.MediumTest;
import android.support.test.filters.SmallTest;
import android.util.Pair;

import org.junit.After;
import org.junit.Assert;
import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.runner.RunWith;

import org.chromium.android_webview.AwContents;
import org.chromium.android_webview.AwCookieManager;
import org.chromium.android_webview.AwSettings;
import org.chromium.android_webview.test.util.CookieUtils;
import org.chromium.android_webview.test.util.CookieUtils.TestCallback;
import org.chromium.android_webview.test.util.JSUtils;
import org.chromium.base.Callback;
import org.chromium.base.test.util.Feature;
import org.chromium.content_public.browser.WebContents;
import org.chromium.content_public.browser.test.util.JavaScriptUtils;
import org.chromium.net.test.EmbeddedTestServer;
import org.chromium.net.test.util.TestWebServer;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.Date;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

/**
 * Tests for the CookieManager.
 */
@RunWith(AwJUnit4ClassRunner.class)
public class CookieManagerTest {
    @Rule
    public AwActivityTestRule mActivityTestRule = new AwActivityTestRule();

    private AwCookieManager mCookieManager;
    private TestAwContentsClient mContentsClient;
    private AwContents mAwContents;

    @Before
    public void setUp() throws Exception {
        mCookieManager = new AwCookieManager();
        mContentsClient = new TestAwContentsClient();
        final AwTestContainerView testContainerView =
                mActivityTestRule.createAwTestContainerViewOnMainSync(mContentsClient);
        mAwContents = testContainerView.getAwContents();
        mAwContents.getSettings().setJavaScriptEnabled(true);
        Assert.assertNotNull(mCookieManager);
    }

    @After
    public void tearDown() throws Exception {
        try {
            clearCookies();
        } catch (Throwable e) {
            throw new RuntimeException("Could not clear cookies.");
        }
    }

    @Test
    @SmallTest
    @Feature({"AndroidWebView", "Privacy"})
    public void testAcceptCookie_default() throws Throwable {
        Assert.assertTrue("Expected CookieManager to accept cookies by default",
                mCookieManager.acceptCookie());
    }

    @Test
    @SmallTest
    @Feature({"AndroidWebView", "Privacy"})
    public void testAcceptCookie_setterGetterFunctionality() throws Throwable {
        mCookieManager.setAcceptCookie(false);
        Assert.assertFalse("Expected #acceptCookie() to return false after setAcceptCookie(false)",
                mCookieManager.acceptCookie());
        mCookieManager.setAcceptCookie(true);
        Assert.assertTrue("Expected #acceptCookie() to return true after setAcceptCookie(true)",
                mCookieManager.acceptCookie());
    }

    /**
     * @param acceptCookieValue the value passed into {@link AwCookieManager#setAcceptCookie}.
     * @param cookieSuffix a suffix to use for the cookie name, should be unique to avoid
     *        side-effects from other tests.
     */
    private void testAcceptCookieHelper(boolean acceptCookieValue, String cookieSuffix)
            throws Throwable {
        mCookieManager.setAcceptCookie(acceptCookieValue);

        TestWebServer webServer = TestWebServer.start();
        try {
            String path = "/cookie_test.html";
            String responseStr =
                    "<html><head><title>TEST!</title></head><body>HELLO!</body></html>";
            String url = webServer.setResponse(path, responseStr, null);
            mActivityTestRule.loadUrlSync(
                    mAwContents, mContentsClient.getOnPageFinishedHelper(), url);
            final String jsCookieName = "js-test" + cookieSuffix;
            setCookieWithJavaScript(jsCookieName, "value");
            if (acceptCookieValue) {
                waitForCookie(url);
                assertHasCookies(url);
                validateCookies(url, jsCookieName);
            } else {
                assertNoCookies(url);
            }

            final List<Pair<String, String>> responseHeaders =
                    new ArrayList<Pair<String, String>>();
            final String headerCookieName = "header-test" + cookieSuffix;
            responseHeaders.add(
                    Pair.create("Set-Cookie", headerCookieName + "=header-value path=" + path));
            url = webServer.setResponse(path, responseStr, responseHeaders);
            mActivityTestRule.loadUrlSync(
                    mAwContents, mContentsClient.getOnPageFinishedHelper(), url);
            if (acceptCookieValue) {
                waitForCookie(url);
                assertHasCookies(url);
                validateCookies(url, jsCookieName, headerCookieName);
            } else {
                assertNoCookies(url);
            }
        } finally {
            webServer.shutdown();
        }
    }

    @Test
    @MediumTest
    @Feature({"AndroidWebView", "Privacy"})
    public void testAcceptCookie_falseWontSetCookies() throws Throwable {
        testAcceptCookieHelper(false, "-disabled");
    }

    @Test
    @MediumTest
    @Feature({"AndroidWebView", "Privacy"})
    public void testAcceptCookie_trueWillSetCookies() throws Throwable {
        testAcceptCookieHelper(true, "-enabled");
    }

    @Test
    @MediumTest
    @Feature({"AndroidWebView", "Privacy"})
    public void testAcceptCookie_falseDoNotSendCookies() throws Throwable {
        blockAllCookies();
        AwActivityTestRule.enableJavaScriptOnUiThread(mAwContents);

        EmbeddedTestServer embeddedTestServer = EmbeddedTestServer.createAndStartServer(
                InstrumentationRegistry.getInstrumentation().getContext());
        try {
            final String url = embeddedTestServer.getURL("/echoheader?Cookie");
            String cookieName = "java-test";
            mCookieManager.setCookie(url, cookieName + "=should-not-work");

            // Setting cookies should still affect the CookieManager itself
            assertHasCookies(url);

            mActivityTestRule.loadUrlSync(
                    mAwContents, mContentsClient.getOnPageFinishedHelper(), url);
            String jsValue = getCookieWithJavaScript(cookieName);
            String message =
                    "WebView should not expose cookies to JavaScript (with setAcceptCookie "
                    + "disabled)";
            Assert.assertEquals(message, "\"\"", jsValue);

            final String cookieHeader = mActivityTestRule.getJavaScriptResultBodyTextContent(
                    mAwContents, mContentsClient);
            message = "WebView should not expose cookies via the Cookie header (with "
                    + "setAcceptCookie disabled)";
            Assert.assertEquals(message, "None", cookieHeader);
        } finally {
            embeddedTestServer.stopAndDestroyServer();
        }
    }

    @Test
    @MediumTest
    @Feature({"AndroidWebView"})
    public void testEmbedderCanSeeRestrictedCookies() throws Throwable {
        TestWebServer webServer = TestWebServer.start();
        try {
            // Set a cookie with the httponly flag, one with samesite=Strict, and one with
            // samesite=Lax, to ensure that they are all visible to CookieManager in the app.
            String cookies[] = {"httponly=foo1; HttpOnly", "strictsamesite=foo2; SameSite=Strict",
                    "laxsamesite=foo3; SameSite=Lax"};
            List<Pair<String, String>> responseHeaders = new ArrayList<Pair<String, String>>();
            for (String cookie : cookies) {
                responseHeaders.add(Pair.create("Set-Cookie", cookie));
            }
            String url = webServer.setResponse("/", "test", responseHeaders);
            mActivityTestRule.loadUrlSync(
                    mAwContents, mContentsClient.getOnPageFinishedHelper(), url);
            waitForCookie(url);
            assertHasCookies(url);
            validateCookies(url, "httponly", "strictsamesite", "laxsamesite");
        } finally {
            webServer.shutdown();
        }
    }

    private void setCookieWithJavaScript(final String name, final String value)
            throws Throwable {
        JSUtils.executeJavaScriptAndWaitForResult(InstrumentationRegistry.getInstrumentation(),
                mAwContents, mContentsClient.getOnEvaluateJavaScriptResultHelper(),
                "var expirationDate = new Date();"
                        + "expirationDate.setDate(expirationDate.getDate() + 5);"
                        + "document.cookie='" + name + "=" + value
                        + "; expires=' + expirationDate.toUTCString();");
    }

    private String getCookieWithJavaScript(final String name) throws Throwable {
        return JSUtils.executeJavaScriptAndWaitForResult(
                InstrumentationRegistry.getInstrumentation(), mAwContents,
                mContentsClient.getOnEvaluateJavaScriptResultHelper(), "document.cookie");
    }

    @Test
    @MediumTest
    @Feature({"AndroidWebView", "Privacy"})
    public void testRemoveAllCookies() throws Exception {
        final String cookieUrl = "http://www.example.com";
        mCookieManager.setCookie(cookieUrl, "name=test");
        assertHasCookies(cookieUrl);
        mCookieManager.removeAllCookies();
        assertNoCookies();
    }

    @Test
    @MediumTest
    @Feature({"AndroidWebView", "Privacy"})
    public void testRemoveSessionCookies() throws Exception {
        final String url = "http://www.example.com";
        final String sessionCookie = "cookie1=peter";
        final String normalCookie = "cookie2=sue";

        mCookieManager.setCookie(url, sessionCookie);
        mCookieManager.setCookie(url, makeExpiringCookie(normalCookie, 600));

        mCookieManager.removeSessionCookies();

        String allCookies = mCookieManager.getCookie(url);
        Assert.assertFalse(allCookies.contains(sessionCookie));
        Assert.assertTrue(allCookies.contains(normalCookie));
    }

    @Test
    @MediumTest
    @Feature({"AndroidWebView", "Privacy"})
    public void testSetCookie() throws Throwable {
        String url = "http://www.example.com";
        String cookie = "name=test";
        mCookieManager.setCookie(url, cookie);
        assertCookieEquals(cookie, url);
    }

    @Test
    @MediumTest
    @Feature({"AndroidWebView", "Privacy"})
    public void testSetCookieWithDomainForUrl() throws Throwable {
        // If the app passes ".www.example.com" or "http://.www.example.com", the glue layer "fixes"
        // this to "http:///.www.example.com"
        String url = "http:///.www.example.com";
        String sameSubdomainUrl = "http://a.www.example.com";
        String differentSubdomainUrl = "http://different.sub.example.com";
        String cookie = "name=test";
        mCookieManager.setCookie(url, cookie);
        assertCookieEquals(cookie, sameSubdomainUrl);
        assertNoCookies(differentSubdomainUrl);
    }

    @Test
    @MediumTest
    @Feature({"AndroidWebView", "Privacy"})
    public void testSetCookieWithDomainForUrlAndExistingDomainAttribute() throws Throwable {
        String url = "http:///.www.example.com";
        String differentSubdomainUrl = "http://different.sub.example.com";
        String cookie = "name=test";
        mCookieManager.setCookie(url, cookie + "; doMaIN \t  =.example.com");
        assertCookieEquals(cookie, url);
        assertCookieEquals(cookie, differentSubdomainUrl);
    }

    @Test
    @MediumTest
    @Feature({"AndroidWebView", "Privacy"})
    public void testSetCookieWithDomainForUrlWithTrailingSemicolonInCookie() throws Throwable {
        String url = "http:///.www.example.com";
        String sameSubdomainUrl = "http://a.www.example.com";
        String differentSubdomainUrl = "http://different.sub.example.com";
        String cookie = "name=test";
        mCookieManager.setCookie(url, cookie + ";");
        assertCookieEquals(cookie, sameSubdomainUrl);
        assertNoCookies(differentSubdomainUrl);
    }

    @Test
    @MediumTest
    @Feature({"AndroidWebView", "Privacy"})
    public void testSetSecureCookieForHttpUrl() throws Throwable {
        String url = "http://www.example.com";
        String secureUrl = "https://www.example.com";
        String cookie = "name=test";
        mCookieManager.setCookie(url, cookie + ";secure");
        assertCookieEquals(cookie, secureUrl);
    }

    @Test
    @MediumTest
    @Feature({"AndroidWebView", "Privacy"})
    public void testHasCookie() throws Throwable {
        Assert.assertFalse(mCookieManager.hasCookies());
        mCookieManager.setCookie("http://www.example.com", "name=test");
        Assert.assertTrue(mCookieManager.hasCookies());
    }

    @Test
    @MediumTest
    @Feature({"AndroidWebView", "Privacy"})
    public void testSetCookieCallback_goodUrl() throws Throwable {
        final String url = "http://www.example.com";
        final String cookie = "name=test";

        final TestCallback<Boolean> callback = new TestCallback<Boolean>();
        int callCount = callback.getOnResultHelper().getCallCount();

        setCookieOnUiThread(url, cookie, callback);
        callback.getOnResultHelper().waitForCallback(callCount);
        Assert.assertTrue(callback.getValue());
        assertCookieEquals(cookie, url);
    }

    @Test
    @MediumTest
    @Feature({"AndroidWebView", "Privacy"})
    public void testSetCookieCallback_badUrl() throws Throwable {
        final String cookie = "name=test";
        final String brokenUrl = "foo";

        final TestCallback<Boolean> callback = new TestCallback<Boolean>();
        int callCount = callback.getOnResultHelper().getCallCount();

        setCookieOnUiThread(brokenUrl, cookie, callback);
        callback.getOnResultHelper().waitForCallback(callCount);
        Assert.assertFalse("Cookie should not be set for bad URLs", callback.getValue());
        assertNoCookies(brokenUrl);
    }

    @Test
    @MediumTest
    @Feature({"AndroidWebView", "Privacy"})
    public void testSetCookieNullCallback() throws Throwable {
        allowFirstPartyCookies();

        final String url = "http://www.example.com";
        final String cookie = "name=test";

        mCookieManager.setCookie(url, cookie, null);

        AwActivityTestRule.pollInstrumentationThread(() -> mCookieManager.hasCookies());
        assertCookieEquals(cookie, url);
    }

    @Test
    @MediumTest
    @Feature({"AndroidWebView", "Privacy"})
    public void testRemoveAllCookiesCallback() throws Throwable {
        TestCallback<Boolean> callback = new TestCallback<Boolean>();
        int callCount = callback.getOnResultHelper().getCallCount();

        mCookieManager.setCookie("http://www.example.com", "name=test");

        // When we remove all cookies the first time some cookies are removed.
        removeAllCookiesOnUiThread(callback);
        callback.getOnResultHelper().waitForCallback(callCount);
        Assert.assertTrue(callback.getValue());
        Assert.assertFalse(mCookieManager.hasCookies());

        callCount = callback.getOnResultHelper().getCallCount();

        // The second time none are removed.
        removeAllCookiesOnUiThread(callback);
        callback.getOnResultHelper().waitForCallback(callCount);
        Assert.assertFalse(callback.getValue());
    }

    @Test
    @MediumTest
    @Feature({"AndroidWebView", "Privacy"})
    public void testRemoveAllCookiesNullCallback() throws Throwable {
        mCookieManager.setCookie("http://www.example.com", "name=test");

        mCookieManager.removeAllCookies(null);

        // Eventually the cookies are removed.
        AwActivityTestRule.pollInstrumentationThread(() -> !mCookieManager.hasCookies());
    }

    @Test
    @MediumTest
    @Feature({"AndroidWebView", "Privacy"})
    public void testRemoveSessionCookiesCallback() throws Throwable {
        final String url = "http://www.example.com";
        final String sessionCookie = "cookie1=peter";
        final String normalCookie = "cookie2=sue";

        TestCallback<Boolean> callback = new TestCallback<Boolean>();
        int callCount = callback.getOnResultHelper().getCallCount();

        mCookieManager.setCookie(url, sessionCookie);
        mCookieManager.setCookie(url, makeExpiringCookie(normalCookie, 600));

        // When there is a session cookie then it is removed.
        removeSessionCookiesOnUiThread(callback);
        callback.getOnResultHelper().waitForCallback(callCount);
        Assert.assertTrue(callback.getValue());
        String allCookies = mCookieManager.getCookie(url);
        Assert.assertTrue(!allCookies.contains(sessionCookie));
        Assert.assertTrue(allCookies.contains(normalCookie));

        callCount = callback.getOnResultHelper().getCallCount();

        // If there are no session cookies then none are removed.
        removeSessionCookiesOnUiThread(callback);
        callback.getOnResultHelper().waitForCallback(callCount);
        Assert.assertFalse(callback.getValue());
    }

    @Test
    @MediumTest
    @Feature({"AndroidWebView", "Privacy"})
    public void testRemoveSessionCookiesNullCallback() throws Throwable {
        final String url = "http://www.example.com";
        final String sessionCookie = "cookie1=peter";
        final String normalCookie = "cookie2=sue";

        mCookieManager.setCookie(url, sessionCookie);
        mCookieManager.setCookie(url, makeExpiringCookie(normalCookie, 600));
        String allCookies = mCookieManager.getCookie(url);
        Assert.assertTrue(allCookies.contains(sessionCookie));
        Assert.assertTrue(allCookies.contains(normalCookie));

        mCookieManager.removeSessionCookies(null);

        // Eventually the session cookie is removed.
        AwActivityTestRule.pollInstrumentationThread(() -> {
            String c = mCookieManager.getCookie(url);
            return !c.contains(sessionCookie) && c.contains(normalCookie);
        });
    }

    @Test
    @MediumTest
    @Feature({"AndroidWebView", "Privacy"})
    public void testExpiredCookiesAreNotSet() throws Exception {
        final String url = "http://www.example.com";
        final String cookie = "cookie1=peter";

        mCookieManager.setCookie(url, makeExpiringCookie(cookie, -1));
        assertNoCookies(url);
    }

    @Test
    @MediumTest
    @Feature({"AndroidWebView", "Privacy"})
    public void testCookiesExpire() throws Exception {
        final String url = "http://www.example.com";
        final String cookie = "cookie1=peter";

        mCookieManager.setCookie(url, makeExpiringCookieMs(cookie, 1200));

        // The cookie exists:
        Assert.assertTrue(mCookieManager.hasCookies());

        // But eventually expires:
        AwActivityTestRule.pollInstrumentationThread(() -> !mCookieManager.hasCookies());
    }

    @Test
    @MediumTest
    @Feature({"AndroidWebView", "Privacy"})
    public void testCookieExpiration() throws Exception {
        final String url = "http://www.example.com";
        final String sessionCookie = "cookie1=peter";
        final String longCookie = "cookie2=marc";

        mCookieManager.setCookie(url, sessionCookie);
        mCookieManager.setCookie(url, makeExpiringCookie(longCookie, 600));

        String allCookies = mCookieManager.getCookie(url);
        Assert.assertTrue(allCookies.contains(sessionCookie));
        Assert.assertTrue(allCookies.contains(longCookie));

        // Removing expired cookies doesn't have an observable effect but since people will still
        // be calling it for a while it shouldn't break anything either.
        mCookieManager.removeExpiredCookies();

        allCookies = mCookieManager.getCookie(url);
        Assert.assertTrue(allCookies.contains(sessionCookie));
        Assert.assertTrue(allCookies.contains(longCookie));
    }

    @Test
    @MediumTest
    @Feature({"AndroidWebView", "Privacy"})
    public void testThirdPartyCookie() throws Throwable {
        // In theory we need two servers to test this, one server ('the first
        // party') which returns a response with a link to a second server ('the third party') at
        // different origin. This second server attempts to set a cookie which should fail if
        // AcceptThirdPartyCookie() is false. Strictly according to the letter of RFC6454 it should
        // be possible to set this situation up with two TestServers on different ports (these count
        // as having different origins) but Chrome is not strict about this and does not check the
        // port. Instead we cheat making some of the urls come from localhost and some from
        // 127.0.0.1 which count (both in theory and pratice) as having different origins.
        TestWebServer webServer = TestWebServer.start();
        try {
            allowFirstPartyCookies();
            blockThirdPartyCookies(mAwContents);

            // We can't set third party cookies.
            // First on the third party server we create a url which tries to set a cookie.
            String cookieUrl = toThirdPartyUrl(
                    makeCookieUrl(webServer, "/cookie_1.js", "test1", "value1"));
            // Then we create a url on the first party server which links to the first url.
            String url = makeScriptLinkUrl(webServer, "/content_1.html", cookieUrl);
            mActivityTestRule.loadUrlSync(
                    mAwContents, mContentsClient.getOnPageFinishedHelper(), url);
            assertNoCookies(cookieUrl);

            allowThirdPartyCookies(mAwContents);

            // We can set third party cookies.
            cookieUrl = toThirdPartyUrl(
                    makeCookieUrl(webServer, "/cookie_2.js", "test2", "value2"));
            url = makeScriptLinkUrl(webServer, "/content_2.html", cookieUrl);
            mActivityTestRule.loadUrlSync(
                    mAwContents, mContentsClient.getOnPageFinishedHelper(), url);
            waitForCookie(cookieUrl);
            assertHasCookies(cookieUrl);
            validateCookies(cookieUrl, "test2");
        } finally {
            webServer.shutdown();
        }
    }

    @Test
    @MediumTest
    @Feature({"AndroidWebView", "Privacy"})
    public void testThirdPartyCookie_redirectFromThirdToFirst() throws Throwable {
        TestWebServer webServer = TestWebServer.start();
        try {
            allowFirstPartyCookies();
            blockThirdPartyCookies(mAwContents);

            // Load a page with a third-party resource. The resource URL redirects to a new URL
            // (which is first-party relative to the main frame). The final resource URL should
            // successfully set its cookies (because it's first-party).
            String resourcePath = "/cookie_1.js";
            String firstPartyCookieUrl = makeCookieUrl(webServer, resourcePath, "test1", "value1");
            String thirdPartyRedirectUrl = toThirdPartyUrl(
                    webServer.setRedirect("/redirect_cookie_1.js", firstPartyCookieUrl));
            String contentUrl =
                    makeScriptLinkUrl(webServer, "/content_1.html", thirdPartyRedirectUrl);
            mActivityTestRule.loadUrlSync(
                    mAwContents, mContentsClient.getOnPageFinishedHelper(), contentUrl);
            assertCookieEquals("test1=value1", firstPartyCookieUrl);
        } finally {
            webServer.shutdown();
        }
    }

    @Test
    @MediumTest
    @Feature({"AndroidWebView", "Privacy"})
    public void testThirdPartyCookie_redirectFromFirstPartyToThird() throws Throwable {
        TestWebServer webServer = TestWebServer.start();
        try {
            allowFirstPartyCookies();
            blockThirdPartyCookies(mAwContents);

            // Load a page with a first-party resource. The resource URL redirects to a new URL
            // (which is third-party relative to the main frame). The final resource URL should be
            // unable to set cookies (because it's third-party).
            String resourcePath = "/cookie_2.js";
            String thirdPartyCookieUrl =
                    toThirdPartyUrl(makeCookieUrl(webServer, resourcePath, "test2", "value2"));
            String firstPartyRedirectUrl =
                    webServer.setRedirect("/redirect_cookie_2.js", thirdPartyCookieUrl);
            String contentUrl =
                    makeScriptLinkUrl(webServer, "/content_2.html", firstPartyRedirectUrl);
            mActivityTestRule.loadUrlSync(
                    mAwContents, mContentsClient.getOnPageFinishedHelper(), contentUrl);
            assertNoCookies(thirdPartyCookieUrl);
        } finally {
            webServer.shutdown();
        }
    }

    private String webSocketCookieHelper(
            boolean shouldUseThirdPartyUrl, String cookieKey, String cookieValue) throws Throwable {
        TestWebServer webServer = TestWebServer.start();
        try {
            // |cookieUrl| sets a cookie on response.
            String cookieUrl =
                    makeCookieWebSocketUrl(webServer, "/cookie_1", cookieKey, cookieValue);
            if (shouldUseThirdPartyUrl) {
                // Let |cookieUrl| be a third-party url to test third-party cookies.
                cookieUrl = toThirdPartyUrl(cookieUrl);
            }
            // This html file includes a script establishing a WebSocket connection to |cookieUrl|.
            String url = makeWebSocketScriptUrl(webServer, "/content_1.html", cookieUrl);
            mActivityTestRule.loadUrlSync(
                    mAwContents, mContentsClient.getOnPageFinishedHelper(), url);
            final String connecting = "0"; // WebSocket.CONNECTING
            final String closed = "3"; // WebSocket.CLOSED
            String readyState = connecting;
            WebContents webContents = mAwContents.getWebContents();
            while (!readyState.equals(closed)) {
                readyState = JavaScriptUtils.executeJavaScriptAndWaitForResult(
                        webContents, "ws.readyState");
            }
            Assert.assertEquals("true",
                    JavaScriptUtils.executeJavaScriptAndWaitForResult(webContents, "hasOpened"));
            return mCookieManager.getCookie(cookieUrl);
        } finally {
            webServer.shutdown();
        }
    }

    @Test
    @MediumTest
    @Feature({"AndroidWebView", "Privacy"})
    public void testCookieForWebSocketHandshake_thirdParty_enabled() throws Throwable {
        allowFirstPartyCookies();
        allowThirdPartyCookies(mAwContents);
        String cookieKey = "test1";
        String cookieValue = "value1";
        Assert.assertEquals(cookieKey + "=" + cookieValue,
                webSocketCookieHelper(true /* shouldUseThirdPartyUrl */, cookieKey, cookieValue));
    }

    @Test
    @MediumTest
    @Feature({"AndroidWebView", "Privacy"})
    public void testCookieForWebSocketHandshake_thirdParty_disabled() throws Throwable {
        allowFirstPartyCookies();
        blockThirdPartyCookies(mAwContents);
        String cookieKey = "test1";
        String cookieValue = "value1";
        Assert.assertNull("Should not set 3P cookie when 3P cookie settings are disabled",
                webSocketCookieHelper(true /* shouldUseThirdPartyUrl */, cookieKey, cookieValue));
    }

    @Test
    @MediumTest
    @Feature({"AndroidWebView", "Privacy"})
    public void testCookieForWebSocketHandshake_firstParty_enabled() throws Throwable {
        allowFirstPartyCookies();
        allowThirdPartyCookies(mAwContents);
        String cookieKey = "test1";
        String cookieValue = "value1";
        Assert.assertEquals(cookieKey + "=" + cookieValue,
                webSocketCookieHelper(false /* shouldUseThirdPartyUrl */, cookieKey, cookieValue));
    }

    @Test
    @MediumTest
    @Feature({"AndroidWebView", "Privacy"})
    public void testCookieForWebSocketHandshake_firstParty_disabled() throws Throwable {
        blockAllCookies();
        String cookieKey = "test1";
        String cookieValue = "value1";
        Assert.assertNull("Should not set 1P cookie when 1P cookie settings are disabled",
                webSocketCookieHelper(false /* shouldUseThirdPartyUrl */, cookieKey, cookieValue));
    }

    /**
     * Creates a response on the TestWebServer which attempts to set a cookie when fetched.
     * @param  webServer  the webServer on which to create the response
     * @param  path the path component of the url (e.g "/cookie_test.html")
     * @param  key the key of the cookie
     * @param  value the value of the cookie
     * @return  the url which gets the response
     */
    private String makeCookieUrl(TestWebServer webServer, String path, String key, String value) {
        String response = "";
        List<Pair<String, String>> responseHeaders = new ArrayList<Pair<String, String>>();
        responseHeaders.add(
                Pair.create("Set-Cookie", key + "=" + value + "; path=" + path));
        return webServer.setResponse(path, response, responseHeaders);
    }

    /**
     * Creates a response on the TestWebServer which attempts to set a cookie when establishing a
     * WebSocket connection.
     * @param  webServer  the webServer on which to create the response
     * @param  path the path component of the url (e.g "/cookie_test.html")
     * @param  key the key of the cookie
     * @param  value the value of the cookie
     * @return  the url which gets the response
     */
    private String makeCookieWebSocketUrl(
            TestWebServer webServer, String path, String key, String value) {
        List<Pair<String, String>> responseHeaders = new ArrayList<Pair<String, String>>();
        responseHeaders.add(Pair.create("Set-Cookie", key + "=" + value + "; path=" + path));
        return webServer.setResponseForWebSocket(path, responseHeaders);
    }

    /**
     * Creates a response on the TestWebServer which contains a script tag with an external src.
     * @param  webServer  the webServer on which to create the response
     * @param  path the path component of the url (e.g "/my_thing_with_script.html")
     * @param  url the url which which should appear as the src of the script tag.
     * @return  the url which gets the response
     */
    private String makeScriptLinkUrl(TestWebServer webServer, String path, String url) {
        String responseStr = "<html><head><title>Content!</title></head>"
                + "<body><script src=" + url + "></script></body></html>";
        return webServer.setResponse(path, responseStr, null);
    }

    /**
     * Creates a response on the TestWebServer which contains a script establishing a WebSocket
     * connection.
     * @param  webServer  the webServer on which to create the response
     * @param  path the path component of the url (e.g "/my_thing_with_script.html")
     * @param  url the url which which should appear as the src of the script tag.
     * @return  the url which gets the response
     */
    private String makeWebSocketScriptUrl(TestWebServer webServer, String path, String url) {
        String responseStr = "<html><head><title>Content!</title></head>"
                + "<body><script>\n"
                + "let ws = new WebSocket('" + url.replaceAll("^http", "ws") + "');\n"
                + "let hasOpened = false;\n"
                + "ws.onopen = () => hasOpened = true;\n"
                + "</script></body></html>";
        return webServer.setResponse(path, responseStr, null);
    }

    @Test
    @MediumTest
    @Feature({"AndroidWebView", "Privacy"})
    public void testThirdPartyJavascriptCookie() throws Throwable {
        TestWebServer webServer = TestWebServer.start();
        try {
            // This test again uses 127.0.0.1/localhost trick to simulate a third party.
            ThirdPartyCookiesTestHelper thirdParty =
                    new ThirdPartyCookiesTestHelper(webServer);

            allowFirstPartyCookies();
            blockThirdPartyCookies(thirdParty.getAwContents());

            // We can't set third party cookies.
            thirdParty.assertThirdPartyIFrameCookieResult("1", false);

            allowThirdPartyCookies(thirdParty.getAwContents());

            // We can set third party cookies.
            thirdParty.assertThirdPartyIFrameCookieResult("2", true);
        } finally {
            webServer.shutdown();
        }
    }

    @Test
    @MediumTest
    @Feature({"AndroidWebView", "Privacy"})
    public void testThirdPartyCookiesArePerWebview() throws Throwable {
        TestWebServer webServer = TestWebServer.start();
        try {
            allowFirstPartyCookies();
            mCookieManager.removeAllCookies();
            Assert.assertFalse(mCookieManager.hasCookies());

            ThirdPartyCookiesTestHelper helperOne = new ThirdPartyCookiesTestHelper(webServer);
            ThirdPartyCookiesTestHelper helperTwo = new ThirdPartyCookiesTestHelper(webServer);

            blockThirdPartyCookies(helperOne.getAwContents());
            blockThirdPartyCookies(helperTwo.getAwContents());
            helperOne.assertThirdPartyIFrameCookieResult("1", false);
            helperTwo.assertThirdPartyIFrameCookieResult("2", false);

            allowThirdPartyCookies(helperTwo.getAwContents());
            Assert.assertFalse("helperOne's third-party cookie setting should be unaffected",
                    helperOne.getSettings().getAcceptThirdPartyCookies());
            helperOne.assertThirdPartyIFrameCookieResult("3", false);
            helperTwo.assertThirdPartyIFrameCookieResult("4", true);

            allowThirdPartyCookies(helperOne.getAwContents());
            Assert.assertTrue("helperTwo's third-party cookie setting shoudl be unaffected",
                    helperTwo.getSettings().getAcceptThirdPartyCookies());
            helperOne.assertThirdPartyIFrameCookieResult("5", true);
            helperTwo.assertThirdPartyIFrameCookieResult("6", true);

            blockThirdPartyCookies(helperTwo.getAwContents());
            Assert.assertTrue("helperOne's third-party cookie setting should be unaffected",
                    helperOne.getSettings().getAcceptThirdPartyCookies());
            helperOne.assertThirdPartyIFrameCookieResult("7", true);
            helperTwo.assertThirdPartyIFrameCookieResult("8", false);
        } finally {
            webServer.shutdown();
        }
    }

    @Test
    @MediumTest
    @Feature({"AndroidWebView", "Privacy"})
    public void testAcceptFileSchemeCookies() throws Throwable {
        mCookieManager.setAcceptFileSchemeCookies(true);
        Assert.assertTrue("allowFileSchemeCookies() should return true after "
                        + "setAcceptFileSchemeCookies(true)",
                mCookieManager.allowFileSchemeCookies());
        mAwContents.getSettings().setAllowFileAccess(true);

        mAwContents.getSettings().setAcceptThirdPartyCookies(true);
        Assert.assertTrue(fileURLCanSetCookie("1"));
        mAwContents.getSettings().setAcceptThirdPartyCookies(false);
        Assert.assertTrue(fileURLCanSetCookie("2"));
    }

    @Test
    @MediumTest
    @Feature({"AndroidWebView", "Privacy"})
    public void testRejectFileSchemeCookies() throws Throwable {
        mCookieManager.setAcceptFileSchemeCookies(false);
        Assert.assertFalse("allowFileSchemeCookies() should return false after "
                        + "setAcceptFileSchemeCookies(false)",
                mCookieManager.allowFileSchemeCookies());
        mAwContents.getSettings().setAllowFileAccess(true);

        mAwContents.getSettings().setAcceptThirdPartyCookies(true);
        Assert.assertFalse(fileURLCanSetCookie("3"));
        mAwContents.getSettings().setAcceptThirdPartyCookies(false);
        Assert.assertFalse(fileURLCanSetCookie("4"));
    }

    @Test
    @MediumTest
    @Feature({"AndroidWebView", "Privacy"})
    public void testInvokeAcceptFileSchemeCookiesTooLate() throws Throwable {
        // AwCookieManager only respects calls to setAcceptFileSchemeCookies() which happen *before*
        // the underlying cookie store is first used. Here we call into the cookie store with dummy
        // values to trigger this case, so we can test the CookieManager's observable state
        // (mainly, that allowFileSchemeCookies() is consistent with the actual behavior of
        // rejecting/accepting file scheme cookies).
        mCookieManager.setCookie("https://www.any.url.will.work/", "any-key=any-value");

        // Now try to enable file scheme cookies.
        mCookieManager.setAcceptFileSchemeCookies(true);
        Assert.assertFalse("allowFileSchemeCookies() should return false if "
                        + "setAcceptFileSchemeCookies was called too late",
                mCookieManager.allowFileSchemeCookies());
        mAwContents.getSettings().setAllowFileAccess(true);

        mAwContents.getSettings().setAcceptThirdPartyCookies(true);
        Assert.assertFalse(fileURLCanSetCookie("5"));
        mAwContents.getSettings().setAcceptThirdPartyCookies(false);
        Assert.assertFalse(fileURLCanSetCookie("6"));
    }

    private boolean fileURLCanSetCookie(String suffix) throws Throwable {
        String value = "value" + suffix;
        String url = "file:///android_asset/cookie_test.html?value=" + value;
        mActivityTestRule.loadUrlSync(mAwContents, mContentsClient.getOnPageFinishedHelper(), url);
        String cookie = mCookieManager.getCookie(url);
        return cookie != null && cookie.contains("test=" + value);
    }

    class ThirdPartyCookiesTestHelper {
        protected final AwContents mAwContents;
        protected final TestAwContentsClient mContentsClient;
        protected final TestWebServer mWebServer;

        ThirdPartyCookiesTestHelper(TestWebServer webServer) throws Throwable {
            mWebServer = webServer;
            mContentsClient = new TestAwContentsClient();
            final AwTestContainerView containerView =
                    mActivityTestRule.createAwTestContainerViewOnMainSync(mContentsClient);
            mAwContents = containerView.getAwContents();
            mAwContents.getSettings().setJavaScriptEnabled(true);
        }

        AwContents getAwContents() {
            return mAwContents;
        }

        AwSettings getSettings() {
            return mAwContents.getSettings();
        }

        TestWebServer getWebServer() {
            return mWebServer;
        }

        void assertThirdPartyIFrameCookieResult(String suffix, boolean expectedResult)
                throws Throwable {
            String key = "test" + suffix;
            String value = "value" + suffix;
            String iframePath = "/iframe_" + suffix + ".html";
            String pagePath = "/content_" + suffix + ".html";

            // We create a script which tries to set a cookie on a third party.
            String cookieUrl = toThirdPartyUrl(
                    makeCookieScriptUrl(getWebServer(), iframePath, key, value));

            // Then we load it as an iframe.
            String url = makeIframeUrl(getWebServer(), pagePath, cookieUrl);
            mActivityTestRule.loadUrlSync(
                    mAwContents, mContentsClient.getOnPageFinishedHelper(), url);

            if (expectedResult) {
                assertHasCookies(cookieUrl);
                validateCookies(cookieUrl, key);
            } else {
                assertNoCookies(cookieUrl);
            }

            // Clear the cookies.
            clearCookies();
            Assert.assertFalse(mCookieManager.hasCookies());
        }
    }

    /**
     * Creates a response on the TestWebServer which attempts to set a cookie when fetched.
     * @param  webServer  the webServer on which to create the response
     * @param  path the path component of the url (e.g "/my_thing_with_iframe.html")
     * @param  url the url which which should appear as the src of the iframe.
     * @return  the url which gets the response
     */
    private String makeIframeUrl(TestWebServer webServer, String path, String url) {
        String responseStr = "<html><head><title>Content!</title></head>"
                + "<body><iframe src=" + url + "></iframe></body></html>";
        return webServer.setResponse(path, responseStr, null);
    }

    /**
     * Creates a response on the TestWebServer with a script that attempts to set a cookie.
     * @param  webServer  the webServer on which to create the response
     * @param  path the path component of the url (e.g "/cookie_test.html")
     * @param  key the key of the cookie
     * @param  value the value of the cookie
     * @return  the url which gets the response
     */
    private String makeCookieScriptUrl(TestWebServer webServer, String path, String key,
            String value) {
        String response = "<html><head></head><body>"
                + "<script>document.cookie = \"" + key + "=" + value + "\";</script></body></html>";
        return webServer.setResponse(path, response, null);
    }

    /**
     * Makes a url look as if it comes from a different host.
     * @param  url the url to fake.
     * @return  the resulting url after faking.
     */
    private String toThirdPartyUrl(String url) {
        return url.replace("localhost", "127.0.0.1");
    }

    private void setCookieOnUiThread(final String url, final String cookie,
            final Callback<Boolean> callback) throws Throwable {
        InstrumentationRegistry.getInstrumentation().runOnMainSync(
                () -> mCookieManager.setCookie(url, cookie, callback));
    }

    private void removeSessionCookiesOnUiThread(final Callback<Boolean> callback) throws Throwable {
        InstrumentationRegistry.getInstrumentation().runOnMainSync(
                () -> mCookieManager.removeSessionCookies(callback));
    }

    private void removeAllCookiesOnUiThread(final Callback<Boolean> callback) throws Throwable {
        InstrumentationRegistry.getInstrumentation().runOnMainSync(
                () -> mCookieManager.removeAllCookies(callback));
    }

    /**
     * Clears all cookies synchronously.
     */
    private void clearCookies() throws Throwable {
        CookieUtils.clearCookies(InstrumentationRegistry.getInstrumentation(), mCookieManager);
    }

    private void waitForCookie(final String url) throws Exception {
        AwActivityTestRule.pollInstrumentationThread(() -> mCookieManager.getCookie(url) != null);
    }

    private void validateCookies(String url, String... expectedCookieNames) {
        final String responseCookie = mCookieManager.getCookie(url);
        String[] cookies = responseCookie.split(";");
        // Convert to sets, since Set#equals() hooks in nicely with assertEquals()
        Set<String> foundCookieNamesSet = new HashSet<String>();
        for (String cookie : cookies) {
            foundCookieNamesSet.add(cookie.substring(0, cookie.indexOf("=")).trim());
        }
        Set<String> expectedCookieNamesSet =
                new HashSet<String>(Arrays.asList(expectedCookieNames));
        Assert.assertEquals("Found cookies list differs from expected list", expectedCookieNamesSet,
                foundCookieNamesSet);
    }

    private String makeExpiringCookie(String cookie, int secondsTillExpiry) {
        return makeExpiringCookieMs(cookie, secondsTillExpiry * 1000);
    }

    @SuppressWarnings("deprecation")
    private String makeExpiringCookieMs(String cookie, int millisecondsTillExpiry) {
        Date date = new Date();
        date.setTime(date.getTime() + millisecondsTillExpiry);
        return cookie + "; expires=" + date.toGMTString();
    }

    /**
     * Asserts there are no cookies set for the given URL. This makes no assertions about other
     * URLs.
     *
     * @param cookieUrl the URL for which we expect no cookies to be set.
     */
    private void assertNoCookies(final String cookieUrl) throws Exception {
        String msg = "Expected to not see cookies for '" + cookieUrl + "'";
        Assert.assertNull(msg, mCookieManager.getCookie(cookieUrl));
    }

    /**
     * Asserts there are no cookies set at all.
     */
    private void assertNoCookies() throws Exception {
        String msg = "Expected to CookieManager to have no cookies";
        Assert.assertFalse(msg, mCookieManager.hasCookies());
    }

    /**
     * Asserts there are cookies set for the given URL.
     *
     * @param cookieUrl the URL for which to check for cookies.
     */
    private void assertHasCookies(final String cookieUrl) throws Exception {
        String msg = "Expected CookieManager to have cookies for '" + cookieUrl
                + "' but it has no cookies";
        Assert.assertTrue(msg, mCookieManager.hasCookies());
        msg = "Expected getCookie to return non-null for '" + cookieUrl + "'";
        Assert.assertNotNull(msg, mCookieManager.getCookie(cookieUrl));
    }

    /**
     * Asserts the cookie key/value pair for a given URL. Note: {@code cookieKeyValuePair} must
     * exactly match the expected {@link AwCookieManager#getCookie()} output, which may return
     * multiple key-value pairs.
     *
     * @param cookieKeyValuePair the expected key/value pair.
     * @param cookieUrl the URL to check cookies for.
     */
    private void assertCookieEquals(final String cookieKeyValuePair, final String cookieUrl)
            throws Exception {
        assertHasCookies(cookieUrl);
        String msg = "Unexpected cookie key/value pair";
        Assert.assertEquals(msg, cookieKeyValuePair, mCookieManager.getCookie(cookieUrl));
    }

    /**
     * Allow third-party cookies for the given {@link AwContents}. This checks the return value of
     * {@link AwCookieManager#getAcceptThirdPartyCookies}. This also checks the value of {@link
     * AwCookieManager#acceptCookie}, since it doesn't make sense to turn on third-party cookies if
     * all cookies have been blocked.
     *
     * @param awContents the AwContents for which to allow third-party cookies.
     * @throws IllegalStateException if cookies are already blocked globally.
     */
    private void allowThirdPartyCookies(AwContents awContents) throws Exception {
        if (!mCookieManager.acceptCookie()) {
            throw new IllegalStateException("It doesn't make sense to allow third-party cookies if "
                    + "cookies have already been globally blocked.");
        }
        awContents.getSettings().setAcceptThirdPartyCookies(true);
        String msg = "getAcceptThirdPartyCookies() should return true after "
                + "setAcceptThirdPartyCookies(true)";
        Assert.assertTrue(msg, awContents.getSettings().getAcceptThirdPartyCookies());
    }

    /**
     * Block third-party cookies for the given {@link AwContents}. This checks the return value of
     * {@link AwCookieManager#getAcceptThirdPartyCookies}.
     *
     * @param awContents the AwContents for which to block third-party cookies.
     */
    private void blockThirdPartyCookies(AwContents awContents) throws Exception {
        awContents.getSettings().setAcceptThirdPartyCookies(false);
        String msg = "getAcceptThirdPartyCookies() should return false after "
                + "setAcceptThirdPartyCookies(false)";
        Assert.assertFalse(msg, awContents.getSettings().getAcceptThirdPartyCookies());
    }

    /**
     * Allow first-party cookies globally. This affects all {@link AwContents}, and this does not
     * affect the third-party cookie settings for any {@link AwContents}. This checks the return
     * value of {@link AwCookieManager#acceptCookie}.
     */
    private void allowFirstPartyCookies() throws Exception {
        mCookieManager.setAcceptCookie(true);
        String msg = "acceptCookie() should return true after setAcceptCookie(true)";
        Assert.assertTrue(msg, mCookieManager.acceptCookie());
    }

    /**
     * Block all cookies for all {@link AwContents}. This blocks both first-party and third-party
     * cookies. This checks the return value of {@link AwCookieManager#acceptCookie}.
     */
    private void blockAllCookies() throws Exception {
        mCookieManager.setAcceptCookie(false);
        String msg = "acceptCookie() should return false after setAcceptCookie(false)";
        Assert.assertFalse(msg, mCookieManager.acceptCookie());
    }
}
