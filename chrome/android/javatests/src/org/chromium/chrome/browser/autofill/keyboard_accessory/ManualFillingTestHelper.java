// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.autofill.keyboard_accessory;

import static android.support.test.espresso.Espresso.onView;
import static android.support.test.espresso.matcher.ViewMatchers.isAssignableFrom;
import static android.support.test.espresso.matcher.ViewMatchers.isDisplayed;
import static android.support.test.espresso.matcher.ViewMatchers.isRoot;

import static org.hamcrest.core.AllOf.allOf;

import static org.chromium.chrome.test.util.ViewUtils.VIEW_GONE;
import static org.chromium.chrome.test.util.ViewUtils.VIEW_INVISIBLE;
import static org.chromium.chrome.test.util.ViewUtils.VIEW_NULL;
import static org.chromium.chrome.test.util.ViewUtils.waitForView;
import static org.chromium.ui.base.LocalizationUtils.setRtlForTesting;

import android.app.Activity;
import android.support.design.widget.TabLayout;
import android.support.test.InstrumentationRegistry;
import android.support.test.espresso.PerformException;
import android.support.test.espresso.UiController;
import android.support.test.espresso.ViewAction;
import android.support.test.espresso.ViewInteraction;
import android.support.test.espresso.matcher.BoundedMatcher;
import android.support.v7.widget.RecyclerView;
import android.text.method.PasswordTransformationMethod;
import android.view.View;
import android.view.ViewGroup;
import android.widget.TextView;

import org.hamcrest.Description;
import org.hamcrest.Matcher;
import org.junit.Assert;

import org.chromium.base.task.PostTask;
import org.chromium.chrome.R;
import org.chromium.chrome.browser.ChromeTabbedActivity;
import org.chromium.chrome.browser.ChromeWindow;
import org.chromium.chrome.browser.autofill.AutofillTestHelper;
import org.chromium.chrome.browser.autofill.PersonalDataManager.AutofillProfile;
import org.chromium.chrome.browser.autofill.keyboard_accessory.bar_component.KeyboardAccessoryCoordinator;
import org.chromium.chrome.browser.autofill.keyboard_accessory.data.KeyboardAccessoryData;
import org.chromium.chrome.browser.autofill.keyboard_accessory.data.KeyboardAccessoryData.AccessorySheetData;
import org.chromium.chrome.browser.autofill.keyboard_accessory.data.PropertyProvider;
import org.chromium.chrome.test.ChromeTabbedActivityTestRule;
import org.chromium.components.autofill.AutofillDelegate;
import org.chromium.components.autofill.AutofillSuggestion;
import org.chromium.content_public.browser.ImeAdapter;
import org.chromium.content_public.browser.UiThreadTaskTraits;
import org.chromium.content_public.browser.WebContents;
import org.chromium.content_public.browser.test.util.Criteria;
import org.chromium.content_public.browser.test.util.CriteriaHelper;
import org.chromium.content_public.browser.test.util.DOMUtils;
import org.chromium.content_public.browser.test.util.TestInputMethodManagerWrapper;
import org.chromium.content_public.browser.test.util.TestThreadUtils;
import org.chromium.net.test.EmbeddedTestServer;
import org.chromium.ui.DropdownItem;
import org.chromium.ui.DropdownPopupWindowInterface;

import java.util.concurrent.ExecutionException;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicReference;

/**
 * Helpers in this class simplify interactions with the Keyboard Accessory and the sheet below it.
 */
public class ManualFillingTestHelper {
    private final static String PASSWORD_NODE_ID = "password_field";
    private final static String USERNAME_NODE_ID = "username_field";
    private final static String SUBMIT_NODE_ID = "input_submit_button";

    private final ChromeTabbedActivityTestRule mActivityTestRule;
    private final AtomicReference<WebContents> mWebContentsRef = new AtomicReference<>();
    private TestInputMethodManagerWrapper mInputMethodManagerWrapper;
    private PropertyProvider<AccessorySheetData> mSheetSuggestionsProvider =
            new PropertyProvider<>();

    private EmbeddedTestServer mEmbeddedTestServer;

    public FakeKeyboard getKeyboard() {
        return (FakeKeyboard) mActivityTestRule.getKeyboardDelegate();
    }

    public ManualFillingTestHelper(ChromeTabbedActivityTestRule activityTestRule) {
        mActivityTestRule = activityTestRule;
    }

    public void loadTestPage(boolean isRtl) throws InterruptedException {
        loadTestPage("/chrome/test/data/password/password_form.html", isRtl);
    }

    public void loadTestPage(String url, boolean isRtl) throws InterruptedException {
        loadTestPage(url, isRtl, false);
    }

    public void loadTestPage(String url, boolean isRtl, boolean waitForNode)
            throws InterruptedException {
        mEmbeddedTestServer = EmbeddedTestServer.createAndStartServer(
                InstrumentationRegistry.getInstrumentation().getContext());
        ChromeWindow.setKeyboardVisibilityDelegateFactory(FakeKeyboard::new);
        mActivityTestRule.startMainActivityWithURL(mEmbeddedTestServer.getURL(url));
        setRtlForTesting(isRtl);
        TestThreadUtils.runOnUiThreadBlocking(() -> {
            ChromeTabbedActivity activity = mActivityTestRule.getActivity();
            mWebContentsRef.set(activity.getActivityTab().getWebContents());
            activity.getManualFillingController()
                    .getMediatorForTesting()
                    .setInsetObserverViewSupplier(
                            ()
                                    -> getKeyboard().createInsetObserver(
                                            activity.getInsetObserverView().getContext()));
            // The TestInputMethodManagerWrapper intercepts showSoftInput so that a keyboard is
            // never brought up.
            final ImeAdapter imeAdapter = ImeAdapter.fromWebContents(mWebContentsRef.get());
            mInputMethodManagerWrapper = TestInputMethodManagerWrapper.create(imeAdapter);
            imeAdapter.setInputMethodManagerWrapper(mInputMethodManagerWrapper);
            activity.getManualFillingController().registerPasswordProvider(
                    mSheetSuggestionsProvider);
        });
        if (waitForNode) DOMUtils.waitForNonZeroNodeBounds(mWebContentsRef.get(), PASSWORD_NODE_ID);
        cacheCredentials(new String[0], new String[0]); // This caches the empty state.
    }

    public void clear() {
        if (mEmbeddedTestServer != null) mEmbeddedTestServer.stopAndDestroyServer();
        ChromeWindow.resetKeyboardVisibilityDelegateFactory();
    }

    // --------------------------------------
    // Helpers interacting with the web page.
    // --------------------------------------

    public WebContents getWebContents() {
        return mWebContentsRef.get();
    }

    public void focusPasswordField() throws TimeoutException, InterruptedException {
        DOMUtils.focusNode(mActivityTestRule.getWebContents(), PASSWORD_NODE_ID);
        TestThreadUtils.runOnUiThreadBlocking(
                () -> { mActivityTestRule.getWebContents().scrollFocusedEditableNodeIntoView(); });
        getKeyboard().showKeyboard(mActivityTestRule.getActivity().getCurrentFocus());
    }

    public String getPasswordText() throws TimeoutException, InterruptedException {
        return DOMUtils.getNodeValue(mWebContentsRef.get(), PASSWORD_NODE_ID);
    }

    public void clickEmailField(boolean forceAccessory)
            throws TimeoutException, InterruptedException {
        // TODO(fhorschig): This should be |focusNode|. Change with autofill popup deprecation.
        DOMUtils.clickNode(mWebContentsRef.get(), USERNAME_NODE_ID);
        if (forceAccessory) {
            TestThreadUtils.runOnUiThreadBlocking(() -> {
                mActivityTestRule.getActivity()
                        .getManualFillingController()
                        .getMediatorForTesting()
                        .showWhenKeyboardIsVisible();
            });
        }
        getKeyboard().showKeyboard(mActivityTestRule.getActivity().getCurrentFocus());
    }

    public void clickNodeAndShowKeyboard(String node)
            throws TimeoutException, InterruptedException {
        DOMUtils.clickNode(mWebContentsRef.get(), node);
        getKeyboard().showKeyboard(mActivityTestRule.getActivity().getCurrentFocus());
    }

    /**
     * Although the submit button has no effect, it takes the focus from the input field and should
     * hide the keyboard.
     */
    public void clickSubmit() throws TimeoutException, InterruptedException {
        DOMUtils.clickNode(mWebContentsRef.get(), SUBMIT_NODE_ID);
        getKeyboard().hideAndroidSoftKeyboard(null);
    }

    // ---------------------------------
    // Helpers to wait for accessory UI.
    // ---------------------------------

    public void waitForKeyboardToDisappear() {
        CriteriaHelper.pollUiThread(() -> {
            Activity activity = mActivityTestRule.getActivity();
            return !getKeyboard().isAndroidSoftKeyboardShowing(
                    activity, activity.getCurrentFocus());
        });
    }

    public void waitForKeyboardAccessoryToDisappear() {
        CriteriaHelper.pollInstrumentationThread(() -> {
            KeyboardAccessoryCoordinator accessory = mActivityTestRule.getActivity()
                                                             .getManualFillingController()
                                                             .getKeyboardAccessory();
            return accessory != null && !accessory.isShown();
        });
        CriteriaHelper.pollUiThread(() -> {
            View accessory = mActivityTestRule.getActivity().findViewById(R.id.keyboard_accessory);
            return accessory != null && !accessory.isShown();
        });
    }

    public void waitForKeyboardAccessoryToBeShown() {
        CriteriaHelper.pollInstrumentationThread(() -> {
            KeyboardAccessoryCoordinator accessory = mActivityTestRule.getActivity()
                                                             .getManualFillingController()
                                                             .getKeyboardAccessory();
            return accessory != null && accessory.isShown();
        });
        CriteriaHelper.pollUiThread(() -> {
            View accessory = mActivityTestRule.getActivity().findViewById(R.id.keyboard_accessory);
            return accessory != null && accessory.isShown();
        });
    }

    public DropdownPopupWindowInterface waitForAutofillPopup(String filterInput) {
        final WebContents webContents = mActivityTestRule.getActivity().getCurrentWebContents();
        final ViewGroup view = webContents.getViewAndroidDelegate().getContainerView();

        // Wait for InputConnection to be ready and fill the filterInput. Then wait for the anchor.
        CriteriaHelper.pollUiThread(
                Criteria.equals(1, () -> mInputMethodManagerWrapper.getShowSoftInputCounter()));
        TestThreadUtils.runOnUiThreadBlocking(() -> {
            ImeAdapter.fromWebContents(webContents).setComposingTextForTest(filterInput, 4);
        });
        CriteriaHelper.pollUiThread(new Criteria("Autofill Popup anchor view was never added.") {
            @Override
            public boolean isSatisfied() {
                return view.findViewById(R.id.dropdown_popup_window) != null;
            }
        });
        View anchorView = view.findViewById(R.id.dropdown_popup_window);

        Assert.assertTrue(anchorView.getTag() instanceof DropdownPopupWindowInterface);
        final DropdownPopupWindowInterface popup =
                (DropdownPopupWindowInterface) anchorView.getTag();
        CriteriaHelper.pollUiThread(new Criteria("Autofill Popup view never showed!") {
            @Override
            public boolean isSatisfied() {
                // Wait until the popup is showing and onLayout() has happened.
                return popup.isShowing() && popup.getListView() != null
                        && popup.getListView().getHeight() != 0;
            }
        });

        return popup;
    }

    // ----------------------------------
    // Helpers to set up the native side.
    // ----------------------------------

    /**
     * Calls cacheCredentials with two simple credentials.
     * @see ManualFillingTestHelper#cacheCredentials(String, String)
     */
    public void cacheTestCredentials() {
        cacheCredentials(new String[] {"mpark@gmail.com", "mayapark@googlemail.com"},
                new String[] {"TestPassword", "SomeReallyLongPassword"});
    }

    /**
     * @see ManualFillingTestHelper#cacheCredentials(String, String)
     * @param username A {@link String} to be used as display text for a username chip.
     * @param password A {@link String} to be used as display text for a password chip.
     */
    public void cacheCredentials(String username, String password) {
        cacheCredentials(new String[] {username}, new String[] {password});
    }

    /**
     * Creates credential pairs from these strings and writes them into the cache of the native
     * controller. The controller will only refresh this cache on page load.
     * @param usernames {@link String}s to be used as display text for username chips.
     * @param passwords {@link String}s to be used as display text for password chips.
     */
    public void cacheCredentials(String[] usernames, String[] passwords) {
        TestThreadUtils.runOnUiThreadBlocking(() -> {
            ManualFillingBridge.cachePasswordSheetData(
                    mActivityTestRule.getWebContents(), usernames, passwords);
        });
    }

    public static void createAutofillTestProfiles()
            throws InterruptedException, ExecutionException, TimeoutException {
        new AutofillTestHelper().setProfile(new AutofillProfile("", "https://www.example.com",
                "Johnathan Smithonian-Jackson", "Acme Inc", "1 Main\nApt A", "CA", "San Francisco",
                "", "94102", "", "US", "(415) 888-9999", "john.sj@acme-mail.inc", "en"));
        new AutofillTestHelper().setProfile(new AutofillProfile("", "https://www.example.com",
                "Jane Erika Donovanova", "Acme Inc", "1 Main\nApt A", "CA", "San Francisco", "",
                "94102", "", "US", "(415) 999-0000", "donovanova.j@acme-mail.inc", "en"));
        new AutofillTestHelper().setProfile(new AutofillProfile("", "https://www.example.com",
                "Marcus McSpartangregor", "Acme Inc", "1 Main\nApt A", "CA", "San Francisco", "",
                "94102", "", "US", "(415) 999-0000", "marc@acme-mail.inc", "en"));
    }

    // --------------------------------------------------
    // Generic helpers to match, check or wait for views.
    // TODO(fhorschig): Consider Moving to ViewUtils.
    // --------------------------------------------------

    /**
     * Use in a |onView().perform| action to select the tab at |tabIndex| for the found tab layout.
     * @param tabIndex The index to be selected.
     * @return The action executed by |perform|.
     */
    static public ViewAction selectTabAtPosition(int tabIndex) {
        return new ViewAction() {
            @Override
            public Matcher<View> getConstraints() {
                return allOf(isDisplayed(), isAssignableFrom(TabLayout.class));
            }

            @Override
            public String getDescription() {
                return "with tab at index " + tabIndex;
            }

            @Override
            public void perform(UiController uiController, View view) {
                TabLayout tabLayout = (TabLayout) view;
                if (tabLayout.getTabAt(tabIndex) == null) {
                    throw new PerformException.Builder()
                            .withCause(new Throwable("No tab at index " + tabIndex))
                            .build();
                }
                PostTask.runOrPostTask(
                        UiThreadTaskTraits.DEFAULT, () -> tabLayout.getTabAt(tabIndex).select());
            }
        };
    }

    /**
     * Use in a |onView().perform| action to scroll to the end of a {@link RecyclerView}.
     * @return The action executed by |perform|.
     */
    static public ViewAction scrollToLastElement() {
        return new ViewAction() {
            @Override
            public Matcher<View> getConstraints() {
                return allOf(isDisplayed(), isAssignableFrom(RecyclerView.class));
            }

            @Override
            public String getDescription() {
                return "scrolling to end of view";
            }

            @Override
            public void perform(UiController uiController, View view) {
                RecyclerView recyclerView = (RecyclerView) view;
                int itemCount = recyclerView.getAdapter().getItemCount();
                if (itemCount <= 0) {
                    throw new PerformException.Builder()
                            .withCause(new Throwable("RecyclerView has no items."))
                            .build();
                }
                recyclerView.scrollToPosition(itemCount - 1);
            }
        };
    }

    /**
     * Matches any {@link TextView} which applies a {@link PasswordTransformationMethod}.
     * @return The matcher checking the transformation method.
     */
    public static Matcher<View> isTransformed() {
        return new BoundedMatcher<View, TextView>(TextView.class) {
            @Override
            public boolean matchesSafely(TextView textView) {
                return textView.getTransformationMethod() instanceof PasswordTransformationMethod;
            }

            @Override
            public void describeTo(Description description) {
                description.appendText("is a transformed password.");
            }
        };
    }

    /**
     * Use like {@link android.support.test.espresso.Espresso#onView}. It waits for a view matching
     * the given |matcher| to be displayed and allows to chain checks/performs on the result.
     * @param matcher The matcher matching exactly the view that is expected to be displayed.
     * @return An interaction on the view matching |matcher.
     */
    public static ViewInteraction whenDisplayed(Matcher<View> matcher) {
        onView(isRoot()).check((r, e) -> waitForView((ViewGroup) r, allOf(matcher, isDisplayed())));
        return onView(matcher);
    }

    public static void waitToBeHidden(Matcher<View> matcher) {
        onView(isRoot()).check((r, e) -> {
            waitForView((ViewGroup) r, matcher, VIEW_INVISIBLE | VIEW_NULL | VIEW_GONE);
        });
    }

    // --------------------------------------------
    // Helpers that force override the native side.
    // TODO(fhorschig): Search alternatives.
    // --------------------------------------------

    public void addGenerationButton() {
        PropertyProvider<KeyboardAccessoryData.Action[]> generationActionProvider =
                new PropertyProvider<>(AccessoryAction.GENERATE_PASSWORD_AUTOMATIC);
        mActivityTestRule.getActivity().getManualFillingController().registerActionProvider(
                generationActionProvider);
        TestThreadUtils.runOnUiThreadBlocking(() -> {
            generationActionProvider.notifyObservers(new KeyboardAccessoryData.Action[] {
                    new KeyboardAccessoryData.Action("Generate Password",
                            AccessoryAction.GENERATE_PASSWORD_AUTOMATIC, result -> {})});
        });
    }

    public void addAutofillChips() {
        PropertyProvider<AutofillSuggestion[]> suggestionProvider =
                new PropertyProvider<>(AccessoryAction.AUTOFILL_SUGGESTION);
        mActivityTestRule.getActivity()
                .getManualFillingController()
                .getKeyboardAccessory()
                .registerAutofillProvider(suggestionProvider, new AutofillDelegate() {
                    @Override
                    public void dismissed() {}
                    @Override
                    public void suggestionSelected(int listIndex) {}
                    @Override
                    public void deleteSuggestion(int listIndex) {}
                    @Override
                    public void accessibilityFocusCleared() {}
                });
        TestThreadUtils.runOnUiThreadBlocking(() -> {
            suggestionProvider.notifyObservers(new AutofillSuggestion[] {
                    new AutofillSuggestion("Johnathan", "Smithonian-Jackson", DropdownItem.NO_ICON,
                            false, 0, false, false, false),
                    new AutofillSuggestion("Jane Erika", "Donovanova", DropdownItem.NO_ICON, false,
                            1, false, false, false),
                    new AutofillSuggestion("Marcus", "McSpartangregor", DropdownItem.NO_ICON, false,
                            2, false, false, false)});
        });
    }
}
